/*
 * qemu_monitor.c: interaction with QEMU monitor console
 *
 * Copyright (C) 2006-2014 Red Hat, Inc.
 * Copyright (C) 2006 Daniel P. Berrange
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

#include "qemu_monitor.h"
#include "qemu_monitor_text.h"
#include "qemu_monitor_json.h"
#include "qemu_domain.h"
#include "qemu_process.h"
#include "virerror.h"
#include "viralloc.h"
#include "virlog.h"
#include "virfile.h"
#include "virprocess.h"
#include "virobject.h"
#include "virprobe.h"
#include "virstring.h"

#ifdef WITH_DTRACE_PROBES
# include "libvirt_qemu_probes.h"
#endif

#define VIR_FROM_THIS VIR_FROM_QEMU

VIR_LOG_INIT("qemu.qemu_monitor");

#define DEBUG_IO 0
#define DEBUG_RAW_IO 0

struct _qemuMonitor {
    virObjectLockable parent;

    virCond notify;

    int fd;
    int watch;
    int hasSendFD;

    virDomainObjPtr vm;

    qemuMonitorCallbacksPtr cb;
    void *callbackOpaque;

    /* If there's a command being processed this will be
     * non-NULL */
    qemuMonitorMessagePtr msg;

    /* Buffer incoming data ready for Text/QMP monitor
     * code to process & find message boundaries */
    size_t bufferOffset;
    size_t bufferLength;
    char *buffer;

    /* If anything went wrong, this will be fed back
     * the next monitor msg */
    virError lastError;

    int nextSerial;

    bool json;
    bool waitGreeting;

    /* cache of query-command-line-options results */
    virJSONValuePtr options;

    /* If found, path to the virtio memballoon driver */
    char *balloonpath;
    bool ballooninit;

    /* Log file fd of the qemu process to dig for usable info */
    int logfd;
};

static virClassPtr qemuMonitorClass;
static void qemuMonitorDispose(void *obj);

static int qemuMonitorOnceInit(void)
{
    if (!(qemuMonitorClass = virClassNew(virClassForObjectLockable(),
                                         "qemuMonitor",
                                         sizeof(qemuMonitor),
                                         qemuMonitorDispose)))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(qemuMonitor)


VIR_ENUM_IMPL(qemuMonitorMigrationStatus,
              QEMU_MONITOR_MIGRATION_STATUS_LAST,
              "inactive", "active", "completed", "failed", "cancelled", "setup")

VIR_ENUM_IMPL(qemuMonitorMigrationCaps,
              QEMU_MONITOR_MIGRATION_CAPS_LAST,
              "xbzrle", "auto-converge", "rdma-pin-all")

VIR_ENUM_IMPL(qemuMonitorVMStatus,
              QEMU_MONITOR_VM_STATUS_LAST,
              "debug", "inmigrate", "internal-error", "io-error", "paused",
              "postmigrate", "prelaunch", "finish-migrate", "restore-vm",
              "running", "save-vm", "shutdown", "watchdog", "guest-panicked")

typedef enum {
    QEMU_MONITOR_BLOCK_IO_STATUS_OK,
    QEMU_MONITOR_BLOCK_IO_STATUS_FAILED,
    QEMU_MONITOR_BLOCK_IO_STATUS_NOSPACE,

    QEMU_MONITOR_BLOCK_IO_STATUS_LAST
} qemuMonitorBlockIOStatus;

VIR_ENUM_DECL(qemuMonitorBlockIOStatus)

VIR_ENUM_IMPL(qemuMonitorBlockIOStatus,
              QEMU_MONITOR_BLOCK_IO_STATUS_LAST,
              "ok", "failed", "nospace")

char *qemuMonitorEscapeArg(const char *in)
{
    int len = 0;
    size_t i, j;
    char *out;

    /* To pass through the QEMU monitor, we need to use escape
       sequences: \r, \n, \", \\
    */

    for (i = 0; in[i] != '\0'; i++) {
        switch (in[i]) {
        case '\r':
        case '\n':
        case '"':
        case '\\':
            len += 2;
            break;
        default:
            len += 1;
            break;
        }
    }

    if (VIR_ALLOC_N(out, len + 1) < 0)
        return NULL;

    for (i = j = 0; in[i] != '\0'; i++) {
        switch (in[i]) {
        case '\r':
            out[j++] = '\\';
            out[j++] = 'r';
            break;
        case '\n':
            out[j++] = '\\';
            out[j++] = 'n';
            break;
        case '"':
        case '\\':
            out[j++] = '\\';
            out[j++] = in[i];
            break;
        default:
            out[j++] = in[i];
            break;
        }
    }
    out[j] = '\0';

    return out;
}

char *qemuMonitorUnescapeArg(const char *in)
{
    size_t i, j;
    char *out;
    int len = strlen(in);
    char next;

    if (VIR_ALLOC_N(out, len + 1) < 0)
        return NULL;

    for (i = j = 0; i < len; ++i) {
        next = in[i];
        if (in[i] == '\\') {
            ++i;
            switch (in[i]) {
            case 'r':
                next = '\r';
                break;
            case 'n':
                next = '\n';
                break;
            case '"':
            case '\\':
                next = in[i];
                break;
            default:
                /* invalid input (including trailing '\' at end of in) */
                VIR_FREE(out);
                return NULL;
            }
        }
        out[j++] = next;
    }
    out[j] = '\0';

    return out;
}

#if DEBUG_RAW_IO
# include <c-ctype.h>
static char * qemuMonitorEscapeNonPrintable(const char *text)
{
    size_t i;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    for (i = 0; text[i] != '\0'; i++) {
        if (c_isprint(text[i]) ||
            text[i] == '\n' ||
            (text[i] == '\r' && text[i + 1] == '\n'))
            virBufferAddChar(&buf, text[i]);
        else
            virBufferAsprintf(&buf, "0x%02x", text[i]);
    }
    return virBufferContentAndReset(&buf);
}
#endif

static void qemuMonitorDispose(void *obj)
{
    qemuMonitorPtr mon = obj;

    VIR_DEBUG("mon=%p", mon);
    if (mon->cb && mon->cb->destroy)
        (mon->cb->destroy)(mon, mon->vm, mon->callbackOpaque);
    virObjectUnref(mon->vm);

    virResetError(&mon->lastError);
    virCondDestroy(&mon->notify);
    VIR_FREE(mon->buffer);
    virJSONValueFree(mon->options);
    VIR_FREE(mon->balloonpath);
    VIR_FORCE_CLOSE(mon->logfd);
}


static int
qemuMonitorOpenUnix(const char *monitor, pid_t cpid)
{
    struct sockaddr_un addr;
    int monfd;
    int timeout = 30; /* In seconds */
    int ret;
    size_t i = 0;

    if ((monfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        virReportSystemError(errno,
                             "%s", _("failed to create socket"));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (virStrcpyStatic(addr.sun_path, monitor) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Monitor path %s too big for destination"), monitor);
        goto error;
    }

    do {
        ret = connect(monfd, (struct sockaddr *) &addr, sizeof(addr));

        if (ret == 0)
            break;

        if ((errno == ENOENT || errno == ECONNREFUSED) &&
            (!cpid || virProcessKill(cpid, 0) == 0)) {
            /* ENOENT       : Socket may not have shown up yet
             * ECONNREFUSED : Leftover socket hasn't been removed yet */
            continue;
        }

        virReportSystemError(errno, "%s",
                             _("failed to connect to monitor socket"));
        goto error;

    } while ((++i <= timeout*5) && (usleep(.2 * 1000000) <= 0));

    if (ret != 0) {
        virReportSystemError(errno, "%s",
                             _("monitor socket did not show up"));
        goto error;
    }

    return monfd;

 error:
    VIR_FORCE_CLOSE(monfd);
    return -1;
}

static int
qemuMonitorOpenPty(const char *monitor)
{
    int monfd;

    if ((monfd = open(monitor, O_RDWR)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unable to open monitor path %s"), monitor);
        return -1;
    }

    return monfd;
}


/* Get a possible error from qemu's log. This function closes the
 * corresponding log fd */
static char *
qemuMonitorGetErrorFromLog(qemuMonitorPtr mon)
{
    int len;
    char *logbuf = NULL;
    int orig_errno = errno;

    if (mon->logfd < 0)
        return NULL;

    if (VIR_ALLOC_N_QUIET(logbuf, 4096) < 0)
        goto error;

    if ((len = qemuProcessReadLog(mon->logfd, logbuf, 4096 - 1, 0, true)) <= 0)
        goto error;

 cleanup:
    errno = orig_errno;
    VIR_FORCE_CLOSE(mon->logfd);
    return logbuf;

 error:
    VIR_FREE(logbuf);
    goto cleanup;
}


/* This method processes data that has been received
 * from the monitor. Looking for async events and
 * replies/errors.
 */
static int
qemuMonitorIOProcess(qemuMonitorPtr mon)
{
    int len;
    qemuMonitorMessagePtr msg = NULL;

    /* See if there's a message & whether its ready for its reply
     * ie whether its completed writing all its data */
    if (mon->msg && mon->msg->txOffset == mon->msg->txLength)
        msg = mon->msg;

#if DEBUG_IO
# if DEBUG_RAW_IO
    char *str1 = qemuMonitorEscapeNonPrintable(msg ? msg->txBuffer : "");
    char *str2 = qemuMonitorEscapeNonPrintable(mon->buffer);
    VIR_ERROR(_("Process %d %p %p [[[[%s]]][[[%s]]]"), (int)mon->bufferOffset, mon->msg, msg, str1, str2);
    VIR_FREE(str1);
    VIR_FREE(str2);
# else
    VIR_DEBUG("Process %d", (int)mon->bufferOffset);
# endif
#endif

    PROBE(QEMU_MONITOR_IO_PROCESS,
          "mon=%p buf=%s len=%zu", mon, mon->buffer, mon->bufferOffset);

    if (mon->json)
        len = qemuMonitorJSONIOProcess(mon,
                                       mon->buffer, mon->bufferOffset,
                                       msg);
    else
        len = qemuMonitorTextIOProcess(mon,
                                       mon->buffer, mon->bufferOffset,
                                       msg);

    if (len < 0)
        return -1;

    if (len && mon->waitGreeting)
        mon->waitGreeting = false;

    if (len < mon->bufferOffset) {
        memmove(mon->buffer, mon->buffer + len, mon->bufferOffset - len);
        mon->bufferOffset -= len;
    } else {
        VIR_FREE(mon->buffer);
        mon->bufferOffset = mon->bufferLength = 0;
    }
#if DEBUG_IO
    VIR_DEBUG("Process done %d used %d", (int)mon->bufferOffset, len);
#endif
    if (msg && msg->finished)
        virCondBroadcast(&mon->notify);
    return len;
}


/* Call this function while holding the monitor lock. */
static int
qemuMonitorIOWriteWithFD(qemuMonitorPtr mon,
                         const char *data,
                         size_t len,
                         int fd)
{
    struct msghdr msg;
    struct iovec iov[1];
    int ret;
    char control[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cmsg;

    memset(&msg, 0, sizeof(msg));
    memset(control, 0, sizeof(control));

    iov[0].iov_base = (void *)data;
    iov[0].iov_len = len;

    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    cmsg = CMSG_FIRSTHDR(&msg);
    /* Some static analyzers, like clang 2.6-0.6.pre2, fail to see
       that our use of CMSG_FIRSTHDR will not return NULL.  */
    sa_assert(cmsg);
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    do {
        ret = sendmsg(mon->fd, &msg, 0);
    } while (ret < 0 && errno == EINTR);

    return ret;
}

/*
 * Called when the monitor is able to write data
 * Call this function while holding the monitor lock.
 */
static int
qemuMonitorIOWrite(qemuMonitorPtr mon)
{
    int done;
    char *buf;
    size_t len;

    /* If no active message, or fully transmitted, the no-op */
    if (!mon->msg || mon->msg->txOffset == mon->msg->txLength)
        return 0;

    if (mon->msg->txFD != -1 && !mon->hasSendFD) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Monitor does not support sending of file descriptors"));
        return -1;
    }

    buf = mon->msg->txBuffer + mon->msg->txOffset;
    len = mon->msg->txLength - mon->msg->txOffset;
    if (mon->msg->txFD == -1)
        done = write(mon->fd, buf, len);
    else
        done = qemuMonitorIOWriteWithFD(mon, buf, len, mon->msg->txFD);

    PROBE(QEMU_MONITOR_IO_WRITE,
          "mon=%p buf=%s len=%zu ret=%d errno=%d",
          mon, buf, len, done, errno);

    if (mon->msg->txFD != -1) {
        PROBE(QEMU_MONITOR_IO_SEND_FD,
              "mon=%p fd=%d ret=%d errno=%d",
              mon, mon->msg->txFD, done, errno);
    }

    if (done < 0) {
        if (errno == EAGAIN)
            return 0;

        virReportSystemError(errno, "%s",
                             _("Unable to write to monitor"));
        return -1;
    }
    mon->msg->txOffset += done;
    return done;
}

/*
 * Called when the monitor has incoming data to read
 * Call this function while holding the monitor lock.
 *
 * Returns -1 on error, or number of bytes read
 */
static int
qemuMonitorIORead(qemuMonitorPtr mon)
{
    size_t avail = mon->bufferLength - mon->bufferOffset;
    int ret = 0;

    if (avail < 1024) {
        if (VIR_REALLOC_N(mon->buffer,
                          mon->bufferLength + 1024) < 0)
            return -1;
        mon->bufferLength += 1024;
        avail += 1024;
    }

    /* Read as much as we can get into our buffer,
       until we block on EAGAIN, or hit EOF */
    while (avail > 1) {
        int got;
        got = read(mon->fd,
                   mon->buffer + mon->bufferOffset,
                   avail - 1);
        if (got < 0) {
            if (errno == EAGAIN)
                break;
            virReportSystemError(errno, "%s",
                                 _("Unable to read from monitor"));
            ret = -1;
            break;
        }
        if (got == 0)
            break;

        ret += got;
        avail -= got;
        mon->bufferOffset += got;
        mon->buffer[mon->bufferOffset] = '\0';
    }

#if DEBUG_IO
    VIR_DEBUG("Now read %d bytes of data", (int)mon->bufferOffset);
#endif

    return ret;
}


static void qemuMonitorUpdateWatch(qemuMonitorPtr mon)
{
    int events =
        VIR_EVENT_HANDLE_HANGUP |
        VIR_EVENT_HANDLE_ERROR;

    if (!mon->watch)
        return;

    if (mon->lastError.code == VIR_ERR_OK) {
        events |= VIR_EVENT_HANDLE_READABLE;

        if ((mon->msg && mon->msg->txOffset < mon->msg->txLength) &&
            !mon->waitGreeting)
            events |= VIR_EVENT_HANDLE_WRITABLE;
    }

    virEventUpdateHandle(mon->watch, events);
}


static void
qemuMonitorIO(int watch, int fd, int events, void *opaque)
{
    qemuMonitorPtr mon = opaque;
    bool error = false;
    bool eof = false;
    bool hangup = false;

    virObjectRef(mon);

    /* lock access to the monitor and protect fd */
    virObjectLock(mon);
#if DEBUG_IO
    VIR_DEBUG("Monitor %p I/O on watch %d fd %d events %d", mon, watch, fd, events);
#endif
    if (mon->fd == -1 || mon->watch == 0) {
        virObjectUnlock(mon);
        virObjectUnref(mon);
        return;
    }

    if (mon->fd != fd || mon->watch != watch) {
        if (events & (VIR_EVENT_HANDLE_HANGUP | VIR_EVENT_HANDLE_ERROR))
            eof = true;
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("event from unexpected fd %d!=%d / watch %d!=%d"),
                       mon->fd, fd, mon->watch, watch);
        error = true;
    } else if (mon->lastError.code != VIR_ERR_OK) {
        if (events & (VIR_EVENT_HANDLE_HANGUP | VIR_EVENT_HANDLE_ERROR))
            eof = true;
        error = true;
    } else {
        if (events & VIR_EVENT_HANDLE_WRITABLE) {
            if (qemuMonitorIOWrite(mon) < 0) {
                error = true;
                if (errno == ECONNRESET)
                    hangup = true;
            }
            events &= ~VIR_EVENT_HANDLE_WRITABLE;
        }

        if (!error &&
            events & VIR_EVENT_HANDLE_READABLE) {
            int got = qemuMonitorIORead(mon);
            events &= ~VIR_EVENT_HANDLE_READABLE;
            if (got < 0) {
                error = true;
                if (errno == ECONNRESET)
                    hangup = true;
            } else if (got == 0) {
                eof = true;
            } else {
                /* Ignore hangup/error events if we read some data, to
                 * give time for that data to be consumed */
                events = 0;

                if (qemuMonitorIOProcess(mon) < 0)
                    error = true;
            }
        }

        if (events & VIR_EVENT_HANDLE_HANGUP) {
            hangup = true;
            if (!error) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("End of file from monitor"));
                eof = true;
                events &= ~VIR_EVENT_HANDLE_HANGUP;
            }
        }

        if (!error && !eof &&
            events & VIR_EVENT_HANDLE_ERROR) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Invalid file descriptor while waiting for monitor"));
            eof = true;
            events &= ~VIR_EVENT_HANDLE_ERROR;
        }
        if (!error && events) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unhandled event %d for monitor fd %d"),
                           events, mon->fd);
            error = true;
        }
    }

    if (error || eof) {
        if (hangup) {
            /* Check if an error message from qemu is available and if so, use
             * it to overwrite the actual message. It's done only in early
             * startup phases or during incoming migration when the message
             * from qemu is certainly more interesting than a
             * "connection reset by peer" message.
             */
            char *qemuMessage;

            if ((qemuMessage = qemuMonitorGetErrorFromLog(mon))) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("early end of file from monitor: "
                                 "possible problem:\n%s"),
                               qemuMessage);
                virCopyLastError(&mon->lastError);
                virResetLastError();
            }

            VIR_FREE(qemuMessage);
        }

        if (mon->lastError.code != VIR_ERR_OK) {
            /* Already have an error, so clear any new error */
            virResetLastError();
        } else {
            virErrorPtr err = virGetLastError();
            if (!err)
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Error while processing monitor IO"));
            virCopyLastError(&mon->lastError);
            virResetLastError();
        }

        VIR_DEBUG("Error on monitor %s", NULLSTR(mon->lastError.message));
        /* If IO process resulted in an error & we have a message,
         * then wakeup that waiter */
        if (mon->msg && !mon->msg->finished) {
            mon->msg->finished = 1;
            virCondSignal(&mon->notify);
        }
    }

    qemuMonitorUpdateWatch(mon);

    /* We have to unlock to avoid deadlock against command thread,
     * but is this safe ?  I think it is, because the callback
     * will try to acquire the virDomainObjPtr mutex next */
    if (eof) {
        qemuMonitorEofNotifyCallback eofNotify = mon->cb->eofNotify;
        virDomainObjPtr vm = mon->vm;

        /* Make sure anyone waiting wakes up now */
        virCondSignal(&mon->notify);
        virObjectUnlock(mon);
        VIR_DEBUG("Triggering EOF callback");
        (eofNotify)(mon, vm, mon->callbackOpaque);
        virObjectUnref(mon);
    } else if (error) {
        qemuMonitorErrorNotifyCallback errorNotify = mon->cb->errorNotify;
        virDomainObjPtr vm = mon->vm;

        /* Make sure anyone waiting wakes up now */
        virCondSignal(&mon->notify);
        virObjectUnlock(mon);
        VIR_DEBUG("Triggering error callback");
        (errorNotify)(mon, vm, mon->callbackOpaque);
        virObjectUnref(mon);
    } else {
        virObjectUnlock(mon);
        virObjectUnref(mon);
    }
}


static qemuMonitorPtr
qemuMonitorOpenInternal(virDomainObjPtr vm,
                        int fd,
                        bool hasSendFD,
                        bool json,
                        qemuMonitorCallbacksPtr cb,
                        void *opaque)
{
    qemuMonitorPtr mon;

    if (!cb->eofNotify) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("EOF notify callback must be supplied"));
        return NULL;
    }
    if (!cb->errorNotify) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Error notify callback must be supplied"));
        return NULL;
    }

    if (qemuMonitorInitialize() < 0)
        return NULL;

    if (!(mon = virObjectLockableNew(qemuMonitorClass)))
        return NULL;

    mon->fd = -1;
    mon->logfd = -1;
    if (virCondInit(&mon->notify) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("cannot initialize monitor condition"));
        goto cleanup;
    }
    mon->fd = fd;
    mon->hasSendFD = hasSendFD;
    mon->vm = virObjectRef(vm);
    mon->json = json;
    if (json)
        mon->waitGreeting = true;
    mon->cb = cb;
    mon->callbackOpaque = opaque;

    if (virSetCloseExec(mon->fd) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Unable to set monitor close-on-exec flag"));
        goto cleanup;
    }
    if (virSetNonBlock(mon->fd) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Unable to put monitor into non-blocking mode"));
        goto cleanup;
    }


    virObjectLock(mon);
    virObjectRef(mon);
    if ((mon->watch = virEventAddHandle(mon->fd,
                                        VIR_EVENT_HANDLE_HANGUP |
                                        VIR_EVENT_HANDLE_ERROR |
                                        VIR_EVENT_HANDLE_READABLE,
                                        qemuMonitorIO,
                                        mon,
                                        virObjectFreeCallback)) < 0) {
        virObjectUnref(mon);
        virObjectUnlock(mon);
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("unable to register monitor events"));
        goto cleanup;
    }

    PROBE(QEMU_MONITOR_NEW,
          "mon=%p refs=%d fd=%d",
          mon, mon->parent.parent.u.s.refs, mon->fd);
    virObjectUnlock(mon);

    return mon;

 cleanup:
    /* We don't want the 'destroy' callback invoked during
     * cleanup from construction failure, because that can
     * give a double-unref on virDomainObjPtr in the caller,
     * so kill the callbacks now.
     */
    mon->cb = NULL;
    /* The caller owns 'fd' on failure */
    mon->fd = -1;
    qemuMonitorClose(mon);
    return NULL;
}

qemuMonitorPtr
qemuMonitorOpen(virDomainObjPtr vm,
                virDomainChrSourceDefPtr config,
                bool json,
                qemuMonitorCallbacksPtr cb,
                void *opaque)
{
    int fd;
    bool hasSendFD = false;
    qemuMonitorPtr ret;

    switch (config->type) {
    case VIR_DOMAIN_CHR_TYPE_UNIX:
        hasSendFD = true;
        if ((fd = qemuMonitorOpenUnix(config->data.nix.path, vm ? vm->pid : 0)) < 0)
            return NULL;
        break;

    case VIR_DOMAIN_CHR_TYPE_PTY:
        if ((fd = qemuMonitorOpenPty(config->data.file.path)) < 0)
            return NULL;
        break;

    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unable to handle monitor type: %s"),
                       virDomainChrTypeToString(config->type));
        return NULL;
    }

    ret = qemuMonitorOpenInternal(vm, fd, hasSendFD, json, cb, opaque);
    if (!ret)
        VIR_FORCE_CLOSE(fd);
    return ret;
}


qemuMonitorPtr qemuMonitorOpenFD(virDomainObjPtr vm,
                                 int sockfd,
                                 bool json,
                                 qemuMonitorCallbacksPtr cb,
                                 void *opaque)
{
    return qemuMonitorOpenInternal(vm, sockfd, true, json, cb, opaque);
}


void qemuMonitorClose(qemuMonitorPtr mon)
{
    if (!mon)
        return;

    virObjectLock(mon);
    PROBE(QEMU_MONITOR_CLOSE,
          "mon=%p refs=%d", mon, mon->parent.parent.u.s.refs);

    if (mon->fd >= 0) {
        if (mon->watch) {
            virEventRemoveHandle(mon->watch);
            mon->watch = 0;
        }
        VIR_FORCE_CLOSE(mon->fd);
    }

    /* In case another thread is waiting for its monitor command to be
     * processed, we need to wake it up with appropriate error set.
     */
    if (mon->msg) {
        if (mon->lastError.code == VIR_ERR_OK) {
            virErrorPtr err = virSaveLastError();

            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("Qemu monitor was closed"));
            virCopyLastError(&mon->lastError);
            if (err) {
                virSetError(err);
                virFreeError(err);
            } else {
                virResetLastError();
            }
        }
        mon->msg->finished = 1;
        virCondSignal(&mon->notify);
    }

    /* Propagate existing monitor error in case the current thread has no
     * error set.
     */
    if (mon->lastError.code != VIR_ERR_OK && !virGetLastError())
        virSetError(&mon->lastError);

    virObjectUnlock(mon);
    virObjectUnref(mon);
}


char *qemuMonitorNextCommandID(qemuMonitorPtr mon)
{
    char *id;

    ignore_value(virAsprintf(&id, "libvirt-%d", ++mon->nextSerial));
    return id;
}


int qemuMonitorSend(qemuMonitorPtr mon,
                    qemuMonitorMessagePtr msg)
{
    int ret = -1;

    /* Check whether qemu quit unexpectedly */
    if (mon->lastError.code != VIR_ERR_OK) {
        VIR_DEBUG("Attempt to send command while error is set %s",
                  NULLSTR(mon->lastError.message));
        virSetError(&mon->lastError);
        return -1;
    }

    mon->msg = msg;
    qemuMonitorUpdateWatch(mon);

    PROBE(QEMU_MONITOR_SEND_MSG,
          "mon=%p msg=%s fd=%d",
          mon, mon->msg->txBuffer, mon->msg->txFD);

    while (!mon->msg->finished) {
        if (virCondWait(&mon->notify, &mon->parent.lock) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Unable to wait on monitor condition"));
            goto cleanup;
        }
    }

    if (mon->lastError.code != VIR_ERR_OK) {
        VIR_DEBUG("Send command resulted in error %s",
                  NULLSTR(mon->lastError.message));
        virSetError(&mon->lastError);
        goto cleanup;
    }

    ret = 0;

 cleanup:
    mon->msg = NULL;
    qemuMonitorUpdateWatch(mon);

    return ret;
}


virJSONValuePtr
qemuMonitorGetOptions(qemuMonitorPtr mon)
{
    return mon->options;
}

void
qemuMonitorSetOptions(qemuMonitorPtr mon, virJSONValuePtr options)
{
    mon->options = options;
}


/**
 * Search the qom objects by it's known name.  The name is compared against
 * filed 'type' formatted as 'link<%name>'.
 *
 * This procedure will be call recursively until found or the qom-list is
 * exhausted.
 *
 * Returns:
 *
 *   0  - Found
 *  -1  - Error bail out
 *  -2  - Not found
 *
 * NOTE: This assumes we have already called qemuDomainObjEnterMonitor()
 */
static int
qemuMonitorFindObjectPath(qemuMonitorPtr mon,
                          const char *curpath,
                          const char *name,
                          char **path)
{
    ssize_t i, npaths = 0;
    int ret = -2;
    char *nextpath = NULL;
    char *type = NULL;
    qemuMonitorJSONListPathPtr *paths = NULL;

    if (virAsprintf(&type, "link<%s>", name) < 0)
        return -1;

    VIR_DEBUG("Searching for '%s' Object Path starting at '%s'", type, curpath);

    npaths = qemuMonitorJSONGetObjectListPaths(mon, curpath, &paths);
    if (npaths < 0)
        goto cleanup;

    for (i = 0; i < npaths && ret == -2; i++) {

        if (STREQ_NULLABLE(paths[i]->type, type)) {
            VIR_DEBUG("Path to '%s' is '%s/%s'", type, curpath, paths[i]->name);
            ret = 0;
            if (virAsprintf(path, "%s/%s", curpath, paths[i]->name) < 0) {
                *path = NULL;
                ret = -1;
            }
            goto cleanup;
        }

        /* Type entries that begin with "child<" are a branch that can be
         * traversed looking for more entries
         */
        if (paths[i]->type && STRPREFIX(paths[i]->type, "child<")) {
            if (virAsprintf(&nextpath, "%s/%s", curpath, paths[i]->name) < 0) {
                ret = -1;
                goto cleanup;
            }

            ret = qemuMonitorFindObjectPath(mon, nextpath, name, path);
        }
    }

 cleanup:
    for (i = 0; i < npaths; i++)
        qemuMonitorJSONListPathFree(paths[i]);
    VIR_FREE(paths);
    VIR_FREE(nextpath);
    VIR_FREE(type);
    return ret;
}


/**
 * Search the qom objects for the balloon driver object by it's known name
 * of "virtio-balloon-pci".  The entry for the driver will be found by using
 * function "qemuMonitorFindObjectPath".
 *
 * Once found, check the entry to ensure it has the correct property listed.
 * If it does not, then obtaining statistics from QEMU will not be possible.
 * This feature was added to QEMU 1.5.
 *
 * Returns:
 *
 *   0  - Found
 *  -1  - Not found or error
 *
 * NOTE: This assumes we have already called qemuDomainObjEnterMonitor()
 */
static int
qemuMonitorFindBalloonObjectPath(qemuMonitorPtr mon,
                                 const char *curpath)
{
    ssize_t i, nprops = 0;
    int ret = -1;
    char *path = NULL;
    qemuMonitorJSONListPathPtr *bprops = NULL;
    virDomainObjPtr vm = mon->vm;

    if (mon->balloonpath) {
        return 0;
    } else if (mon->ballooninit) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot determine balloon device path"));
        return -1;
    }

    /* Not supported */
    if (!vm->def->memballoon ||
        vm->def->memballoon->model != VIR_DOMAIN_MEMBALLOON_MODEL_VIRTIO) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Memory balloon model must be virtio to "
                         "get memballoon path"));
        return -1;
    }

    if (qemuMonitorFindObjectPath(mon, curpath, "virtio-balloon-pci", &path) < 0)
        return -1;

    nprops = qemuMonitorJSONGetObjectListPaths(mon, path, &bprops);
    if (nprops < 0)
        goto cleanup;

    for (i = 0; i < nprops; i++) {
        if (STREQ(bprops[i]->name, "guest-stats-polling-interval")) {
            VIR_DEBUG("Found Balloon Object Path %s", path);
            mon->balloonpath = path;
            path = NULL;
            ret = 0;
            goto cleanup;
        }
    }


    /* If we get here, we found the path, but not the property */
    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                   _("Property 'guest-stats-polling-interval' "
                     "not found on memory balloon driver."));

 cleanup:
    for (i = 0; i < nprops; i++)
        qemuMonitorJSONListPathFree(bprops[i]);
    VIR_FREE(bprops);
    VIR_FREE(path);
    return ret;
}


/**
 * To update video memory size in status XML we need to load correct values from
 * QEMU.  This is supported only with JSON monitor.
 *
 * Returns 0 on success, -1 on failure and sets proper error message.
 */
int
qemuMonitorUpdateVideoMemorySize(qemuMonitorPtr mon,
                                 virDomainVideoDefPtr video,
                                 const char *videoName)
{
    int ret = -1;
    char *path = NULL;

    if (mon->json) {
        ret = qemuMonitorFindObjectPath(mon, "/", videoName, &path);
        if (ret < 0) {
            if (ret == -2)
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Failed to find QOM Object path for "
                                 "device '%s'"), videoName);
            return -1;
        }

        ret = qemuMonitorJSONUpdateVideoMemorySize(mon, video, path);
        VIR_FREE(path);
        return ret;
    }

    return 0;
}


int qemuMonitorHMPCommandWithFd(qemuMonitorPtr mon,
                                const char *cmd,
                                int scm_fd,
                                char **reply)
{
    char *json_cmd = NULL;
    int ret = -1;

    if (mon->json) {
        /* hack to avoid complicating each call to text monitor functions */
        json_cmd = qemuMonitorUnescapeArg(cmd);
        if (!json_cmd) {
            VIR_DEBUG("Could not unescape command: %s", cmd);
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Unable to unescape command"));
            goto cleanup;
        }
        ret = qemuMonitorJSONHumanCommandWithFd(mon, json_cmd, scm_fd, reply);
    } else {
        ret = qemuMonitorTextCommandWithFd(mon, cmd, scm_fd, reply);
    }

 cleanup:
    VIR_FREE(json_cmd);
    return ret;
}

/* Ensure proper locking around callbacks.  */
#define QEMU_MONITOR_CALLBACK(mon, ret, callback, ...)          \
    do {                                                        \
        virObjectRef(mon);                                      \
        virObjectUnlock(mon);                                   \
        if ((mon)->cb && (mon)->cb->callback)                   \
            (ret) = (mon)->cb->callback(mon, __VA_ARGS__,       \
                                        (mon)->callbackOpaque); \
        virObjectLock(mon);                                     \
        virObjectUnref(mon);                                    \
    } while (0)

int qemuMonitorGetDiskSecret(qemuMonitorPtr mon,
                             virConnectPtr conn,
                             const char *path,
                             char **secret,
                             size_t *secretLen)
{
    int ret = -1;
    *secret = NULL;
    *secretLen = 0;

    QEMU_MONITOR_CALLBACK(mon, ret, diskSecretLookup, conn, mon->vm,
                          path, secret, secretLen);
    return ret;
}


int
qemuMonitorEmitEvent(qemuMonitorPtr mon, const char *event,
                     long long seconds, unsigned int micros,
                     const char *details)
{
    int ret = -1;
    VIR_DEBUG("mon=%p event=%s", mon, event);

    QEMU_MONITOR_CALLBACK(mon, ret, domainEvent, mon->vm, event, seconds,
                          micros, details);
    return ret;
}


int qemuMonitorEmitShutdown(qemuMonitorPtr mon)
{
    int ret = -1;
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, ret, domainShutdown, mon->vm);
    return ret;
}


int qemuMonitorEmitReset(qemuMonitorPtr mon)
{
    int ret = -1;
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, ret, domainReset, mon->vm);
    return ret;
}


int qemuMonitorEmitPowerdown(qemuMonitorPtr mon)
{
    int ret = -1;
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, ret, domainPowerdown, mon->vm);
    return ret;
}


int qemuMonitorEmitStop(qemuMonitorPtr mon)
{
    int ret = -1;
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, ret, domainStop, mon->vm);
    return ret;
}


int qemuMonitorEmitResume(qemuMonitorPtr mon)
{
    int ret = -1;
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, ret, domainResume, mon->vm);
    return ret;
}


int qemuMonitorEmitGuestPanic(qemuMonitorPtr mon)
{
    int ret = -1;
    VIR_DEBUG("mon=%p", mon);
    QEMU_MONITOR_CALLBACK(mon, ret, domainGuestPanic, mon->vm);
    return ret;
}


int qemuMonitorEmitRTCChange(qemuMonitorPtr mon, long long offset)
{
    int ret = -1;
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, ret, domainRTCChange, mon->vm, offset);
    return ret;
}


int qemuMonitorEmitWatchdog(qemuMonitorPtr mon, int action)
{
    int ret = -1;
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, ret, domainWatchdog, mon->vm, action);
    return ret;
}


int qemuMonitorEmitIOError(qemuMonitorPtr mon,
                           const char *diskAlias,
                           int action,
                           const char *reason)
{
    int ret = -1;
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, ret, domainIOError, mon->vm,
                          diskAlias, action, reason);
    return ret;
}


int qemuMonitorEmitGraphics(qemuMonitorPtr mon,
                            int phase,
                            int localFamily,
                            const char *localNode,
                            const char *localService,
                            int remoteFamily,
                            const char *remoteNode,
                            const char *remoteService,
                            const char *authScheme,
                            const char *x509dname,
                            const char *saslUsername)
{
    int ret = -1;
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, ret, domainGraphics, mon->vm, phase,
                          localFamily, localNode, localService,
                          remoteFamily, remoteNode, remoteService,
                          authScheme, x509dname, saslUsername);
    return ret;
}

int qemuMonitorEmitTrayChange(qemuMonitorPtr mon,
                              const char *devAlias,
                              int reason)
{
    int ret = -1;
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, ret, domainTrayChange, mon->vm,
                          devAlias, reason);

    return ret;
}

int qemuMonitorEmitPMWakeup(qemuMonitorPtr mon)
{
    int ret = -1;
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, ret, domainPMWakeup, mon->vm);

    return ret;
}

int qemuMonitorEmitPMSuspend(qemuMonitorPtr mon)
{
    int ret = -1;
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, ret, domainPMSuspend, mon->vm);

    return ret;
}

int qemuMonitorEmitPMSuspendDisk(qemuMonitorPtr mon)
{
    int ret = -1;
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, ret, domainPMSuspendDisk, mon->vm);

    return ret;
}

int qemuMonitorEmitBlockJob(qemuMonitorPtr mon,
                            const char *diskAlias,
                            int type,
                            int status)
{
    int ret = -1;
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, ret, domainBlockJob, mon->vm,
                          diskAlias, type, status);
    return ret;
}


int qemuMonitorEmitBalloonChange(qemuMonitorPtr mon,
                                 unsigned long long actual)
{
    int ret = -1;
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, ret, domainBalloonChange, mon->vm, actual);
    return ret;
}


int
qemuMonitorEmitDeviceDeleted(qemuMonitorPtr mon,
                             const char *devAlias)
{
    int ret = -1;
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, ret, domainDeviceDeleted, mon->vm, devAlias);

    return ret;
}


int
qemuMonitorEmitNicRxFilterChanged(qemuMonitorPtr mon,
                                  const char *devAlias)
{
    int ret = -1;
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, ret, domainNicRxFilterChanged, mon->vm, devAlias);

    return ret;
}


int
qemuMonitorEmitSerialChange(qemuMonitorPtr mon,
                            const char *devAlias,
                            bool connected)
{
    int ret = -1;
    VIR_DEBUG("mon=%p, devAlias='%s', connected=%d", mon, devAlias, connected);

    QEMU_MONITOR_CALLBACK(mon, ret, domainSerialChange, mon->vm, devAlias, connected);

    return ret;
}


int qemuMonitorSetCapabilities(qemuMonitorPtr mon)
{
    int ret;
    VIR_DEBUG("mon=%p", mon);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json) {
        ret = qemuMonitorJSONSetCapabilities(mon);
        if (ret < 0)
            goto cleanup;
    } else {
        ret = 0;
    }

 cleanup:
    return ret;
}


int
qemuMonitorStartCPUs(qemuMonitorPtr mon,
                     virConnectPtr conn)
{
    int ret;
    VIR_DEBUG("mon=%p", mon);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONStartCPUs(mon, conn);
    else
        ret = qemuMonitorTextStartCPUs(mon, conn);
    return ret;
}


int
qemuMonitorStopCPUs(qemuMonitorPtr mon)
{
    int ret;
    VIR_DEBUG("mon=%p", mon);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONStopCPUs(mon);
    else
        ret = qemuMonitorTextStopCPUs(mon);
    return ret;
}


int
qemuMonitorGetStatus(qemuMonitorPtr mon,
                     bool *running,
                     virDomainPausedReason *reason)
{
    int ret;
    VIR_DEBUG("mon=%p, running=%p, reason=%p", mon, running, reason);

    if (!mon || !running) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("both monitor and running must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONGetStatus(mon, running, reason);
    else
        ret = qemuMonitorTextGetStatus(mon, running, reason);
    return ret;
}


int qemuMonitorSystemPowerdown(qemuMonitorPtr mon)
{
    int ret;
    VIR_DEBUG("mon=%p", mon);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONSystemPowerdown(mon);
    else
        ret = qemuMonitorTextSystemPowerdown(mon);
    return ret;
}


int qemuMonitorSystemReset(qemuMonitorPtr mon)
{
    int ret;
    VIR_DEBUG("mon=%p", mon);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONSystemReset(mon);
    else
        ret = qemuMonitorTextSystemReset(mon);
    return ret;
}


int qemuMonitorGetCPUInfo(qemuMonitorPtr mon,
                          int **pids)
{
    int ret;
    VIR_DEBUG("mon=%p", mon);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONGetCPUInfo(mon, pids);
    else
        ret = qemuMonitorTextGetCPUInfo(mon, pids);
    return ret;
}

int qemuMonitorSetLink(qemuMonitorPtr mon,
                       const char *name,
                       virDomainNetInterfaceLinkState state)
{
    int ret;
    VIR_DEBUG("mon=%p, name=%p:%s, state=%u", mon, name, name, state);

    if (!mon || !name) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor || name must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONSetLink(mon, name, state);
    else
        ret = qemuMonitorTextSetLink(mon, name, state);
    return ret;
}

int qemuMonitorGetVirtType(qemuMonitorPtr mon,
                           int *virtType)
{
    int ret;
    VIR_DEBUG("mon=%p", mon);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONGetVirtType(mon, virtType);
    else
        ret = qemuMonitorTextGetVirtType(mon, virtType);
    return ret;
}


int qemuMonitorGetBalloonInfo(qemuMonitorPtr mon,
                              unsigned long long *currmem)
{
    int ret;
    VIR_DEBUG("mon=%p", mon);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONGetBalloonInfo(mon, currmem);
    else
        ret = qemuMonitorTextGetBalloonInfo(mon, currmem);
    return ret;
}


int qemuMonitorGetMemoryStats(qemuMonitorPtr mon,
                              virDomainMemoryStatPtr stats,
                              unsigned int nr_stats)
{
    int ret;
    VIR_DEBUG("mon=%p stats=%p nstats=%u", mon, stats, nr_stats);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json) {
        ignore_value(qemuMonitorFindBalloonObjectPath(mon, "/"));
        mon->ballooninit = true;
        ret = qemuMonitorJSONGetMemoryStats(mon, mon->balloonpath,
                                            stats, nr_stats);
    } else {
        ret = qemuMonitorTextGetMemoryStats(mon, stats, nr_stats);
    }
    return ret;
}

int qemuMonitorSetMemoryStatsPeriod(qemuMonitorPtr mon,
                                    int period)
{
    int ret = -1;
    VIR_DEBUG("mon=%p period=%d", mon, period);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    if (qemuMonitorFindBalloonObjectPath(mon, "/") == 0) {
        ret = qemuMonitorJSONSetMemoryStatsPeriod(mon, mon->balloonpath,
                                                  period);
    }
    mon->ballooninit = true;
    return ret;
}

int
qemuMonitorBlockIOStatusToError(const char *status)
{
    int st = qemuMonitorBlockIOStatusTypeFromString(status);

    if (st < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown block IO status: %s"), status);
        return -1;
    }

    switch ((qemuMonitorBlockIOStatus) st) {
    case QEMU_MONITOR_BLOCK_IO_STATUS_OK:
        return VIR_DOMAIN_DISK_ERROR_NONE;
    case QEMU_MONITOR_BLOCK_IO_STATUS_FAILED:
        return VIR_DOMAIN_DISK_ERROR_UNSPEC;
    case QEMU_MONITOR_BLOCK_IO_STATUS_NOSPACE:
        return VIR_DOMAIN_DISK_ERROR_NO_SPACE;

    /* unreachable */
    case QEMU_MONITOR_BLOCK_IO_STATUS_LAST:
        break;
    }
    return -1;
}

virHashTablePtr
qemuMonitorGetBlockInfo(qemuMonitorPtr mon)
{
    int ret;
    virHashTablePtr table;

    VIR_DEBUG("mon=%p", mon);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return NULL;
    }

    if (!(table = virHashCreate(32, virHashValueFree)))
        return NULL;

    if (mon->json)
        ret = qemuMonitorJSONGetBlockInfo(mon, table);
    else
        ret = qemuMonitorTextGetBlockInfo(mon, table);

    if (ret < 0) {
        virHashFree(table);
        return NULL;
    }

    return table;
}

struct qemuDomainDiskInfo *
qemuMonitorBlockInfoLookup(virHashTablePtr blockInfo,
                           const char *dev)
{
    struct qemuDomainDiskInfo *info;

    VIR_DEBUG("blockInfo=%p dev=%s", blockInfo, NULLSTR(dev));

    if (!(info = virHashLookup(blockInfo, dev))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("cannot find info for device '%s'"),
                       NULLSTR(dev));
    }

    return info;
}

int qemuMonitorGetBlockStatsInfo(qemuMonitorPtr mon,
                                 const char *dev_name,
                                 long long *rd_req,
                                 long long *rd_bytes,
                                 long long *rd_total_times,
                                 long long *wr_req,
                                 long long *wr_bytes,
                                 long long *wr_total_times,
                                 long long *flush_req,
                                 long long *flush_total_times,
                                 long long *errs)
{
    int ret;
    VIR_DEBUG("mon=%p dev=%s", mon, dev_name);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONGetBlockStatsInfo(mon, dev_name,
                                               rd_req, rd_bytes,
                                               rd_total_times,
                                               wr_req, wr_bytes,
                                               wr_total_times,
                                               flush_req,
                                               flush_total_times,
                                               errs);
    else
        ret = qemuMonitorTextGetBlockStatsInfo(mon, dev_name,
                                               rd_req, rd_bytes,
                                               rd_total_times,
                                               wr_req, wr_bytes,
                                               wr_total_times,
                                               flush_req,
                                               flush_total_times,
                                               errs);
    return ret;
}


/* Creates a hash table in 'ret_stats' with all block stats.
 * Returns <0 on error, 0 on success.
 */
int
qemuMonitorGetAllBlockStatsInfo(qemuMonitorPtr mon,
                                virHashTablePtr *ret_stats,
                                bool backingChain)
{
    VIR_DEBUG("mon=%p ret_stats=%p, backing=%d", mon, ret_stats, backingChain);

    if (!mon->json) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("unable to query all block stats with this QEMU"));
        return -1;
    }

    return qemuMonitorJSONGetAllBlockStatsInfo(mon, ret_stats, backingChain);
}


/* Updates "stats" to fill virtual and physical size of the image */
int
qemuMonitorBlockStatsUpdateCapacity(qemuMonitorPtr mon,
                                    virHashTablePtr stats,
                                    bool backingChain)
{
    VIR_DEBUG("mon=%p, stats=%p, backing=%d", mon, stats, backingChain);

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("block capacity/size info requires JSON monitor"));
        return -1;
    }

    return qemuMonitorJSONBlockStatsUpdateCapacity(mon, stats, backingChain);
}


/* Return 0 and update @nparams with the number of block stats
 * QEMU supports if success. Return -1 if failure.
 */
int qemuMonitorGetBlockStatsParamsNumber(qemuMonitorPtr mon,
                                         int *nparams)
{
    int ret;
    VIR_DEBUG("mon=%p nparams=%p", mon, nparams);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONGetBlockStatsParamsNumber(mon, nparams);
    else
        ret = qemuMonitorTextGetBlockStatsParamsNumber(mon, nparams);

    return ret;
}

int qemuMonitorGetBlockExtent(qemuMonitorPtr mon,
                              const char *dev_name,
                              unsigned long long *extent)
{
    int ret;
    VIR_DEBUG("mon=%p, dev_name=%s", mon, dev_name);

    if (mon->json)
        ret = qemuMonitorJSONGetBlockExtent(mon, dev_name, extent);
    else
        ret = qemuMonitorTextGetBlockExtent(mon, dev_name, extent);

    return ret;
}

int qemuMonitorBlockResize(qemuMonitorPtr mon,
                           const char *device,
                           unsigned long long size)
{
    int ret;
    VIR_DEBUG("mon=%p, device=%s size=%llu", mon, device, size);

    if (mon->json)
        ret = qemuMonitorJSONBlockResize(mon, device, size);
    else
        ret = qemuMonitorTextBlockResize(mon, device, size);

    return ret;
}

int qemuMonitorSetVNCPassword(qemuMonitorPtr mon,
                              const char *password)
{
    int ret;
    VIR_DEBUG("mon=%p, password=%p",
          mon, password);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!password)
        password = "";

    if (mon->json)
        ret = qemuMonitorJSONSetVNCPassword(mon, password);
    else
        ret = qemuMonitorTextSetVNCPassword(mon, password);
    return ret;
}

static const char* qemuMonitorTypeToProtocol(int type)
{
    switch (type) {
    case VIR_DOMAIN_GRAPHICS_TYPE_VNC:
        return "vnc";
    case VIR_DOMAIN_GRAPHICS_TYPE_SPICE:
        return "spice";
    default:
        virReportError(VIR_ERR_INVALID_ARG,
                       _("unsupported protocol type %s"),
                       virDomainGraphicsTypeToString(type));
        return NULL;
    }
}

/* Returns -2 if not supported with this monitor connection */
int qemuMonitorSetPassword(qemuMonitorPtr mon,
                           int type,
                           const char *password,
                           const char *action_if_connected)
{
    const char *protocol = qemuMonitorTypeToProtocol(type);
    int ret;

    if (!protocol)
        return -1;

    VIR_DEBUG("mon=%p, protocol=%s, password=%p, action_if_connected=%s",
          mon, protocol, password, action_if_connected);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!password)
        password = "";

    if (!action_if_connected)
        action_if_connected = "keep";

    if (mon->json)
        ret = qemuMonitorJSONSetPassword(mon, protocol, password, action_if_connected);
    else
        ret = qemuMonitorTextSetPassword(mon, protocol, password, action_if_connected);
    return ret;
}

int qemuMonitorExpirePassword(qemuMonitorPtr mon,
                              int type,
                              const char *expire_time)
{
    const char *protocol = qemuMonitorTypeToProtocol(type);
    int ret;

    if (!protocol)
        return -1;

    VIR_DEBUG("mon=%p, protocol=%s, expire_time=%s",
          mon, protocol, expire_time);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!expire_time)
        expire_time = "now";

    if (mon->json)
        ret = qemuMonitorJSONExpirePassword(mon, protocol, expire_time);
    else
        ret = qemuMonitorTextExpirePassword(mon, protocol, expire_time);
    return ret;
}

int qemuMonitorSetBalloon(qemuMonitorPtr mon,
                          unsigned long newmem)
{
    int ret;
    VIR_DEBUG("mon=%p newmem=%lu", mon, newmem);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONSetBalloon(mon, newmem);
    else
        ret = qemuMonitorTextSetBalloon(mon, newmem);
    return ret;
}


int qemuMonitorSetCPU(qemuMonitorPtr mon, int cpu, bool online)
{
    int ret;
    VIR_DEBUG("mon=%p cpu=%d online=%d", mon, cpu, online);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONSetCPU(mon, cpu, online);
    else
        ret = qemuMonitorTextSetCPU(mon, cpu, online);
    return ret;
}


int qemuMonitorEjectMedia(qemuMonitorPtr mon,
                          const char *dev_name,
                          bool force)
{
    int ret;
    VIR_DEBUG("mon=%p dev_name=%s force=%d", mon, dev_name, force);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONEjectMedia(mon, dev_name, force);
    else
        ret = qemuMonitorTextEjectMedia(mon, dev_name, force);
    return ret;
}


int qemuMonitorChangeMedia(qemuMonitorPtr mon,
                           const char *dev_name,
                           const char *newmedia,
                           const char *format)
{
    int ret;
    VIR_DEBUG("mon=%p dev_name=%s newmedia=%s format=%s",
          mon, dev_name, newmedia, format);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONChangeMedia(mon, dev_name, newmedia, format);
    else
        ret = qemuMonitorTextChangeMedia(mon, dev_name, newmedia, format);
    return ret;
}


int qemuMonitorSaveVirtualMemory(qemuMonitorPtr mon,
                                 unsigned long long offset,
                                 size_t length,
                                 const char *path)
{
    int ret;
    VIR_DEBUG("mon=%p offset=%llu length=%zu path=%s",
          mon, offset, length, path);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONSaveVirtualMemory(mon, offset, length, path);
    else
        ret = qemuMonitorTextSaveVirtualMemory(mon, offset, length, path);
    return ret;
}

int qemuMonitorSavePhysicalMemory(qemuMonitorPtr mon,
                                  unsigned long long offset,
                                  size_t length,
                                  const char *path)
{
    int ret;
    VIR_DEBUG("mon=%p offset=%llu length=%zu path=%s",
          mon, offset, length, path);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONSavePhysicalMemory(mon, offset, length, path);
    else
        ret = qemuMonitorTextSavePhysicalMemory(mon, offset, length, path);
    return ret;
}


int qemuMonitorSetMigrationSpeed(qemuMonitorPtr mon,
                                 unsigned long bandwidth)
{
    int ret;
    VIR_DEBUG("mon=%p bandwidth=%lu", mon, bandwidth);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (bandwidth > QEMU_DOMAIN_MIG_BANDWIDTH_MAX) {
        virReportError(VIR_ERR_OVERFLOW,
                       _("bandwidth must be less than %llu"),
                       QEMU_DOMAIN_MIG_BANDWIDTH_MAX + 1ULL);
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONSetMigrationSpeed(mon, bandwidth);
    else
        ret = qemuMonitorTextSetMigrationSpeed(mon, bandwidth);
    return ret;
}


int qemuMonitorSetMigrationDowntime(qemuMonitorPtr mon,
                                    unsigned long long downtime)
{
    int ret;
    VIR_DEBUG("mon=%p downtime=%llu", mon, downtime);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONSetMigrationDowntime(mon, downtime);
    else
        ret = qemuMonitorTextSetMigrationDowntime(mon, downtime);
    return ret;
}


int
qemuMonitorGetMigrationCacheSize(qemuMonitorPtr mon,
                                 unsigned long long *cacheSize)
{
    VIR_DEBUG("mon=%p cacheSize=%p", mon, cacheSize);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONGetMigrationCacheSize(mon, cacheSize);
}

int
qemuMonitorSetMigrationCacheSize(qemuMonitorPtr mon,
                                 unsigned long long cacheSize)
{
    VIR_DEBUG("mon=%p cacheSize=%llu", mon, cacheSize);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONSetMigrationCacheSize(mon, cacheSize);
}


int qemuMonitorGetMigrationStatus(qemuMonitorPtr mon,
                                  qemuMonitorMigrationStatusPtr status)
{
    int ret;
    VIR_DEBUG("mon=%p", mon);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONGetMigrationStatus(mon, status);
    else
        ret = qemuMonitorTextGetMigrationStatus(mon, status);
    return ret;
}


int qemuMonitorGetSpiceMigrationStatus(qemuMonitorPtr mon,
                                       bool *spice_migrated)
{
    int ret;
    VIR_DEBUG("mon=%p", mon);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json) {
        ret = qemuMonitorJSONGetSpiceMigrationStatus(mon, spice_migrated);
    } else {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return ret;
}


int qemuMonitorMigrateToFd(qemuMonitorPtr mon,
                           unsigned int flags,
                           int fd)
{
    int ret;
    VIR_DEBUG("mon=%p fd=%d flags=%x",
          mon, fd, flags);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (qemuMonitorSendFileHandle(mon, "migrate", fd) < 0)
        return -1;

    if (mon->json)
        ret = qemuMonitorJSONMigrate(mon, flags, "fd:migrate");
    else
        ret = qemuMonitorTextMigrate(mon, flags, "fd:migrate");

    if (ret < 0) {
        if (qemuMonitorCloseFileHandle(mon, "migrate") < 0)
            VIR_WARN("failed to close migration handle");
    }

    return ret;
}


int qemuMonitorMigrateToHost(qemuMonitorPtr mon,
                             unsigned int flags,
                             const char *protocol,
                             const char *hostname,
                             int port)
{
    int ret;
    char *uri = NULL;
    VIR_DEBUG("mon=%p hostname=%s port=%d flags=%x",
          mon, hostname, port, flags);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }


    if (virAsprintf(&uri, "%s:%s:%d", protocol, hostname, port) < 0)
        return -1;

    if (mon->json)
        ret = qemuMonitorJSONMigrate(mon, flags, uri);
    else
        ret = qemuMonitorTextMigrate(mon, flags, uri);

    VIR_FREE(uri);
    return ret;
}


int qemuMonitorMigrateToCommand(qemuMonitorPtr mon,
                                unsigned int flags,
                                const char * const *argv)
{
    char *argstr;
    char *dest = NULL;
    int ret = -1;
    VIR_DEBUG("mon=%p argv=%p flags=%x",
          mon, argv, flags);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    argstr = virArgvToString(argv);
    if (!argstr)
        goto cleanup;

    if (virAsprintf(&dest, "exec:%s", argstr) < 0)
        goto cleanup;

    if (mon->json)
        ret = qemuMonitorJSONMigrate(mon, flags, dest);
    else
        ret = qemuMonitorTextMigrate(mon, flags, dest);

 cleanup:
    VIR_FREE(argstr);
    VIR_FREE(dest);
    return ret;
}

int qemuMonitorMigrateToFile(qemuMonitorPtr mon,
                             unsigned int flags,
                             const char * const *argv,
                             const char *target,
                             unsigned long long offset)
{
    char *argstr;
    char *dest = NULL;
    int ret = -1;
    char *safe_target = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    VIR_DEBUG("mon=%p argv=%p target=%s offset=%llu flags=%x",
          mon, argv, target, offset, flags);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (offset % QEMU_MONITOR_MIGRATE_TO_FILE_BS) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("file offset must be a multiple of %llu"),
                       QEMU_MONITOR_MIGRATE_TO_FILE_BS);
        return -1;
    }

    argstr = virArgvToString(argv);
    if (!argstr)
        goto cleanup;

    /* Migrate to file */
    virBufferEscapeShell(&buf, target);
    if (virBufferCheckError(&buf) < 0)
        goto cleanup;
    safe_target = virBufferContentAndReset(&buf);

    /* Two dd processes, sharing the same stdout, are necessary to
     * allow starting at an alignment of 512, but without wasting
     * padding to get to the larger alignment useful for speed.  Use
     * <> redirection to avoid truncating a regular file.  */
    if (virAsprintf(&dest, "exec:" VIR_WRAPPER_SHELL_PREFIX "%s | "
                    "{ dd bs=%llu seek=%llu if=/dev/null && "
                    "dd ibs=%llu obs=%llu; } 1<>%s" VIR_WRAPPER_SHELL_SUFFIX,
                    argstr, QEMU_MONITOR_MIGRATE_TO_FILE_BS,
                    offset / QEMU_MONITOR_MIGRATE_TO_FILE_BS,
                    QEMU_MONITOR_MIGRATE_TO_FILE_TRANSFER_SIZE,
                    QEMU_MONITOR_MIGRATE_TO_FILE_TRANSFER_SIZE,
                    safe_target) < 0)
        goto cleanup;

    if (mon->json)
        ret = qemuMonitorJSONMigrate(mon, flags, dest);
    else
        ret = qemuMonitorTextMigrate(mon, flags, dest);

 cleanup:
    VIR_FREE(safe_target);
    VIR_FREE(argstr);
    VIR_FREE(dest);
    return ret;
}

int qemuMonitorMigrateToUnix(qemuMonitorPtr mon,
                             unsigned int flags,
                             const char *unixfile)
{
    char *dest = NULL;
    int ret = -1;
    VIR_DEBUG("mon=%p, unixfile=%s flags=%x",
          mon, unixfile, flags);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (virAsprintf(&dest, "unix:%s", unixfile) < 0)
        return -1;

    if (mon->json)
        ret = qemuMonitorJSONMigrate(mon, flags, dest);
    else
        ret = qemuMonitorTextMigrate(mon, flags, dest);

    VIR_FREE(dest);
    return ret;
}

int qemuMonitorMigrateCancel(qemuMonitorPtr mon)
{
    int ret;
    VIR_DEBUG("mon=%p", mon);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONMigrateCancel(mon);
    else
        ret = qemuMonitorTextMigrateCancel(mon);
    return ret;
}

/**
 * Returns 1 if @capability is supported, 0 if it's not, or -1 on error.
 */
int qemuMonitorGetDumpGuestMemoryCapability(qemuMonitorPtr mon,
                                            const char *capability)
{
    VIR_DEBUG("mon=%p capability=%s", mon, capability);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    /* No capability is supported without JSON monitor */
    if (!mon->json)
        return 0;

    return qemuMonitorJSONGetDumpGuestMemoryCapability(mon, capability);
}

int
qemuMonitorDumpToFd(qemuMonitorPtr mon, int fd, const char *dumpformat)
{
    int ret;
    VIR_DEBUG("mon=%p fd=%d dumpformat=%s", mon, fd, dumpformat);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        /* We don't have qemuMonitorTextDump(), so we should check mon->json
         * here.
         */
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("dump-guest-memory is not supported in text mode"));
        return -1;
    }

    if (qemuMonitorSendFileHandle(mon, "dump", fd) < 0)
        return -1;

    ret = qemuMonitorJSONDump(mon, "fd:dump", dumpformat);

    if (ret < 0) {
        if (qemuMonitorCloseFileHandle(mon, "dump") < 0)
            VIR_WARN("failed to close dumping handle");
    }

    return ret;
}

int qemuMonitorGraphicsRelocate(qemuMonitorPtr mon,
                                int type,
                                const char *hostname,
                                int port,
                                int tlsPort,
                                const char *tlsSubject)
{
    int ret;
    VIR_DEBUG("mon=%p type=%d hostname=%s port=%d tlsPort=%d tlsSubject=%s",
              mon, type, hostname, port, tlsPort, NULLSTR(tlsSubject));

    if (mon->json)
        ret = qemuMonitorJSONGraphicsRelocate(mon,
                                              type,
                                              hostname,
                                              port,
                                              tlsPort,
                                              tlsSubject);
    else
        ret = qemuMonitorTextGraphicsRelocate(mon,
                                              type,
                                              hostname,
                                              port,
                                              tlsPort,
                                              tlsSubject);

    return ret;
}


int qemuMonitorAddUSBDisk(qemuMonitorPtr mon,
                          const char *path)
{
    int ret;
    VIR_DEBUG("mon=%p path=%s", mon, path);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONAddUSBDisk(mon, path);
    else
        ret = qemuMonitorTextAddUSBDisk(mon, path);
    return ret;
}


int qemuMonitorAddUSBDeviceExact(qemuMonitorPtr mon,
                                 int bus,
                                 int dev)
{
    int ret;
    VIR_DEBUG("mon=%p bus=%d dev=%d", mon, bus, dev);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONAddUSBDeviceExact(mon, bus, dev);
    else
        ret = qemuMonitorTextAddUSBDeviceExact(mon, bus, dev);
    return ret;
}

int qemuMonitorAddUSBDeviceMatch(qemuMonitorPtr mon,
                                 int vendor,
                                 int product)
{
    int ret;
    VIR_DEBUG("mon=%p vendor=%d product=%d",
          mon, vendor, product);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONAddUSBDeviceMatch(mon, vendor, product);
    else
        ret = qemuMonitorTextAddUSBDeviceMatch(mon, vendor, product);
    return ret;
}


int qemuMonitorAddPCIHostDevice(qemuMonitorPtr mon,
                                virDevicePCIAddress *hostAddr,
                                virDevicePCIAddress *guestAddr)
{
    int ret;
    VIR_DEBUG("mon=%p domain=%d bus=%d slot=%d function=%d",
          mon,
          hostAddr->domain, hostAddr->bus, hostAddr->slot, hostAddr->function);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONAddPCIHostDevice(mon, hostAddr, guestAddr);
    else
        ret = qemuMonitorTextAddPCIHostDevice(mon, hostAddr, guestAddr);
    return ret;
}


int qemuMonitorAddPCIDisk(qemuMonitorPtr mon,
                          const char *path,
                          const char *bus,
                          virDevicePCIAddress *guestAddr)
{
    int ret;
    VIR_DEBUG("mon=%p path=%s bus=%s",
          mon, path, bus);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONAddPCIDisk(mon, path, bus, guestAddr);
    else
        ret = qemuMonitorTextAddPCIDisk(mon, path, bus, guestAddr);
    return ret;
}


int qemuMonitorAddPCINetwork(qemuMonitorPtr mon,
                             const char *nicstr,
                             virDevicePCIAddress *guestAddr)
{
    int ret;
    VIR_DEBUG("mon=%p nicstr=%s", mon, nicstr);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONAddPCINetwork(mon, nicstr, guestAddr);
    else
        ret = qemuMonitorTextAddPCINetwork(mon, nicstr, guestAddr);
    return ret;
}


int qemuMonitorRemovePCIDevice(qemuMonitorPtr mon,
                               virDevicePCIAddress *guestAddr)
{
    int ret;
    VIR_DEBUG("mon=%p domain=%d bus=%d slot=%d function=%d",
          mon, guestAddr->domain, guestAddr->bus,
          guestAddr->slot, guestAddr->function);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONRemovePCIDevice(mon, guestAddr);
    else
        ret = qemuMonitorTextRemovePCIDevice(mon, guestAddr);
    return ret;
}


int qemuMonitorSendFileHandle(qemuMonitorPtr mon,
                              const char *fdname,
                              int fd)
{
    int ret;
    VIR_DEBUG("mon=%p, fdname=%s fd=%d",
          mon, fdname, fd);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (fd < 0) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("fd must be valid"));
        return -1;
    }

    if (!mon->hasSendFD) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("qemu is not using a unix socket monitor, "
                         "cannot send fd %s"), fdname);
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONSendFileHandle(mon, fdname, fd);
    else
        ret = qemuMonitorTextSendFileHandle(mon, fdname, fd);
    return ret;
}


int qemuMonitorCloseFileHandle(qemuMonitorPtr mon,
                               const char *fdname)
{
    int ret = -1;
    virErrorPtr error;

    VIR_DEBUG("mon=%p fdname=%s",
          mon, fdname);

    error = virSaveLastError();

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        goto cleanup;
    }

    if (mon->json)
        ret = qemuMonitorJSONCloseFileHandle(mon, fdname);
    else
        ret = qemuMonitorTextCloseFileHandle(mon, fdname);

 cleanup:
    if (error) {
        virSetError(error);
        virFreeError(error);
    }
    return ret;
}


/* Add the open file descriptor FD into the non-negative set FDSET.
 * If NAME is present, it will be passed along for logging purposes.
 * Returns the counterpart fd that qemu received, or -1 on error.  */
int
qemuMonitorAddFd(qemuMonitorPtr mon, int fdset, int fd, const char *name)
{
    int ret = -1;
    VIR_DEBUG("mon=%p, fdset=%d, fd=%d, name=%s",
              mon, fdset, fd, NULLSTR(name));

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (fd < 0 || fdset < 0) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("fd and fdset must be valid"));
        return -1;
    }

    if (!mon->hasSendFD) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("qemu is not using a unix socket monitor, "
                         "cannot send fd %s"), NULLSTR(name));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONAddFd(mon, fdset, fd, name);
    else
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("add fd requires JSON monitor"));
    return ret;
}


/* Remove one of qemu's fds from the given FDSET, or if FD is
 * negative, remove the entire set.  Preserve any previous error on
 * entry.  Returns 0 on success, -1 on error.  */
int
qemuMonitorRemoveFd(qemuMonitorPtr mon, int fdset, int fd)
{
    int ret = -1;
    virErrorPtr error;

    VIR_DEBUG("mon=%p, fdset=%d, fd=%d", mon, fdset, fd);

    error = virSaveLastError();

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        goto cleanup;
    }

    if (mon->json)
        ret = qemuMonitorJSONRemoveFd(mon, fdset, fd);
    else
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("remove fd requires JSON monitor"));

 cleanup:
    if (error) {
        virSetError(error);
        virFreeError(error);
    }
    return ret;
}


int qemuMonitorAddHostNetwork(qemuMonitorPtr mon,
                              const char *netstr,
                              int *tapfd, char **tapfdName, int tapfdSize,
                              int *vhostfd, char **vhostfdName, int vhostfdSize)
{
    int ret = -1;
    size_t i = 0, j = 0;

    VIR_DEBUG("mon=%p netstr=%s tapfd=%p tapfdName=%p tapfdSize=%d "
              "vhostfd=%p vhostfdName=%p vhostfdSize=%d",
              mon, netstr, tapfd, tapfdName, tapfdSize,
              vhostfd, vhostfdName, vhostfdSize);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    for (i = 0; i < tapfdSize; i++) {
        if (qemuMonitorSendFileHandle(mon, tapfdName[i], tapfd[i]) < 0)
            goto cleanup;
    }
    for (j = 0; j < vhostfdSize; j++) {
        if (qemuMonitorSendFileHandle(mon, vhostfdName[j], vhostfd[j]) < 0)
            goto cleanup;
    }

    if (mon->json)
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor should be using AddNetdev"));
    else
        ret = qemuMonitorTextAddHostNetwork(mon, netstr);

 cleanup:
    if (ret < 0) {
        while (i--) {
            if (qemuMonitorCloseFileHandle(mon, tapfdName[i]) < 0)
                VIR_WARN("failed to close device handle '%s'", tapfdName[i]);
        }
        while (j--) {
            if (qemuMonitorCloseFileHandle(mon, vhostfdName[j]) < 0)
                VIR_WARN("failed to close device handle '%s'", vhostfdName[j]);
        }
    }

    return ret;
}


int qemuMonitorRemoveHostNetwork(qemuMonitorPtr mon,
                                 int vlan,
                                 const char *netname)
{
    int ret = -1;
    VIR_DEBUG("mon=%p netname=%s",
          mon, netname);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor should be using RemoveNetdev"));
    else
        ret = qemuMonitorTextRemoveHostNetwork(mon, vlan, netname);
    return ret;
}


int qemuMonitorAddNetdev(qemuMonitorPtr mon,
                         const char *netdevstr,
                         int *tapfd, char **tapfdName, int tapfdSize,
                         int *vhostfd, char **vhostfdName, int vhostfdSize)
{
    int ret = -1;
    size_t i = 0, j = 0;

    VIR_DEBUG("mon=%p netdevstr=%s tapfd=%p tapfdName=%p tapfdSize=%d"
              "vhostfd=%p vhostfdName=%p vhostfdSize=%d",
              mon, netdevstr, tapfd, tapfdName, tapfdSize,
              vhostfd, vhostfdName, vhostfdSize);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    for (i = 0; i < tapfdSize; i++) {
        if (qemuMonitorSendFileHandle(mon, tapfdName[i], tapfd[i]) < 0)
            goto cleanup;
    }
    for (j = 0; j < vhostfdSize; j++) {
        if (qemuMonitorSendFileHandle(mon, vhostfdName[j], vhostfd[j]) < 0)
            goto cleanup;
    }

    if (mon->json)
        ret = qemuMonitorJSONAddNetdev(mon, netdevstr);
    else
        ret = qemuMonitorTextAddNetdev(mon, netdevstr);

 cleanup:
    if (ret < 0) {
        while (i--) {
            if (qemuMonitorCloseFileHandle(mon, tapfdName[i]) < 0)
                VIR_WARN("failed to close device handle '%s'", tapfdName[i]);
        }
        while (j--) {
            if (qemuMonitorCloseFileHandle(mon, vhostfdName[j]) < 0)
                VIR_WARN("failed to close device handle '%s'", vhostfdName[j]);
        }
    }

    return ret;
}

int qemuMonitorRemoveNetdev(qemuMonitorPtr mon,
                            const char *alias)
{
    int ret;
    VIR_DEBUG("mon=%p alias=%s",
          mon, alias);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONRemoveNetdev(mon, alias);
    else
        ret = qemuMonitorTextRemoveNetdev(mon, alias);
    return ret;
}


int
qemuMonitorQueryRxFilter(qemuMonitorPtr mon, const char *alias,
                         virNetDevRxFilterPtr *filter)
{
    int ret = -1;
    VIR_DEBUG("mon=%p alias=%s filter=%p",
              mon, alias, filter);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }


    VIR_DEBUG("mon=%p, alias=%s", mon, alias);

    if (mon->json)
        ret = qemuMonitorJSONQueryRxFilter(mon, alias, filter);
    else
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("query-rx-filter requires JSON monitor"));
    return ret;
}


static void
qemuMonitorChardevInfoFree(void *data,
                           const void *name ATTRIBUTE_UNUSED)
{
    qemuMonitorChardevInfoPtr info = data;

    VIR_FREE(info->ptyPath);
    VIR_FREE(info);
}


int
qemuMonitorGetChardevInfo(qemuMonitorPtr mon,
                          virHashTablePtr *retinfo)
{
    int ret;
    virHashTablePtr info = NULL;

    VIR_DEBUG("mon=%p retinfo=%p", mon, retinfo);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        goto error;
    }

    if (!(info = virHashCreate(10, qemuMonitorChardevInfoFree)))
        goto error;

    if (mon->json)
        ret = qemuMonitorJSONGetChardevInfo(mon, info);
    else
        ret = qemuMonitorTextGetChardevInfo(mon, info);

    if (ret < 0)
        goto error;

    *retinfo = info;
    return 0;

 error:
    virHashFree(info);
    *retinfo = NULL;
    return -1;
}


int qemuMonitorAttachPCIDiskController(qemuMonitorPtr mon,
                                       const char *bus,
                                       virDevicePCIAddress *guestAddr)
{
    VIR_DEBUG("mon=%p type=%s", mon, bus);
    int ret;

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONAttachPCIDiskController(mon, bus, guestAddr);
    else
        ret = qemuMonitorTextAttachPCIDiskController(mon, bus, guestAddr);

    return ret;
}


int qemuMonitorAttachDrive(qemuMonitorPtr mon,
                           const char *drivestr,
                           virDevicePCIAddress *controllerAddr,
                           virDomainDeviceDriveAddress *driveAddr)
{
    VIR_DEBUG("mon=%p drivestr=%s domain=%d bus=%d slot=%d function=%d",
          mon, drivestr,
          controllerAddr->domain, controllerAddr->bus,
          controllerAddr->slot, controllerAddr->function);
    int ret = 1;

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor should be using AddDrive"));
    else
        ret = qemuMonitorTextAttachDrive(mon, drivestr, controllerAddr, driveAddr);

    return ret;
}

int qemuMonitorGetAllPCIAddresses(qemuMonitorPtr mon,
                                  qemuMonitorPCIAddress **addrs)
{
    VIR_DEBUG("mon=%p addrs=%p", mon, addrs);
    int ret;

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONGetAllPCIAddresses(mon, addrs);
    else
        ret = qemuMonitorTextGetAllPCIAddresses(mon, addrs);
    return ret;
}

int qemuMonitorDriveDel(qemuMonitorPtr mon,
                        const char *drivestr)
{
    VIR_DEBUG("mon=%p drivestr=%s", mon, drivestr);
    int ret;

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONDriveDel(mon, drivestr);
    else
        ret = qemuMonitorTextDriveDel(mon, drivestr);
    return ret;
}

int qemuMonitorDelDevice(qemuMonitorPtr mon,
                         const char *devalias)
{
    VIR_DEBUG("mon=%p devalias=%s", mon, devalias);
    int ret;

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONDelDevice(mon, devalias);
    else
        ret = qemuMonitorTextDelDevice(mon, devalias);
    return ret;
}


int qemuMonitorAddDeviceWithFd(qemuMonitorPtr mon,
                               const char *devicestr,
                               int fd,
                               const char *fdname)
{
    VIR_DEBUG("mon=%p device=%s fd=%d fdname=%s", mon, devicestr, fd,
              NULLSTR(fdname));
    int ret;

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (fd >= 0 && qemuMonitorSendFileHandle(mon, fdname, fd) < 0)
        return -1;

    if (mon->json)
        ret = qemuMonitorJSONAddDevice(mon, devicestr);
    else
        ret = qemuMonitorTextAddDevice(mon, devicestr);

    if (ret < 0 && fd >= 0) {
        if (qemuMonitorCloseFileHandle(mon, fdname) < 0)
            VIR_WARN("failed to close device handle '%s'", fdname);
    }

    return ret;
}

int qemuMonitorAddDevice(qemuMonitorPtr mon,
                         const char *devicestr)
{
    return qemuMonitorAddDeviceWithFd(mon, devicestr, -1, NULL);
}


/**
 * qemuMonitorAddObject:
 * @mon: Pointer to monitor object
 * @type: Type name of object to add
 * @objalias: Alias of the new object
 * @props: Optional arguments for the given type. The object is consumed and
 *         should not be referenced by the caller after this function returns.
 *
 * Returns 0 on success -1 on error.
 */
int
qemuMonitorAddObject(qemuMonitorPtr mon,
                     const char *type,
                     const char *objalias,
                     virJSONValuePtr props)
{
    VIR_DEBUG("mon=%p type=%s objalias=%s props=%p",
              mon, type, objalias, props);
    int ret = -1;

    if (mon->json)
        ret = qemuMonitorJSONAddObject(mon, type, objalias, props);
    else
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("object adding requires JSON monitor"));

    return ret;
}


int
qemuMonitorDelObject(qemuMonitorPtr mon,
                     const char *objalias)
{
    VIR_DEBUG("mon=%p objalias=%s", mon, objalias);
    int ret = -1;

    if (mon->json)
        ret = qemuMonitorJSONDelObject(mon, objalias);
    else
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("object deletion requires JSON monitor"));

    return ret;
}


int qemuMonitorAddDrive(qemuMonitorPtr mon,
                        const char *drivestr)
{
    VIR_DEBUG("mon=%p drive=%s", mon, drivestr);
    int ret;

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONAddDrive(mon, drivestr);
    else
        ret = qemuMonitorTextAddDrive(mon, drivestr);
    return ret;
}


int qemuMonitorSetDrivePassphrase(qemuMonitorPtr mon,
                                  const char *alias,
                                  const char *passphrase)
{
    VIR_DEBUG("mon=%p alias=%s passphrase=%p(value hidden)", mon, alias, passphrase);
    int ret;

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONSetDrivePassphrase(mon, alias, passphrase);
    else
        ret = qemuMonitorTextSetDrivePassphrase(mon, alias, passphrase);
    return ret;
}

int qemuMonitorCreateSnapshot(qemuMonitorPtr mon, const char *name)
{
    int ret;

    VIR_DEBUG("mon=%p, name=%s", mon, name);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONCreateSnapshot(mon, name);
    else
        ret = qemuMonitorTextCreateSnapshot(mon, name);
    return ret;
}

int qemuMonitorLoadSnapshot(qemuMonitorPtr mon, const char *name)
{
    int ret;

    VIR_DEBUG("mon=%p, name=%s", mon, name);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONLoadSnapshot(mon, name);
    else
        ret = qemuMonitorTextLoadSnapshot(mon, name);
    return ret;
}

int qemuMonitorDeleteSnapshot(qemuMonitorPtr mon, const char *name)
{
    int ret;

    VIR_DEBUG("mon=%p, name=%s", mon, name);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONDeleteSnapshot(mon, name);
    else
        ret = qemuMonitorTextDeleteSnapshot(mon, name);
    return ret;
}

/* Use the snapshot_blkdev command to convert the existing file for
 * device into a read-only backing file of a new qcow2 image located
 * at file.  */
int
qemuMonitorDiskSnapshot(qemuMonitorPtr mon, virJSONValuePtr actions,
                        const char *device, const char *file,
                        const char *format, bool reuse)
{
    int ret = -1;

    VIR_DEBUG("mon=%p, actions=%p, device=%s, file=%s, format=%s, reuse=%d",
              mon, actions, device, file, format, reuse);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONDiskSnapshot(mon, actions, device, file, format,
                                          reuse);
    else
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("disk snapshot requires JSON monitor"));
    return ret;
}

/* Start a drive-mirror block job.  bandwidth is in bytes/sec.  */
int
qemuMonitorDriveMirror(qemuMonitorPtr mon,
                       const char *device, const char *file,
                       const char *format, unsigned long long bandwidth,
                       unsigned int granularity, unsigned long long buf_size,
                       unsigned int flags)
{
    int ret = -1;

    VIR_DEBUG("mon=%p, device=%s, file=%s, format=%s, bandwidth=%lld, "
              "granularity=%#x, buf_size=%lld, flags=%x",
              mon, device, file, NULLSTR(format), bandwidth, granularity,
              buf_size, flags);

    if (mon->json)
        ret = qemuMonitorJSONDriveMirror(mon, device, file, format, bandwidth,
                                         granularity, buf_size, flags);
    else
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("drive-mirror requires JSON monitor"));
    return ret;
}

/* Use the transaction QMP command to run atomic snapshot commands.  */
int
qemuMonitorTransaction(qemuMonitorPtr mon, virJSONValuePtr actions)
{
    int ret = -1;

    VIR_DEBUG("mon=%p, actions=%p", mon, actions);

    if (mon->json)
        ret = qemuMonitorJSONTransaction(mon, actions);
    else
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("transaction requires JSON monitor"));
    return ret;
}

/* Start a block-commit block job.  bandwidth is in bytes/sec.  */
int
qemuMonitorBlockCommit(qemuMonitorPtr mon, const char *device,
                       const char *top, const char *base,
                       const char *backingName,
                       unsigned long long bandwidth)
{
    int ret = -1;

    VIR_DEBUG("mon=%p, device=%s, top=%s, base=%s, backingName=%s, "
              "bandwidth=%llu",
              mon, device, top, base, NULLSTR(backingName), bandwidth);

    if (mon->json)
        ret = qemuMonitorJSONBlockCommit(mon, device, top, base,
                                         backingName, bandwidth);
    else
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("block-commit requires JSON monitor"));
    return ret;
}


/* Probe whether active commits are supported by a given qemu binary. */
bool
qemuMonitorSupportsActiveCommit(qemuMonitorPtr mon)
{
    if (!mon->json)
        return false;

    return qemuMonitorJSONBlockCommit(mon, "bogus", NULL, NULL, NULL, 0) == -2;
}


/* Use the block-job-complete monitor command to pivot a block copy
 * job.  */
int
qemuMonitorDrivePivot(qemuMonitorPtr mon, const char *device,
                      const char *file, const char *format)
{
    int ret = -1;

    VIR_DEBUG("mon=%p, device=%s, file=%s, format=%s",
              mon, device, file, NULLSTR(format));

    if (mon->json)
        ret = qemuMonitorJSONDrivePivot(mon, device, file, format);
    else
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("drive pivot requires JSON monitor"));
    return ret;
}

int qemuMonitorArbitraryCommand(qemuMonitorPtr mon,
                                const char *cmd,
                                char **reply,
                                bool hmp)
{
    int ret;

    VIR_DEBUG("mon=%p, cmd=%s, reply=%p, hmp=%d", mon, cmd, reply, hmp);

    if (mon->json)
        ret = qemuMonitorJSONArbitraryCommand(mon, cmd, reply, hmp);
    else
        ret = qemuMonitorTextArbitraryCommand(mon, cmd, reply);
    return ret;
}


int qemuMonitorInjectNMI(qemuMonitorPtr mon)
{
    int ret;

    VIR_DEBUG("mon=%p", mon);

    if (mon->json)
        ret = qemuMonitorJSONInjectNMI(mon);
    else
        ret = qemuMonitorTextInjectNMI(mon);
    return ret;
}

int qemuMonitorSendKey(qemuMonitorPtr mon,
                       unsigned int holdtime,
                       unsigned int *keycodes,
                       unsigned int nkeycodes)
{
    int ret;

    VIR_DEBUG("mon=%p, holdtime=%u, nkeycodes=%u",
              mon, holdtime, nkeycodes);

    if (mon->json)
        ret = qemuMonitorJSONSendKey(mon, holdtime, keycodes, nkeycodes);
    else
        ret = qemuMonitorTextSendKey(mon, holdtime, keycodes, nkeycodes);
    return ret;
}

int qemuMonitorScreendump(qemuMonitorPtr mon,
                          const char *file)
{
    int ret;

    VIR_DEBUG("mon=%p, file=%s", mon, file);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (mon->json)
        ret = qemuMonitorJSONScreendump(mon, file);
    else
        ret = qemuMonitorTextScreendump(mon, file);
    return ret;
}

/* bandwidth is in bytes/sec */
int
qemuMonitorBlockJob(qemuMonitorPtr mon,
                    const char *device,
                    const char *base,
                    const char *backingName,
                    unsigned long long bandwidth,
                    qemuMonitorBlockJobCmd mode,
                    bool modern)
{
    int ret = -1;

    VIR_DEBUG("mon=%p, device=%s, base=%s, backingName=%s, bandwidth=%lluB, "
              "mode=%o, modern=%d",
              mon, device, NULLSTR(base), NULLSTR(backingName),
              bandwidth, mode, modern);

    if (mon->json)
        ret = qemuMonitorJSONBlockJob(mon, device, base, backingName,
                                      bandwidth, mode, modern);
    else
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("block jobs require JSON monitor"));
    return ret;
}


int
qemuMonitorBlockJobInfo(qemuMonitorPtr mon,
                        const char *device,
                        virDomainBlockJobInfoPtr info,
                        unsigned long long *bandwidth)
{
    int ret = -1;

    VIR_DEBUG("mon=%p, device=%s, info=%p, bandwidth=%p",
              mon, device, info, bandwidth);

    if (mon->json)
        ret = qemuMonitorJSONBlockJobInfo(mon, device, info, bandwidth);
    else
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("block jobs require JSON monitor"));
    return ret;
}


int qemuMonitorSetBlockIoThrottle(qemuMonitorPtr mon,
                                  const char *device,
                                  virDomainBlockIoTuneInfoPtr info,
                                  bool supportMaxOptions)
{
    int ret;

    VIR_DEBUG("mon=%p, device=%p, info=%p", mon, device, info);

    if (mon->json) {
        ret = qemuMonitorJSONSetBlockIoThrottle(mon, device, info, supportMaxOptions);
    } else {
        ret = qemuMonitorTextSetBlockIoThrottle(mon, device, info);
    }
    return ret;
}

int qemuMonitorGetBlockIoThrottle(qemuMonitorPtr mon,
                                  const char *device,
                                  virDomainBlockIoTuneInfoPtr reply,
                                  bool supportMaxOptions)
{
    int ret;

    VIR_DEBUG("mon=%p, device=%p, reply=%p", mon, device, reply);

    if (mon->json) {
        ret = qemuMonitorJSONGetBlockIoThrottle(mon, device, reply, supportMaxOptions);
    } else {
        ret = qemuMonitorTextGetBlockIoThrottle(mon, device, reply);
    }
    return ret;
}


int qemuMonitorVMStatusToPausedReason(const char *status)
{
    int st;

    if (!status)
        return VIR_DOMAIN_PAUSED_UNKNOWN;

    if ((st = qemuMonitorVMStatusTypeFromString(status)) < 0) {
        VIR_WARN("Qemu reported unknown VM status: '%s'", status);
        return VIR_DOMAIN_PAUSED_UNKNOWN;
    }

    switch ((qemuMonitorVMStatus) st) {
    case QEMU_MONITOR_VM_STATUS_DEBUG:
    case QEMU_MONITOR_VM_STATUS_INTERNAL_ERROR:
    case QEMU_MONITOR_VM_STATUS_RESTORE_VM:
        return VIR_DOMAIN_PAUSED_UNKNOWN;

    case QEMU_MONITOR_VM_STATUS_INMIGRATE:
    case QEMU_MONITOR_VM_STATUS_POSTMIGRATE:
    case QEMU_MONITOR_VM_STATUS_FINISH_MIGRATE:
        return VIR_DOMAIN_PAUSED_MIGRATION;

    case QEMU_MONITOR_VM_STATUS_IO_ERROR:
        return VIR_DOMAIN_PAUSED_IOERROR;

    case QEMU_MONITOR_VM_STATUS_PAUSED:
    case QEMU_MONITOR_VM_STATUS_PRELAUNCH:
        return VIR_DOMAIN_PAUSED_USER;

    case QEMU_MONITOR_VM_STATUS_RUNNING:
        VIR_WARN("Qemu reports the guest is paused but status is 'running'");
        return VIR_DOMAIN_PAUSED_UNKNOWN;

    case QEMU_MONITOR_VM_STATUS_SAVE_VM:
        return VIR_DOMAIN_PAUSED_SAVE;

    case QEMU_MONITOR_VM_STATUS_SHUTDOWN:
        return VIR_DOMAIN_PAUSED_SHUTTING_DOWN;

    case QEMU_MONITOR_VM_STATUS_WATCHDOG:
        return VIR_DOMAIN_PAUSED_WATCHDOG;

    case QEMU_MONITOR_VM_STATUS_GUEST_PANICKED:
        return VIR_DOMAIN_PAUSED_CRASHED;

    /* unreachable from this point on */
    case QEMU_MONITOR_VM_STATUS_LAST:
        ;
    }
    return VIR_DOMAIN_PAUSED_UNKNOWN;
}


int qemuMonitorOpenGraphics(qemuMonitorPtr mon,
                            const char *protocol,
                            int fd,
                            const char *fdname,
                            bool skipauth)
{
    VIR_DEBUG("mon=%p protocol=%s fd=%d fdname=%s skipauth=%d",
              mon, protocol, fd, NULLSTR(fdname), skipauth);
    int ret;

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (qemuMonitorSendFileHandle(mon, fdname, fd) < 0)
        return -1;

    if (mon->json)
        ret = qemuMonitorJSONOpenGraphics(mon, protocol, fdname, skipauth);
    else
        ret = qemuMonitorTextOpenGraphics(mon, protocol, fdname, skipauth);

    if (ret < 0) {
        if (qemuMonitorCloseFileHandle(mon, fdname) < 0)
            VIR_WARN("failed to close device handle '%s'", fdname);
    }

    return ret;
}

int qemuMonitorSystemWakeup(qemuMonitorPtr mon)
{
    VIR_DEBUG("mon=%p", mon);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONSystemWakeup(mon);
}

int qemuMonitorGetVersion(qemuMonitorPtr mon,
                          int *major,
                          int *minor,
                          int *micro,
                          char **package)
{
    VIR_DEBUG("mon=%p major=%p minor=%p micro=%p package=%p",
              mon, major, minor, micro, package);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONGetVersion(mon, major, minor, micro, package);
}

int qemuMonitorGetMachines(qemuMonitorPtr mon,
                           qemuMonitorMachineInfoPtr **machines)
{
    VIR_DEBUG("mon=%p machines=%p",
              mon, machines);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONGetMachines(mon, machines);
}

void qemuMonitorMachineInfoFree(qemuMonitorMachineInfoPtr machine)
{
    if (!machine)
        return;
    VIR_FREE(machine->name);
    VIR_FREE(machine->alias);
    VIR_FREE(machine);
}

int qemuMonitorGetCPUDefinitions(qemuMonitorPtr mon,
                                 char ***cpus)
{
    VIR_DEBUG("mon=%p cpus=%p",
              mon, cpus);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONGetCPUDefinitions(mon, cpus);
}


int qemuMonitorGetCommands(qemuMonitorPtr mon,
                           char ***commands)
{
    VIR_DEBUG("mon=%p commands=%p",
              mon, commands);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONGetCommands(mon, commands);
}


int qemuMonitorGetEvents(qemuMonitorPtr mon,
                         char ***events)
{
    VIR_DEBUG("mon=%p events=%p",
              mon, events);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONGetEvents(mon, events);
}


/* Collect the parameters associated with a given command line option.
 * Return count of known parameters or -1 on error.  */
int
qemuMonitorGetCommandLineOptionParameters(qemuMonitorPtr mon,
                                          const char *option,
                                          char ***params,
                                          bool *found)
{
    VIR_DEBUG("mon=%p option=%s params=%p", mon, option, params);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONGetCommandLineOptionParameters(mon, option,
                                                         params, found);
}


int qemuMonitorGetKVMState(qemuMonitorPtr mon,
                           bool *enabled,
                           bool *present)
{
    VIR_DEBUG("mon=%p enabled=%p present=%p",
              mon, enabled, present);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONGetKVMState(mon, enabled, present);
}


int qemuMonitorGetObjectTypes(qemuMonitorPtr mon,
                              char ***types)
{
    VIR_DEBUG("mon=%p types=%p",
              mon, types);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONGetObjectTypes(mon, types);
}


int qemuMonitorGetObjectProps(qemuMonitorPtr mon,
                              const char *type,
                              char ***props)
{
    VIR_DEBUG("mon=%p type=%s props=%p",
              mon, type, props);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONGetObjectProps(mon, type, props);
}


char *qemuMonitorGetTargetArch(qemuMonitorPtr mon)
{
    VIR_DEBUG("mon=%p",
              mon);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return NULL;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return NULL;
    }

    return qemuMonitorJSONGetTargetArch(mon);
}


int
qemuMonitorGetMigrationCapabilities(qemuMonitorPtr mon,
                                    char ***capabilities)
{
    VIR_DEBUG("mon=%p", mon);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    /* No capability is supported without JSON monitor */
    if (!mon->json)
        return 0;

    return qemuMonitorJSONGetMigrationCapabilities(mon, capabilities);
}


/**
 * Returns 1 if @capability is supported, 0 if it's not, or -1 on error.
 */
int qemuMonitorGetMigrationCapability(qemuMonitorPtr mon,
                                      qemuMonitorMigrationCaps capability)
{
    VIR_DEBUG("mon=%p capability=%d", mon, capability);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    /* No capability is supported without JSON monitor */
    if (!mon->json)
        return 0;

    return qemuMonitorJSONGetMigrationCapability(mon, capability);
}

int qemuMonitorSetMigrationCapability(qemuMonitorPtr mon,
                                      qemuMonitorMigrationCaps capability,
                                      bool state)
{
    VIR_DEBUG("mon=%p capability=%d", mon, capability);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONSetMigrationCapability(mon, capability, state);
}

int qemuMonitorNBDServerStart(qemuMonitorPtr mon,
                              const char *host,
                              unsigned int port)
{
    VIR_DEBUG("mon=%p host=%s port=%u",
              mon, host, port);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONNBDServerStart(mon, host, port);
}

int qemuMonitorNBDServerAdd(qemuMonitorPtr mon,
                            const char *deviceID,
                            bool writable)
{
    VIR_DEBUG("mon=%p deviceID=%s",
              mon, deviceID);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONNBDServerAdd(mon, deviceID, writable);
}

int qemuMonitorNBDServerStop(qemuMonitorPtr mon)
{
    VIR_DEBUG("mon=%p", mon);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONNBDServerStop(mon);
}


int qemuMonitorGetTPMModels(qemuMonitorPtr mon,
                            char ***tpmmodels)
{
    VIR_DEBUG("mon=%p tpmmodels=%p",
              mon, tpmmodels);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONGetTPMModels(mon, tpmmodels);
}


int qemuMonitorGetTPMTypes(qemuMonitorPtr mon,
                           char ***tpmtypes)
{
    VIR_DEBUG("mon=%p tpmtypes=%p",
              mon, tpmtypes);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONGetTPMTypes(mon, tpmtypes);
}

int qemuMonitorAttachCharDev(qemuMonitorPtr mon,
                             const char *chrID,
                             virDomainChrSourceDefPtr chr)
{
    VIR_DEBUG("mon=%p chrID=%s chr=%p", mon, chrID, chr);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONAttachCharDev(mon, chrID, chr);
}

int qemuMonitorDetachCharDev(qemuMonitorPtr mon,
                             const char *chrID)
{
    VIR_DEBUG("mon=%p chrID=%s", mon, chrID);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONDetachCharDev(mon, chrID);
}

int
qemuMonitorGetDeviceAliases(qemuMonitorPtr mon,
                            char ***aliases)
{
    VIR_DEBUG("mon=%p, aliases=%p", mon, aliases);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONGetDeviceAliases(mon, aliases);
}


/**
 * qemuMonitorSetDomainLog:
 * Set the file descriptor of the open VM log file to report potential
 * early startup errors of qemu.
 *
 * @mon: Monitor object to set the log file reading on
 * @logfd: File descriptor of the already open log file
 */
int
qemuMonitorSetDomainLog(qemuMonitorPtr mon, int logfd)
{
    VIR_FORCE_CLOSE(mon->logfd);
    if (logfd >= 0 &&
        (mon->logfd = dup(logfd)) < 0) {
        virReportSystemError(errno, "%s", _("failed to duplicate log fd"));
        return -1;
    }

    return 0;
}


/**
 * qemuMonitorJSONGetGuestCPU:
 * @mon: Pointer to the monitor
 * @arch: arch of the guest
 * @data: returns the cpu data
 *
 * Retrieve the definition of the guest CPU from a running qemu instance.
 *
 * Returns 0 on success, -2 if the operation is not supported by the guest,
 * -1 on other errors.
 */
int
qemuMonitorGetGuestCPU(qemuMonitorPtr mon,
                       virArch arch,
                       virCPUDataPtr *data)
{
    VIR_DEBUG("mon=%p, arch='%s' data='%p'", mon, virArchToString(arch), data);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    *data = NULL;

    return qemuMonitorJSONGetGuestCPU(mon, arch, data);
}

/**
 * qemuMonitorRTCResetReinjection:
 * @mon: Pointer to the monitor
 *
 * Issue rtc-reset-reinjection command.
 * This should be used in cases where guest time is restored via
 * guest agent, so RTC injection is not needed (in fact it would
 * confuse guest's RTC).
 *
 * Returns 0 on success
 *        -1 on error.
 */
int
qemuMonitorRTCResetReinjection(qemuMonitorPtr mon)
{

    VIR_DEBUG("mon=%p", mon);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    if (!mon->json) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("JSON monitor is required"));
        return -1;
    }

    return qemuMonitorJSONRTCResetReinjection(mon);
}

/**
 * qemuMonitorGetIOThreads:
 * @mon: Pointer to the monitor
 * @iothreads: Location to return array of IOThreadInfo data
 *
 * Issue query-iothreads command.
 * Retrieve the list of iothreads defined/running for the machine
 *
 * Returns count of IOThreadInfo structures on success
 *        -1 on error.
 */
int
qemuMonitorGetIOThreads(qemuMonitorPtr mon,
                        qemuMonitorIOThreadsInfoPtr **iothreads)
{

    VIR_DEBUG("mon=%p iothreads=%p", mon, iothreads);

    if (!mon) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("monitor must not be NULL"));
        return -1;
    }

    /* Requires JSON to make the query */
    if (!mon->json) {
        *iothreads = NULL;
        return 0;
    }

    return qemuMonitorJSONGetIOThreads(mon, iothreads);
}

void qemuMonitorIOThreadsInfoFree(qemuMonitorIOThreadsInfoPtr iothread)
{
    if (!iothread)
        return;
    VIR_FREE(iothread->name);
    VIR_FREE(iothread);
}
