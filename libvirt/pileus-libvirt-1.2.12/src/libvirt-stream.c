/*
 * libvirt-stream.c: entry points for virStreamPtr APIs
 *
 * Copyright (C) 2006-2014 Red Hat, Inc.
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
 */

#include <config.h>

#include "datatypes.h"
#include "viralloc.h"
#include "virlog.h"

VIR_LOG_INIT("libvirt.stream");

#define VIR_FROM_THIS VIR_FROM_STREAMS


/**
 * virStreamNew:
 * @conn: pointer to the connection
 * @flags: bitwise-OR of virStreamFlags
 *
 * Creates a new stream object which can be used to perform
 * streamed I/O with other public API function.
 *
 * When no longer needed, a stream object must be released
 * with virStreamFree. If a data stream has been used,
 * then the application must call virStreamFinish or
 * virStreamAbort before free'ing to, in order to notify
 * the driver of termination.
 *
 * If a non-blocking data stream is required passed
 * VIR_STREAM_NONBLOCK for flags, otherwise pass 0.
 *
 * Returns the new stream, or NULL upon error
 */
virStreamPtr
virStreamNew(virConnectPtr conn,
             unsigned int flags)
{
    virStreamPtr st;

    VIR_DEBUG("conn=%p, flags=%x", conn, flags);

    virResetLastError();

    virCheckConnectReturn(conn, NULL);

    st = virGetStream(conn);
    if (st)
        st->flags = flags;
    else
        virDispatchError(conn);

    return st;
}


/**
 * virStreamRef:
 * @stream: pointer to the stream
 *
 * Increment the reference count on the stream. For each
 * additional call to this method, there shall be a corresponding
 * call to virStreamFree to release the reference count, once
 * the caller no longer needs the reference to this object.
 *
 * Returns 0 in case of success, -1 in case of failure
 */
int
virStreamRef(virStreamPtr stream)
{
    VIR_DEBUG("stream=%p refs=%d", stream,
              stream ? stream->object.u.s.refs : 0);

    virResetLastError();

    virCheckStreamReturn(stream, -1);

    virObjectRef(stream);
    return 0;
}


/**
 * virStreamSend:
 * @stream: pointer to the stream object
 * @data: buffer to write to stream
 * @nbytes: size of @data buffer
 *
 * Write a series of bytes to the stream. This method may
 * block the calling application for an arbitrary amount
 * of time. Once an application has finished sending data
 * it should call virStreamFinish to wait for successful
 * confirmation from the driver, or detect any error.
 *
 * This method may not be used if a stream source has been
 * registered.
 *
 * Errors are not guaranteed to be reported synchronously
 * with the call, but may instead be delayed until a
 * subsequent call.
 *
 * An example using this with a hypothetical file upload
 * API looks like
 *
 *     virStreamPtr st = virStreamNew(conn, 0);
 *     int fd = open("demo.iso", O_RDONLY);
 *
 *     virConnectUploadFile(conn, "demo.iso", st);
 *
 *     while (1) {
 *          char buf[1024];
 *          int got = read(fd, buf, 1024);
 *          if (got < 0) {
 *             virStreamAbort(st);
 *             break;
 *          }
 *          if (got == 0) {
 *             virStreamFinish(st);
 *             break;
 *          }
 *          int offset = 0;
 *          while (offset < got) {
 *             int sent = virStreamSend(st, buf+offset, got-offset);
 *             if (sent < 0) {
 *                virStreamAbort(st);
 *                goto done;
 *             }
 *             offset += sent;
 *          }
 *      }
 *      if (virStreamFinish(st) < 0)
 *         ... report an error ....
 *    done:
 *      virStreamFree(st);
 *      close(fd);
 *
 * Returns the number of bytes written, which may be less
 * than requested.
 *
 * Returns -1 upon error, at which time the stream will
 * be marked as aborted, and the caller should now release
 * the stream with virStreamFree.
 *
 * Returns -2 if the outgoing transmit buffers are full &
 * the stream is marked as non-blocking.
 */
int
virStreamSend(virStreamPtr stream,
              const char *data,
              size_t nbytes)
{
    VIR_DEBUG("stream=%p, data=%p, nbytes=%zi", stream, data, nbytes);

    virResetLastError();

    virCheckStreamReturn(stream, -1);
    virCheckNonNullArgGoto(data, error);

    if (stream->driver &&
        stream->driver->streamSend) {
        int ret;
        ret = (stream->driver->streamSend)(stream, data, nbytes);
        if (ret == -2)
            return -2;
        if (ret < 0)
            goto error;
        return ret;
    }

    virReportUnsupportedError();

 error:
    virDispatchError(stream->conn);
    return -1;
}


/**
 * virStreamRecv:
 * @stream: pointer to the stream object
 * @data: buffer to read into from stream
 * @nbytes: size of @data buffer
 *
 * Reads a series of bytes from the stream. This method may
 * block the calling application for an arbitrary amount
 * of time.
 *
 * Errors are not guaranteed to be reported synchronously
 * with the call, but may instead be delayed until a
 * subsequent call.
 *
 * An example using this with a hypothetical file download
 * API looks like
 *
 *     virStreamPtr st = virStreamNew(conn, 0);
 *     int fd = open("demo.iso", O_WRONLY, 0600);
 *
 *     virConnectDownloadFile(conn, "demo.iso", st);
 *
 *     while (1) {
 *         char buf[1024];
 *         int got = virStreamRecv(st, buf, 1024);
 *         if (got < 0)
 *            break;
 *         if (got == 0) {
 *            virStreamFinish(st);
 *            break;
 *         }
 *         int offset = 0;
 *         while (offset < got) {
 *            int sent = write(fd, buf + offset, got - offset);
 *            if (sent < 0) {
 *               virStreamAbort(st);
 *               goto done;
 *            }
 *            offset += sent;
 *         }
 *     }
 *     if (virStreamFinish(st) < 0)
 *        ... report an error ....
 *   done:
 *     virStreamFree(st);
 *     close(fd);
 *
 *
 * Returns the number of bytes read, which may be less
 * than requested.
 *
 * Returns 0 when the end of the stream is reached, at
 * which time the caller should invoke virStreamFinish()
 * to get confirmation of stream completion.
 *
 * Returns -1 upon error, at which time the stream will
 * be marked as aborted, and the caller should now release
 * the stream with virStreamFree.
 *
 * Returns -2 if there is no data pending to be read & the
 * stream is marked as non-blocking.
 */
int
virStreamRecv(virStreamPtr stream,
              char *data,
              size_t nbytes)
{
    VIR_DEBUG("stream=%p, data=%p, nbytes=%zi", stream, data, nbytes);

    virResetLastError();

    virCheckStreamReturn(stream, -1);
    virCheckNonNullArgGoto(data, error);

    if (stream->driver &&
        stream->driver->streamRecv) {
        int ret;
        ret = (stream->driver->streamRecv)(stream, data, nbytes);
        if (ret == -2)
            return -2;
        if (ret < 0)
            goto error;
        return ret;
    }

    virReportUnsupportedError();

 error:
    virDispatchError(stream->conn);
    return -1;
}


/**
 * virStreamSendAll:
 * @stream: pointer to the stream object
 * @handler: source callback for reading data from application
 * @opaque: application defined data
 *
 * Send the entire data stream, reading the data from the
 * requested data source. This is simply a convenient alternative
 * to virStreamSend, for apps that do blocking-I/O.
 *
 * An example using this with a hypothetical file upload
 * API looks like
 *
 *   int mysource(virStreamPtr st, char *buf, int nbytes, void *opaque) {
 *       int *fd = opaque;
 *
 *       return read(*fd, buf, nbytes);
 *   }
 *
 *   virStreamPtr st = virStreamNew(conn, 0);
 *   int fd = open("demo.iso", O_RDONLY);
 *
 *   virConnectUploadFile(conn, st);
 *   if (virStreamSendAll(st, mysource, &fd) < 0) {
 *      ...report an error ...
 *      goto done;
 *   }
 *   if (virStreamFinish(st) < 0)
 *      ...report an error...
 *   virStreamFree(st);
 *   close(fd);
 *
 * Returns 0 if all the data was successfully sent. The caller
 * should invoke virStreamFinish(st) to flush the stream upon
 * success and then virStreamFree
 *
 * Returns -1 upon any error, with virStreamAbort() already
 * having been called,  so the caller need only call
 * virStreamFree()
 */
int
virStreamSendAll(virStreamPtr stream,
                 virStreamSourceFunc handler,
                 void *opaque)
{
    char *bytes = NULL;
    int want = 1024*64;
    int ret = -1;
    VIR_DEBUG("stream=%p, handler=%p, opaque=%p", stream, handler, opaque);

    virResetLastError();

    virCheckStreamReturn(stream, -1);
    virCheckNonNullArgGoto(handler, cleanup);

    if (stream->flags & VIR_STREAM_NONBLOCK) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("data sources cannot be used for non-blocking streams"));
        goto cleanup;
    }

    if (VIR_ALLOC_N(bytes, want) < 0)
        goto cleanup;

    for (;;) {
        int got, offset = 0;
        got = (handler)(stream, bytes, want, opaque);
        if (got < 0) {
            virStreamAbort(stream);
            goto cleanup;
        }
        if (got == 0)
            break;
        while (offset < got) {
            int done;
            done = virStreamSend(stream, bytes + offset, got - offset);
            if (done < 0)
                goto cleanup;
            offset += done;
        }
    }
    ret = 0;

 cleanup:
    VIR_FREE(bytes);

    if (ret != 0)
        virDispatchError(stream->conn);

    return ret;
}


/**
 * virStreamRecvAll:
 * @stream: pointer to the stream object
 * @handler: sink callback for writing data to application
 * @opaque: application defined data
 *
 * Receive the entire data stream, sending the data to the
 * requested data sink. This is simply a convenient alternative
 * to virStreamRecv, for apps that do blocking-I/O.
 *
 * An example using this with a hypothetical file download
 * API looks like
 *
 *   int mysink(virStreamPtr st, const char *buf, int nbytes, void *opaque) {
 *       int *fd = opaque;
 *
 *       return write(*fd, buf, nbytes);
 *   }
 *
 *   virStreamPtr st = virStreamNew(conn, 0);
 *   int fd = open("demo.iso", O_WRONLY);
 *
 *   virConnectUploadFile(conn, st);
 *   if (virStreamRecvAll(st, mysink, &fd) < 0) {
 *      ...report an error ...
 *      goto done;
 *   }
 *   if (virStreamFinish(st) < 0)
 *      ...report an error...
 *   virStreamFree(st);
 *   close(fd);
 *
 * Returns 0 if all the data was successfully received. The caller
 * should invoke virStreamFinish(st) to flush the stream upon
 * success and then virStreamFree
 *
 * Returns -1 upon any error, with virStreamAbort() already
 * having been called,  so the caller need only call
 * virStreamFree()
 */
int
virStreamRecvAll(virStreamPtr stream,
                 virStreamSinkFunc handler,
                 void *opaque)
{
    char *bytes = NULL;
    int want = 1024*64;
    int ret = -1;
    VIR_DEBUG("stream=%p, handler=%p, opaque=%p", stream, handler, opaque);

    virResetLastError();

    virCheckStreamReturn(stream, -1);
    virCheckNonNullArgGoto(handler, cleanup);

    if (stream->flags & VIR_STREAM_NONBLOCK) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("data sinks cannot be used for non-blocking streams"));
        goto cleanup;
    }


    if (VIR_ALLOC_N(bytes, want) < 0)
        goto cleanup;

    for (;;) {
        int got, offset = 0;
        got = virStreamRecv(stream, bytes, want);
        if (got < 0)
            goto cleanup;
        if (got == 0)
            break;
        while (offset < got) {
            int done;
            done = (handler)(stream, bytes + offset, got - offset, opaque);
            if (done < 0) {
                virStreamAbort(stream);
                goto cleanup;
            }
            offset += done;
        }
    }
    ret = 0;

 cleanup:
    VIR_FREE(bytes);

    if (ret != 0)
        virDispatchError(stream->conn);

    return ret;
}


/**
 * virStreamEventAddCallback:
 * @stream: pointer to the stream object
 * @events: set of events to monitor
 * @cb: callback to invoke when an event occurs
 * @opaque: application defined data
 * @ff: callback to free @opaque data
 *
 * Register a callback to be notified when a stream
 * becomes writable, or readable. This is most commonly
 * used in conjunction with non-blocking data streams
 * to integrate into an event loop
 *
 * Returns 0 on success, -1 upon error
 */
int
virStreamEventAddCallback(virStreamPtr stream,
                          int events,
                          virStreamEventCallback cb,
                          void *opaque,
                          virFreeCallback ff)
{
    VIR_DEBUG("stream=%p, events=%d, cb=%p, opaque=%p, ff=%p",
              stream, events, cb, opaque, ff);

    virResetLastError();

    virCheckStreamReturn(stream, -1);

    if (stream->driver &&
        stream->driver->streamEventAddCallback) {
        int ret;
        ret = (stream->driver->streamEventAddCallback)(stream, events, cb, opaque, ff);
        if (ret < 0)
            goto error;
        return ret;
    }

    virReportUnsupportedError();

 error:
    virDispatchError(stream->conn);
    return -1;
}


/**
 * virStreamEventUpdateCallback:
 * @stream: pointer to the stream object
 * @events: set of events to monitor
 *
 * Changes the set of events to monitor for a stream. This allows
 * for event notification to be changed without having to
 * unregister & register the callback completely. This method
 * is guaranteed to succeed if a callback is already registered
 *
 * Returns 0 on success, -1 if no callback is registered
 */
int
virStreamEventUpdateCallback(virStreamPtr stream,
                             int events)
{
    VIR_DEBUG("stream=%p, events=%d", stream, events);

    virResetLastError();

    virCheckStreamReturn(stream, -1);

    if (stream->driver &&
        stream->driver->streamEventUpdateCallback) {
        int ret;
        ret = (stream->driver->streamEventUpdateCallback)(stream, events);
        if (ret < 0)
            goto error;
        return ret;
    }

    virReportUnsupportedError();

 error:
    virDispatchError(stream->conn);
    return -1;
}


/**
 * virStreamEventRemoveCallback:
 * @stream: pointer to the stream object
 *
 * Remove an event callback from the stream
 *
 * Returns 0 on success, -1 on error
 */
int
virStreamEventRemoveCallback(virStreamPtr stream)
{
    VIR_DEBUG("stream=%p", stream);

    virResetLastError();

    virCheckStreamReturn(stream, -1);

    if (stream->driver &&
        stream->driver->streamEventRemoveCallback) {
        int ret;
        ret = (stream->driver->streamEventRemoveCallback)(stream);
        if (ret < 0)
            goto error;
        return ret;
    }

    virReportUnsupportedError();

 error:
    virDispatchError(stream->conn);
    return -1;
}


/**
 * virStreamFinish:
 * @stream: pointer to the stream object
 *
 * Indicate that there is no further data to be transmitted
 * on the stream. For output streams this should be called once
 * all data has been written. For input streams this should be
 * called once virStreamRecv returns end-of-file.
 *
 * This method is a synchronization point for all asynchronous
 * errors, so if this returns a success code the application can
 * be sure that all data has been successfully processed.
 *
 * Returns 0 on success, -1 upon error
 */
int
virStreamFinish(virStreamPtr stream)
{
    VIR_DEBUG("stream=%p", stream);

    virResetLastError();

    virCheckStreamReturn(stream, -1);

    if (stream->driver &&
        stream->driver->streamFinish) {
        int ret;
        ret = (stream->driver->streamFinish)(stream);
        if (ret < 0)
            goto error;
        return ret;
    }

    virReportUnsupportedError();

 error:
    virDispatchError(stream->conn);
    return -1;
}


/**
 * virStreamAbort:
 * @stream: pointer to the stream object
 *
 * Request that the in progress data transfer be cancelled
 * abnormally before the end of the stream has been reached.
 * For output streams this can be used to inform the driver
 * that the stream is being terminated early. For input
 * streams this can be used to inform the driver that it
 * should stop sending data.
 *
 * Returns 0 on success, -1 upon error
 */
int
virStreamAbort(virStreamPtr stream)
{
    VIR_DEBUG("stream=%p", stream);

    virResetLastError();

    virCheckStreamReturn(stream, -1);

    if (!stream->driver) {
        VIR_DEBUG("aborting unused stream");
        return 0;
    }

    if (stream->driver->streamAbort) {
        int ret;
        ret = (stream->driver->streamAbort)(stream);
        if (ret < 0)
            goto error;
        return ret;
    }

    virReportUnsupportedError();

 error:
    virDispatchError(stream->conn);
    return -1;
}


/**
 * virStreamFree:
 * @stream: pointer to the stream object
 *
 * Decrement the reference count on a stream, releasing
 * the stream object if the reference count has hit zero.
 *
 * There must not be an active data transfer in progress
 * when releasing the stream. If a stream needs to be
 * disposed of prior to end of stream being reached, then
 * the virStreamAbort function should be called first.
 *
 * Returns 0 upon success, or -1 on error
 */
int
virStreamFree(virStreamPtr stream)
{
    VIR_DEBUG("stream=%p", stream);

    virResetLastError();

    virCheckStreamReturn(stream, -1);

    /* XXX Enforce shutdown before free'ing resources ? */

    virObjectUnref(stream);
    return 0;
}
