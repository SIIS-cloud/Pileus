/*
 * bridge_driver.c: core driver methods for managing network
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

#include <sys/types.h>
#include <sys/poll.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <paths.h>
#include <pwd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <dirent.h>
#if HAVE_SYS_SYSCTL_H
# include <sys/sysctl.h>
#endif

#include "virerror.h"
#include "datatypes.h"
#include "bridge_driver.h"
#include "bridge_driver_platform.h"
#include "network_conf.h"
#include "device_conf.h"
#include "driver.h"
#include "virbuffer.h"
#include "virpidfile.h"
#include "vircommand.h"
#include "viralloc.h"
#include "viruuid.h"
#include "viriptables.h"
#include "virlog.h"
#include "virdnsmasq.h"
#include "configmake.h"
#include "virnetdev.h"
#include "virpci.h"
#include "virnetdevbridge.h"
#include "virnetdevtap.h"
#include "virnetdevvportprofile.h"
#include "virdbus.h"
#include "virfile.h"
#include "virstring.h"
#include "viraccessapicheck.h"
#include "network_event.h"
#include "virhook.h"
#include "virjson.h"

#define VIR_FROM_THIS VIR_FROM_NETWORK

/**
 * VIR_NETWORK_DHCP_LEASE_FILE_SIZE_MAX:
 *
 * Macro providing the upper limit on the size of leases file
 */
#define VIR_NETWORK_DHCP_LEASE_FILE_SIZE_MAX (32 * 1024 * 1024)

VIR_LOG_INIT("network.bridge_driver");

static virNetworkDriverStatePtr driver;


static void networkDriverLock(void)
{
    virMutexLock(&driver->lock);
}
static void networkDriverUnlock(void)
{
    virMutexUnlock(&driver->lock);
}

static int networkStateCleanup(void);

static int networkStartNetwork(virNetworkObjPtr network);

static int networkShutdownNetwork(virNetworkObjPtr network);

static int networkStartNetworkVirtual(virNetworkObjPtr network);

static int networkShutdownNetworkVirtual(virNetworkObjPtr network);

static int networkStartNetworkExternal(virNetworkObjPtr network);

static int networkShutdownNetworkExternal(virNetworkObjPtr network);

static void networkReloadFirewallRules(void);
static void networkRefreshDaemons(void);

static int networkPlugBandwidth(virNetworkObjPtr net,
                                virDomainNetDefPtr iface);
static int networkUnplugBandwidth(virNetworkObjPtr net,
                                  virDomainNetDefPtr iface);

static void networkNetworkObjTaint(virNetworkObjPtr net,
                                   virNetworkTaintFlags taint);

static virNetworkObjPtr
networkObjFromNetwork(virNetworkPtr net)
{
    virNetworkObjPtr network;
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    networkDriverLock();
    network = virNetworkFindByUUID(&driver->networks, net->uuid);
    networkDriverUnlock();

    if (!network) {
        virUUIDFormat(net->uuid, uuidstr);
        virReportError(VIR_ERR_NO_NETWORK,
                       _("no network with matching uuid '%s' (%s)"),
                       uuidstr, net->name);
    }

    return network;
}

static int
networkRunHook(virNetworkObjPtr network,
               virDomainDefPtr dom,
               virDomainNetDefPtr iface,
               int op,
               int sub_op)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *xml = NULL, *net_xml = NULL, *dom_xml = NULL;
    int hookret;
    int ret = -1;

    if (virHookPresent(VIR_HOOK_DRIVER_NETWORK)) {
        if (!network) {
            VIR_DEBUG("Not running hook as @network is NULL");
            ret = 0;
            goto cleanup;
        }

        virBufferAddLit(&buf, "<hookData>\n");
        virBufferAdjustIndent(&buf, 2);
        if (iface && virDomainNetDefFormat(&buf, iface, 0) < 0)
            goto cleanup;
        if (virNetworkDefFormatBuf(&buf, network->def, 0) < 0)
            goto cleanup;
        if (dom && virDomainDefFormatInternal(dom, 0, &buf) < 0)
            goto cleanup;

        virBufferAdjustIndent(&buf, -2);
        virBufferAddLit(&buf, "</hookData>");

        if (virBufferCheckError(&buf) < 0)
            goto cleanup;

        xml = virBufferContentAndReset(&buf);
        hookret = virHookCall(VIR_HOOK_DRIVER_NETWORK, network->def->name,
                              op, sub_op, NULL, xml, NULL);

        /*
         * If the script raised an error, pass it to the callee.
         */
        if (hookret < 0)
            goto cleanup;

        networkNetworkObjTaint(network, VIR_NETWORK_TAINT_HOOK);
    }

    ret = 0;
 cleanup:
    virBufferFreeAndReset(&buf);
    VIR_FREE(xml);
    VIR_FREE(net_xml);
    VIR_FREE(dom_xml);
    return ret;
}

static char *
networkDnsmasqLeaseFileNameDefault(const char *netname)
{
    char *leasefile;

    ignore_value(virAsprintf(&leasefile, "%s/%s.leases",
                             driver->dnsmasqStateDir, netname));
    return leasefile;
}

static char *
networkDnsmasqLeaseFileNameCustom(const char *bridge)
{
    char *leasefile;

    ignore_value(virAsprintf(&leasefile, "%s/%s.status",
                             driver->dnsmasqStateDir, bridge));
    return leasefile;
}

static char *
networkDnsmasqConfigFileName(const char *netname)
{
    char *conffile;

    ignore_value(virAsprintf(&conffile, "%s/%s.conf",
                             driver->dnsmasqStateDir, netname));
    return conffile;
}

static char *
networkRadvdPidfileBasename(const char *netname)
{
    /* this is simple but we want to be sure it's consistently done */
    char *pidfilebase;

    ignore_value(virAsprintf(&pidfilebase, "%s-radvd", netname));
    return pidfilebase;
}

static char *
networkRadvdConfigFileName(const char *netname)
{
    char *configfile;

    ignore_value(virAsprintf(&configfile, "%s/%s-radvd.conf",
                             driver->radvdStateDir, netname));
    return configfile;
}

/* do needed cleanup steps and remove the network from the list */
static int
networkRemoveInactive(virNetworkObjPtr net)
{
    char *leasefile = NULL;
    char *customleasefile = NULL;
    char *radvdconfigfile = NULL;
    char *configfile = NULL;
    char *radvdpidbase = NULL;
    char *statusfile = NULL;
    dnsmasqContext *dctx = NULL;
    virNetworkDefPtr def = virNetworkObjGetPersistentDef(net);

    int ret = -1;

    /* remove the (possibly) existing dnsmasq and radvd files */
    if (!(dctx = dnsmasqContextNew(def->name,
                                   driver->dnsmasqStateDir))) {
        goto cleanup;
    }

    if (!(leasefile = networkDnsmasqLeaseFileNameDefault(def->name)))
        goto cleanup;

    if (!(customleasefile = networkDnsmasqLeaseFileNameCustom(def->bridge)))
        goto cleanup;

    if (!(radvdconfigfile = networkRadvdConfigFileName(def->name)))
        goto cleanup;

    if (!(radvdpidbase = networkRadvdPidfileBasename(def->name)))
        goto cleanup;

    if (!(configfile = networkDnsmasqConfigFileName(def->name)))
        goto cleanup;

    if (!(statusfile
          = virNetworkConfigFile(driver->stateDir, def->name)))
        goto cleanup;

    /* dnsmasq */
    dnsmasqDelete(dctx);
    unlink(leasefile);
    unlink(customleasefile);
    unlink(configfile);

    /* radvd */
    unlink(radvdconfigfile);
    virPidFileDelete(driver->pidDir, radvdpidbase);

    /* remove status file */
    unlink(statusfile);

    /* remove the network definition */
    virNetworkRemoveInactive(&driver->networks, net);

    ret = 0;

 cleanup:
    VIR_FREE(leasefile);
    VIR_FREE(configfile);
    VIR_FREE(customleasefile);
    VIR_FREE(radvdconfigfile);
    VIR_FREE(radvdpidbase);
    VIR_FREE(statusfile);
    dnsmasqContextFree(dctx);
    return ret;
}

static char *
networkBridgeDummyNicName(const char *brname)
{
    static const char dummyNicSuffix[] = "-nic";
    char *nicname;

    if (strlen(brname) + sizeof(dummyNicSuffix) > IFNAMSIZ) {
        /* because the length of an ifname is limited to IFNAMSIZ-1
         * (usually 15), and we're adding 4 more characters, we must
         * truncate the original name to 11 to fit. In order to catch
         * a possible numeric ending (eg virbr0, virbr1, etc), we grab
         * the first 8 and last 3 characters of the string.
         */
        ignore_value(virAsprintf(&nicname, "%.*s%s%s",
                                 /* space for last 3 chars + "-nic" + NULL */
                                 (int)(IFNAMSIZ - (3 + sizeof(dummyNicSuffix))),
                                 brname, brname + strlen(brname) - 3,
                                 dummyNicSuffix));
    } else {
        ignore_value(virAsprintf(&nicname, "%s%s", brname, dummyNicSuffix));
    }
    return nicname;
}

/* Update the internal status of all allegedly active networks
 * according to external conditions on the host (i.e. anything that
 * isn't stored directly in each network's state file). */
static void
networkUpdateAllState(void)
{
    size_t i;

    for (i = 0; i < driver->networks.count; i++) {
        virNetworkObjPtr obj = driver->networks.objs[i];

        if (!obj->active)
            continue;

        virNetworkObjLock(obj);

        switch (obj->def->forward.type) {
        case VIR_NETWORK_FORWARD_NONE:
        case VIR_NETWORK_FORWARD_NAT:
        case VIR_NETWORK_FORWARD_ROUTE:
            /* If bridge doesn't exist, then mark it inactive */
            if (!(obj->def->bridge && virNetDevExists(obj->def->bridge) == 1))
                obj->active = 0;
            break;

        case VIR_NETWORK_FORWARD_BRIDGE:
            if (obj->def->bridge) {
                if (virNetDevExists(obj->def->bridge) != 1)
                    obj->active = 0;
                break;
            }
            /* intentionally drop through to common case for all
             * macvtap networks (forward='bridge' with no bridge
             * device defined is macvtap using its 'bridge' mode)
             */
        case VIR_NETWORK_FORWARD_PRIVATE:
        case VIR_NETWORK_FORWARD_VEPA:
        case VIR_NETWORK_FORWARD_PASSTHROUGH:
            /* so far no extra checks */
            break;

        case VIR_NETWORK_FORWARD_HOSTDEV:
            /* so far no extra checks */
            break;
        }

        /* Try and read dnsmasq/radvd pids of active networks */
        if (obj->active && obj->def->ips && (obj->def->nips > 0)) {
            char *radvdpidbase;

            ignore_value(virPidFileReadIfAlive(driver->pidDir,
                                               obj->def->name,
                                               &obj->dnsmasqPid,
                                               dnsmasqCapsGetBinaryPath(driver->dnsmasqCaps)));
            radvdpidbase = networkRadvdPidfileBasename(obj->def->name);
            if (!radvdpidbase)
                break;
            ignore_value(virPidFileReadIfAlive(driver->pidDir,
                                               radvdpidbase,
                                               &obj->radvdPid, RADVD));
            VIR_FREE(radvdpidbase);
        }

        virNetworkObjUnlock(obj);
    }

    /* remove inactive transient networks */
    i = 0;
    while (i < driver->networks.count) {
        virNetworkObjPtr obj = driver->networks.objs[i];
        virNetworkObjLock(obj);

        if (!obj->persistent && !obj->active) {
            networkRemoveInactive(obj);
            continue;
        }

        virNetworkObjUnlock(obj);
        i++;
    }
}


static void
networkAutostartConfigs(void)
{
    size_t i;

    for (i = 0; i < driver->networks.count; i++) {
        virNetworkObjLock(driver->networks.objs[i]);
        if (driver->networks.objs[i]->autostart &&
            !virNetworkObjIsActive(driver->networks.objs[i])) {
            if (networkStartNetwork(driver->networks.objs[i]) < 0) {
                /* failed to start but already logged */
            }
        }
        virNetworkObjUnlock(driver->networks.objs[i]);
    }
}

#if HAVE_FIREWALLD
static DBusHandlerResult
firewalld_dbus_filter_bridge(DBusConnection *connection ATTRIBUTE_UNUSED,
                             DBusMessage *message, void *user_data ATTRIBUTE_UNUSED)
{
    if (dbus_message_is_signal(message, DBUS_INTERFACE_DBUS,
                               "NameOwnerChanged") ||
        dbus_message_is_signal(message, "org.fedoraproject.FirewallD1",
                               "Reloaded"))
    {
        VIR_DEBUG("Reload in bridge_driver because of firewalld.");
        networkReloadFirewallRules();
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}
#endif

static int
networkMigrateStateFiles(void)
{
    /* Due to a change in location of network state xml beginning in
     * libvirt 1.2.4 (from /var/lib/libvirt/network to
     * /var/run/libvirt/network), we must check for state files in two
     * locations. Anything found in the old location must be written
     * to the new location, then erased from the old location. (Note
     * that we read/write the file rather than calling rename()
     * because the old and new state directories are likely in
     * different filesystems).
     */
    int ret = -1;
    const char *oldStateDir = LOCALSTATEDIR "/lib/libvirt/network";
    DIR *dir;
    int direrr;
    struct dirent *entry;
    char *oldPath = NULL, *newPath = NULL;
    char *contents = NULL;

    if (!(dir = opendir(oldStateDir))) {
        if (errno == ENOENT)
            return 0;

        virReportSystemError(errno, _("failed to open directory '%s'"),
                             oldStateDir);
        return -1;
    }

    if (virFileMakePath(driver->stateDir) < 0) {
        virReportSystemError(errno, _("cannot create directory %s"),
                             driver->stateDir);
        goto cleanup;
    }

    while ((direrr = virDirRead(dir, &entry, oldStateDir)) > 0) {
        if (entry->d_type != DT_UNKNOWN &&
            entry->d_type != DT_REG)
            continue;

        if (STREQ(entry->d_name, ".") ||
            STREQ(entry->d_name, ".."))
            continue;

        if (virAsprintf(&oldPath, "%s/%s",
                        oldStateDir, entry->d_name) < 0)
            goto cleanup;

        if (entry->d_type == DT_UNKNOWN) {
            struct stat st;

            if (lstat(oldPath, &st) < 0) {
                virReportSystemError(errno,
                                     _("failed to stat network status file '%s'"),
                                     oldPath);
                goto cleanup;
            }

            if (!S_ISREG(st.st_mode)) {
                VIR_FREE(oldPath);
                continue;
            }
        }

        if (virFileReadAll(oldPath, 1024*1024, &contents) < 0)
            goto cleanup;

        if (virAsprintf(&newPath, "%s/%s",
                        driver->stateDir, entry->d_name) < 0)
            goto cleanup;
        if (virFileWriteStr(newPath, contents, S_IRUSR | S_IWUSR) < 0) {
            virReportSystemError(errno,
                                 _("failed to write network status file '%s'"),
                                 newPath);
            goto cleanup;
        }

        unlink(oldPath);
        VIR_FREE(oldPath);
        VIR_FREE(newPath);
        VIR_FREE(contents);
    }
    if (direrr < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    closedir(dir);
    VIR_FREE(oldPath);
    VIR_FREE(newPath);
    VIR_FREE(contents);
    return ret;
}

/**
 * networkStateInitialize:
 *
 * Initialization function for the QEmu daemon
 */
static int
networkStateInitialize(bool privileged,
                       virStateInhibitCallback callback ATTRIBUTE_UNUSED,
                       void *opaque ATTRIBUTE_UNUSED)
{
    int ret = -1;
    char *configdir = NULL;
    char *rundir = NULL;
#ifdef HAVE_FIREWALLD
    DBusConnection *sysbus = NULL;
#endif

    if (VIR_ALLOC(driver) < 0)
        goto error;

    if (virMutexInit(&driver->lock) < 0) {
        VIR_FREE(driver);
        goto error;
    }
    networkDriverLock();

    /* configuration/state paths are one of
     * ~/.config/libvirt/... (session/unprivileged)
     * /etc/libvirt/... && /var/(run|lib)/libvirt/... (system/privileged).
     */
    if (privileged) {
        if (VIR_STRDUP(driver->networkConfigDir,
                       SYSCONFDIR "/libvirt/qemu/networks") < 0 ||
            VIR_STRDUP(driver->networkAutostartDir,
                       SYSCONFDIR "/libvirt/qemu/networks/autostart") < 0 ||
            VIR_STRDUP(driver->stateDir,
                       LOCALSTATEDIR "/run/libvirt/network") < 0 ||
            VIR_STRDUP(driver->pidDir,
                       LOCALSTATEDIR "/run/libvirt/network") < 0 ||
            VIR_STRDUP(driver->dnsmasqStateDir,
                       LOCALSTATEDIR "/lib/libvirt/dnsmasq") < 0 ||
            VIR_STRDUP(driver->radvdStateDir,
                       LOCALSTATEDIR "/lib/libvirt/radvd") < 0)
            goto error;

        /* migration from old to new location is only applicable for
         * privileged mode - unprivileged mode directories haven't
         * changed location.
         */
        if (networkMigrateStateFiles() < 0)
            goto error;
    } else {
        configdir = virGetUserConfigDirectory();
        rundir = virGetUserRuntimeDirectory();
        if (!(configdir && rundir))
            goto error;

        if ((virAsprintf(&driver->networkConfigDir,
                         "%s/qemu/networks", configdir) < 0) ||
            (virAsprintf(&driver->networkAutostartDir,
                         "%s/qemu/networks/autostart", configdir) < 0) ||
            (virAsprintf(&driver->stateDir,
                         "%s/network/lib", rundir) < 0) ||
            (virAsprintf(&driver->pidDir,
                         "%s/network/run", rundir) < 0) ||
            (virAsprintf(&driver->dnsmasqStateDir,
                         "%s/dnsmasq/lib", rundir) < 0) ||
            (virAsprintf(&driver->radvdStateDir,
                         "%s/radvd/lib", rundir) < 0)) {
            goto error;
        }
    }

    if (virFileMakePath(driver->stateDir) < 0) {
        virReportSystemError(errno,
                             _("cannot create directory %s"),
                             driver->stateDir);
        goto error;
    }

    /* if this fails now, it will be retried later with dnsmasqCapsRefresh() */
    driver->dnsmasqCaps = dnsmasqCapsNewFromBinary(DNSMASQ);

    if (virNetworkLoadAllState(&driver->networks,
                               driver->stateDir) < 0)
        goto error;

    if (virNetworkLoadAllConfigs(&driver->networks,
                                 driver->networkConfigDir,
                                 driver->networkAutostartDir) < 0)
        goto error;

    networkUpdateAllState();
    networkReloadFirewallRules();
    networkRefreshDaemons();

    driver->networkEventState = virObjectEventStateNew();

    networkDriverUnlock();

#ifdef HAVE_FIREWALLD
    if (!(sysbus = virDBusGetSystemBus())) {
        virErrorPtr err = virGetLastError();
        VIR_WARN("DBus not available, disabling firewalld support "
                 "in bridge_driver: %s", err->message);
    } else {
        /* add matches for
         * NameOwnerChanged on org.freedesktop.DBus for firewalld start/stop
         * Reloaded on org.fedoraproject.FirewallD1 for firewalld reload
         */
        dbus_bus_add_match(sysbus,
                           "type='signal'"
                           ",interface='"DBUS_INTERFACE_DBUS"'"
                           ",member='NameOwnerChanged'"
                           ",arg0='org.fedoraproject.FirewallD1'",
                           NULL);
        dbus_bus_add_match(sysbus,
                           "type='signal'"
                           ",interface='org.fedoraproject.FirewallD1'"
                           ",member='Reloaded'",
                           NULL);
        dbus_connection_add_filter(sysbus, firewalld_dbus_filter_bridge,
                                   NULL, NULL);
    }
#endif

    ret = 0;
 cleanup:
    VIR_FREE(configdir);
    VIR_FREE(rundir);
    return ret;

 error:
    if (driver)
        networkDriverUnlock();
    networkStateCleanup();
    goto cleanup;
}

/**
 * networkStateAutoStart:
 *
 * Function to AutoStart the bridge configs
 */
static void
networkStateAutoStart(void)
{
    if (!driver)
        return;

    networkDriverLock();
    networkAutostartConfigs();
    networkDriverUnlock();
}

/**
 * networkStateReload:
 *
 * Function to restart the QEmu daemon, it will recheck the configuration
 * files and update its state and the networking
 */
static int
networkStateReload(void)
{
    if (!driver)
        return 0;

    networkDriverLock();
    virNetworkLoadAllState(&driver->networks,
                           driver->stateDir);
    virNetworkLoadAllConfigs(&driver->networks,
                             driver->networkConfigDir,
                             driver->networkAutostartDir);
    networkReloadFirewallRules();
    networkRefreshDaemons();
    networkAutostartConfigs();
    networkDriverUnlock();
    return 0;
}


/**
 * networkStateCleanup:
 *
 * Shutdown the QEmu daemon, it will stop all active domains and networks
 */
static int
networkStateCleanup(void)
{
    if (!driver)
        return -1;

    networkDriverLock();

    virObjectEventStateFree(driver->networkEventState);

    /* free inactive networks */
    virNetworkObjListFree(&driver->networks);

    VIR_FREE(driver->networkConfigDir);
    VIR_FREE(driver->networkAutostartDir);
    VIR_FREE(driver->stateDir);
    VIR_FREE(driver->pidDir);
    VIR_FREE(driver->dnsmasqStateDir);
    VIR_FREE(driver->radvdStateDir);

    virObjectUnref(driver->dnsmasqCaps);

    networkDriverUnlock();
    virMutexDestroy(&driver->lock);

    VIR_FREE(driver);

    return 0;
}


/* networkKillDaemon:
 *
 * kill the specified pid/name, and wait a bit to make sure it's dead.
 */
static int
networkKillDaemon(pid_t pid, const char *daemonName, const char *networkName)
{
    size_t i;
    int ret = -1;
    const char *signame = "TERM";

    /* send SIGTERM, then wait up to 3 seconds for the process to
     * disappear, send SIGKILL, then wait for up to another 2
     * seconds. If that fails, log a warning and continue, hoping
     * for the best.
     */
    for (i = 0; i < 25; i++) {
        int signum = 0;
        if (i == 0) {
            signum = SIGTERM;
        } else if (i == 15) {
            signum = SIGKILL;
            signame = "KILL";
        }
        if (kill(pid, signum) < 0) {
            if (errno == ESRCH) {
                ret = 0;
            } else {
                char ebuf[1024];
                VIR_WARN("Failed to terminate %s process %d "
                         "for network '%s' with SIG%s: %s",
                         daemonName, pid, networkName, signame,
                         virStrerror(errno, ebuf, sizeof(ebuf)));
            }
            goto cleanup;
        }
        /* NB: since networks have no reference count like
         * domains, there is no safe way to unlock the network
         * object temporarily, and so we can't follow the
         * procedure used by the qemu driver of 1) unlock driver
         * 2) sleep, 3) add ref to object 4) unlock object, 5)
         * re-lock driver, 6) re-lock object. We may need to add
         * that functionality eventually, but for now this
         * function is rarely used and, at worst, leaving the
         * network driver locked during this loop of sleeps will
         * have the effect of holding up any other thread trying
         * to make modifications to a network for up to 5 seconds;
         * since modifications to networks are much less common
         * than modifications to domains, this seems a reasonable
         * tradeoff in exchange for less code disruption.
         */
        usleep(20 * 1000);
    }
    VIR_WARN("Timed out waiting after SIG%s to %s process %d "
             "(network '%s')",
             signame, daemonName, pid, networkName);
 cleanup:
    return ret;
}

/* the following does not build a file, it builds a list
 * which is later saved into a file
 */

static int
networkBuildDnsmasqDhcpHostsList(dnsmasqContext *dctx,
                                 virNetworkIpDefPtr ipdef)
{
    size_t i;
    bool ipv6 = false;

    if (VIR_SOCKET_ADDR_IS_FAMILY(&ipdef->address, AF_INET6))
        ipv6 = true;
    for (i = 0; i < ipdef->nhosts; i++) {
        virNetworkDHCPHostDefPtr host = &(ipdef->hosts[i]);
        if (VIR_SOCKET_ADDR_VALID(&host->ip))
            if (dnsmasqAddDhcpHost(dctx, host->mac, &host->ip,
                                   host->name, host->id, ipv6) < 0)
                return -1;
    }

    return 0;
}

static int
networkBuildDnsmasqHostsList(dnsmasqContext *dctx,
                             virNetworkDNSDefPtr dnsdef)
{
    size_t i, j;

    if (dnsdef) {
        for (i = 0; i < dnsdef->nhosts; i++) {
            virNetworkDNSHostDefPtr host = &(dnsdef->hosts[i]);
            if (VIR_SOCKET_ADDR_VALID(&host->ip)) {
                for (j = 0; j < host->nnames; j++)
                    if (dnsmasqAddHost(dctx, &host->ip, host->names[j]) < 0)
                        return -1;
            }
        }
    }

    return 0;
}


int
networkDnsmasqConfContents(virNetworkObjPtr network,
                           const char *pidfile,
                           char **configstr,
                           dnsmasqContext *dctx,
                           dnsmasqCapsPtr caps ATTRIBUTE_UNUSED)
{
    virBuffer configbuf = VIR_BUFFER_INITIALIZER;
    int r, ret = -1;
    int nbleases = 0;
    size_t i;
    virNetworkDNSDefPtr dns = &network->def->dns;
    virNetworkIpDefPtr tmpipdef, ipdef, ipv4def, ipv6def;
    bool ipv6SLAAC;

    *configstr = NULL;

    /*
     * All dnsmasq parameters are put into a configuration file, except the
     * command line --conf-file=parameter which specifies the location of
     * configuration file.
     *
     * All dnsmasq conf-file parameters must be specified as "foo=bar"
     * as oppose to "--foo bar" which was acceptable on the command line.
     */

    /*
     * Needed to ensure dnsmasq uses same algorithm for processing
     * multiple namedriver entries in /etc/resolv.conf as GLibC.
     */

    /* create dnsmasq config file appropriate for this network */
    virBufferAsprintf(&configbuf,
                      "##WARNING:  THIS IS AN AUTO-GENERATED FILE. "
                      "CHANGES TO IT ARE LIKELY TO BE\n"
                      "##OVERWRITTEN AND LOST.  Changes to this "
                      "configuration should be made using:\n"
                      "##    virsh net-edit %s\n"
                      "## or other application using the libvirt API.\n"
                      "##\n## dnsmasq conf file created by libvirt\n"
                      "strict-order\n",
                      network->def->name);

    if (network->def->dns.forwarders) {
        virBufferAddLit(&configbuf, "no-resolv\n");
        for (i = 0; i < network->def->dns.nfwds; i++) {
            virBufferAsprintf(&configbuf, "server=%s\n",
                              network->def->dns.forwarders[i]);
        }
    }

    if (network->def->domain) {
        if (network->def->domainLocalOnly == VIR_TRISTATE_BOOL_YES) {
            virBufferAsprintf(&configbuf,
                              "local=/%s/\n",
                              network->def->domain);
        }
        virBufferAsprintf(&configbuf,
                          "domain=%s\n"
                          "expand-hosts\n",
                          network->def->domain);
    }

    if (network->def->dns.forwardPlainNames == VIR_TRISTATE_BOOL_NO) {
        virBufferAddLit(&configbuf, "domain-needed\n");
        /* need to specify local=// whether or not a domain is
         * specified, unless the config says we should forward "plain"
         * names (i.e. not fully qualified, no '.' characters)
         */
        virBufferAddLit(&configbuf, "local=//\n");
    }

    if (pidfile)
        virBufferAsprintf(&configbuf, "pid-file=%s\n", pidfile);

    /* dnsmasq will *always* listen on localhost unless told otherwise */
    virBufferAddLit(&configbuf, "except-interface=lo\n");

    if (dnsmasqCapsGet(caps, DNSMASQ_CAPS_BIND_DYNAMIC)) {
        /* using --bind-dynamic with only --interface (no
         * --listen-address) prevents dnsmasq from responding to dns
         * queries that arrive on some interface other than our bridge
         * interface (in other words, requests originating somewhere
         * other than one of the virtual guests connected directly to
         * this network). This was added in response to CVE 2012-3411.
         */
        virBufferAsprintf(&configbuf,
                          "bind-dynamic\n"
                          "interface=%s\n",
                          network->def->bridge);
    } else {
        virBufferAddLit(&configbuf, "bind-interfaces\n");
        /*
         * --interface does not actually work with dnsmasq < 2.47,
         * due to DAD for ipv6 addresses on the interface.
         *
         * virCommandAddArgList(cmd, "--interface", network->def->bridge, NULL);
         *
         * So listen on all defined IPv[46] addresses
         */
        for (i = 0;
             (tmpipdef = virNetworkDefGetIpByIndex(network->def, AF_UNSPEC, i));
             i++) {
            char *ipaddr = virSocketAddrFormat(&tmpipdef->address);

            if (!ipaddr)
                goto cleanup;

            /* also part of CVE 2012-3411 - if the host's version of
             * dnsmasq doesn't have bind-dynamic, only allow listening on
             * private/local IP addresses (see RFC1918/RFC3484/RFC4193)
             */
            if (!dnsmasqCapsGet(caps, DNSMASQ_CAPS_BINDTODEVICE) &&
                !virSocketAddrIsPrivate(&tmpipdef->address)) {
                unsigned long version = dnsmasqCapsGetVersion(caps);

                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("Publicly routable address %s is prohibited. "
                                 "The version of dnsmasq on this host (%d.%d) "
                                 "doesn't support the bind-dynamic option or "
                                 "use SO_BINDTODEVICE on listening sockets, "
                                 "one of which is required for safe operation "
                                 "on a publicly routable subnet "
                                 "(see CVE-2012-3411). You must either "
                                 "upgrade dnsmasq, or use a private/local "
                                 "subnet range for this network "
                                 "(as described in RFC1918/RFC3484/RFC4193)."),
                               ipaddr, (int)version / 1000000,
                               (int)(version % 1000000) / 1000);
                VIR_FREE(ipaddr);
                goto cleanup;
            }
            virBufferAsprintf(&configbuf, "listen-address=%s\n", ipaddr);
            VIR_FREE(ipaddr);
        }
    }

    /* If this is an isolated network, set the default route option
     * (3) to be empty to avoid setting a default route that's
     * guaranteed to not work, and set no-resolv so that no dns
     * requests are forwarded on to the dns server listed in the
     * host's /etc/resolv.conf (since this could be used as a channel
     * to build a connection to the outside).
     */
    if (network->def->forward.type == VIR_NETWORK_FORWARD_NONE) {
        virBufferAddLit(&configbuf, "dhcp-option=3\n"
                        "no-resolv\n");
    }

    for (i = 0; i < dns->ntxts; i++) {
        virBufferAsprintf(&configbuf, "txt-record=%s,%s\n",
                          dns->txts[i].name,
                          dns->txts[i].value);
    }

    for (i = 0; i < dns->nsrvs; i++) {
        /* service/protocol are required, and should have been validated
         * by the parser.
         */
        if (!dns->srvs[i].service) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Missing required 'service' "
                             "attribute in SRV record of network '%s'"),
                           network->def->name);
            goto cleanup;
        }
        if (!dns->srvs[i].protocol) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Missing required 'service' "
                             "attribute in SRV record of network '%s'"),
                           network->def->name);
            goto cleanup;
        }
        /* RFC2782 requires that service and protocol be preceded by
         * an underscore.
         */
        virBufferAsprintf(&configbuf, "srv-host=_%s._%s",
                          dns->srvs[i].service, dns->srvs[i].protocol);

        /* domain is optional - it defaults to the domain of this network */
        if (dns->srvs[i].domain)
            virBufferAsprintf(&configbuf, ".%s", dns->srvs[i].domain);

        /* If target is empty or ".", that means "the service is
         * decidedly not available at this domain" (RFC2782). In that
         * case, any port, priority, or weight is irrelevant.
         */
        if (dns->srvs[i].target && STRNEQ(dns->srvs[i].target, ".")) {

            virBufferAsprintf(&configbuf, ",%s", dns->srvs[i].target);
            /* port, priority, and weight are optional, but are
             * identified by their position in the line. If an item is
             * unspecified, but something later in the line *is*
             * specified, we need to give the default value for the
             * unspecified item. (According to the dnsmasq manpage,
             * the default for port is 1).
             */
            if (dns->srvs[i].port ||
                dns->srvs[i].priority || dns->srvs[i].weight)
                virBufferAsprintf(&configbuf, ",%d",
                                  dns->srvs[i].port ? dns->srvs[i].port : 1);
            if (dns->srvs[i].priority || dns->srvs[i].weight)
                virBufferAsprintf(&configbuf, ",%d", dns->srvs[i].priority);
            if (dns->srvs[i].weight)
                virBufferAsprintf(&configbuf, ",%d", dns->srvs[i].weight);
        }
        virBufferAddLit(&configbuf, "\n");
    }

    /* Find the first dhcp for both IPv4 and IPv6 */
    for (i = 0, ipv4def = NULL, ipv6def = NULL, ipv6SLAAC = false;
         (ipdef = virNetworkDefGetIpByIndex(network->def, AF_UNSPEC, i));
         i++) {
        if (VIR_SOCKET_ADDR_IS_FAMILY(&ipdef->address, AF_INET)) {
            if (ipdef->nranges || ipdef->nhosts) {
                if (ipv4def) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("For IPv4, multiple DHCP definitions "
                                     "cannot be specified."));
                    goto cleanup;
                } else {
                    ipv4def = ipdef;
                }
            }
        }
        if (VIR_SOCKET_ADDR_IS_FAMILY(&ipdef->address, AF_INET6)) {
            if (ipdef->nranges || ipdef->nhosts) {
                if (!DNSMASQ_DHCPv6_SUPPORT(caps)) {
                    unsigned long version = dnsmasqCapsGetVersion(caps);
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                   _("The version of dnsmasq on this host "
                                     "(%d.%d) doesn't adequately support "
                                     "IPv6 dhcp range or dhcp host "
                                     "specification. Version %d.%d or later "
                                     "is required."),
                                   (int)version / 1000000,
                                   (int)(version % 1000000) / 1000,
                                   DNSMASQ_DHCPv6_MAJOR_REQD,
                                   DNSMASQ_DHCPv6_MINOR_REQD);
                    goto cleanup;
                }
                if (ipv6def) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("For IPv6, multiple DHCP definitions "
                                     "cannot be specified."));
                    goto cleanup;
                } else {
                    ipv6def = ipdef;
                }
            } else {
                ipv6SLAAC = true;
            }
        }
    }

    if (ipv6def && ipv6SLAAC) {
        VIR_WARN("For IPv6, when DHCP is specified for one address, then "
                 "state-full Router Advertising will occur.  The additional "
                 "IPv6 addresses specified require manually configured guest "
                 "network to work properly since both state-full (DHCP) "
                 "and state-less (SLAAC) addressing are not supported "
                 "on the same network interface.");
    }

    ipdef = ipv4def ? ipv4def : ipv6def;

    while (ipdef) {
        for (r = 0; r < ipdef->nranges; r++) {
            char *saddr = virSocketAddrFormat(&ipdef->ranges[r].start);
            if (!saddr)
                goto cleanup;
            char *eaddr = virSocketAddrFormat(&ipdef->ranges[r].end);
            if (!eaddr) {
                VIR_FREE(saddr);
                goto cleanup;
            }
            virBufferAsprintf(&configbuf, "dhcp-range=%s,%s\n",
                              saddr, eaddr);
            VIR_FREE(saddr);
            VIR_FREE(eaddr);
            nbleases += virSocketAddrGetRange(&ipdef->ranges[r].start,
                                              &ipdef->ranges[r].end);
        }

        /*
         * For static-only DHCP, i.e. with no range but at least one
         * host element, we have to add a special --dhcp-range option
         * to enable the service in dnsmasq. (this is for dhcp-hosts=
         * support)
         */
        if (!ipdef->nranges && ipdef->nhosts) {
            char *bridgeaddr = virSocketAddrFormat(&ipdef->address);
            if (!bridgeaddr)
                goto cleanup;
            virBufferAsprintf(&configbuf, "dhcp-range=%s,static\n", bridgeaddr);
            VIR_FREE(bridgeaddr);
        }

        if (networkBuildDnsmasqDhcpHostsList(dctx, ipdef) < 0)
            goto cleanup;

        /* Note: the following is IPv4 only */
        if (VIR_SOCKET_ADDR_IS_FAMILY(&ipdef->address, AF_INET)) {
            if (ipdef->nranges || ipdef->nhosts)
                virBufferAddLit(&configbuf, "dhcp-no-override\n");

            if (ipdef->tftproot) {
                virBufferAddLit(&configbuf, "enable-tftp\n");
                virBufferAsprintf(&configbuf, "tftp-root=%s\n", ipdef->tftproot);
            }

            if (ipdef->bootfile) {
                if (VIR_SOCKET_ADDR_VALID(&ipdef->bootserver)) {
                    char *bootserver = virSocketAddrFormat(&ipdef->bootserver);

                    if (!bootserver)
                        goto cleanup;
                    virBufferAsprintf(&configbuf, "dhcp-boot=%s%s%s\n",
                                      ipdef->bootfile, ",,", bootserver);
                    VIR_FREE(bootserver);
                } else {
                    virBufferAsprintf(&configbuf, "dhcp-boot=%s\n", ipdef->bootfile);
                }
            }
        }
        ipdef = (ipdef == ipv6def) ? NULL : ipv6def;
    }

    if (nbleases > 0)
        virBufferAsprintf(&configbuf, "dhcp-lease-max=%d\n", nbleases);

    /* this is done once per interface */
    if (networkBuildDnsmasqHostsList(dctx, dns) < 0)
        goto cleanup;

    /* Even if there are currently no static hosts, if we're
     * listening for DHCP, we should write a 0-length hosts
     * file to allow for runtime additions.
     */
    if (ipv4def || ipv6def)
        virBufferAsprintf(&configbuf, "dhcp-hostsfile=%s\n",
                          dctx->hostsfile->path);

    /* Likewise, always create this file and put it on the
     * commandline, to allow for runtime additions.
     */
    virBufferAsprintf(&configbuf, "addn-hosts=%s\n",
                      dctx->addnhostsfile->path);

    /* Are we doing RA instead of radvd? */
    if (DNSMASQ_RA_SUPPORT(caps)) {
        if (ipv6def) {
            virBufferAddLit(&configbuf, "enable-ra\n");
        } else {
            for (i = 0;
                 (ipdef = virNetworkDefGetIpByIndex(network->def, AF_INET6, i));
                 i++) {
                if (!(ipdef->nranges || ipdef->nhosts)) {
                    char *bridgeaddr = virSocketAddrFormat(&ipdef->address);
                    if (!bridgeaddr)
                        goto cleanup;
                    virBufferAsprintf(&configbuf,
                                      "dhcp-range=%s,ra-only\n", bridgeaddr);
                    VIR_FREE(bridgeaddr);
                }
            }
        }
    }

    if (!(*configstr = virBufferContentAndReset(&configbuf)))
        goto cleanup;

    ret = 0;

 cleanup:
    virBufferFreeAndReset(&configbuf);
    return ret;
}

/* build the dnsmasq command line */
static int ATTRIBUTE_NONNULL(2)
networkBuildDhcpDaemonCommandLine(virNetworkObjPtr network,
                                  virCommandPtr *cmdout,
                                  char *pidfile, dnsmasqContext *dctx,
                                  dnsmasqCapsPtr caps)
{
    virCommandPtr cmd = NULL;
    int ret = -1;
    char *configfile = NULL;
    char *configstr = NULL;
    char *leaseshelper_path = NULL;

    network->dnsmasqPid = -1;

    if (networkDnsmasqConfContents(network, pidfile, &configstr, dctx, caps) < 0)
        goto cleanup;
    if (!configstr)
        goto cleanup;

    /* construct the filename */
    if (!(configfile = networkDnsmasqConfigFileName(network->def->name)))
        goto cleanup;

    /* Write the file */
    if (virFileWriteStr(configfile, configstr, 0600) < 0) {
        virReportSystemError(errno,
                             _("couldn't write dnsmasq config file '%s'"),
                             configfile);
        goto cleanup;
    }

    /* This helper is used to create custom leases file for libvirt */
    if (!(leaseshelper_path = virFileFindResource("libvirt_leaseshelper",
                                                  "src",
                                                  LIBEXECDIR)))
        goto cleanup;

    cmd = virCommandNew(dnsmasqCapsGetBinaryPath(caps));
    virCommandAddArgFormat(cmd, "--conf-file=%s", configfile);
    /* Libvirt gains full control of leases database */
    virCommandAddArgFormat(cmd, "--leasefile-ro");
    virCommandAddArgFormat(cmd, "--dhcp-script=%s", leaseshelper_path);
    virCommandAddEnvPair(cmd, "VIR_BRIDGE_NAME", network->def->bridge);

    *cmdout = cmd;
    ret = 0;
 cleanup:
    VIR_FREE(configfile);
    VIR_FREE(configstr);
    VIR_FREE(leaseshelper_path);
    return ret;
}

static int
networkStartDhcpDaemon(virNetworkObjPtr network)
{
    virCommandPtr cmd = NULL;
    char *pidfile = NULL;
    int ret = -1;
    dnsmasqContext *dctx = NULL;

    if (!virNetworkDefGetIpByIndex(network->def, AF_UNSPEC, 0)) {
        /* no IP addresses, so we don't need to run */
        ret = 0;
        goto cleanup;
    }

    if (virFileMakePath(driver->pidDir) < 0) {
        virReportSystemError(errno,
                             _("cannot create directory %s"),
                             driver->pidDir);
        goto cleanup;
    }

    if (!(pidfile = virPidFileBuildPath(driver->pidDir,
                                        network->def->name)))
        goto cleanup;

    if (virFileMakePath(driver->dnsmasqStateDir) < 0) {
        virReportSystemError(errno,
                             _("cannot create directory %s"),
                             driver->dnsmasqStateDir);
        goto cleanup;
    }

    dctx = dnsmasqContextNew(network->def->name, driver->dnsmasqStateDir);
    if (dctx == NULL)
        goto cleanup;

    if (dnsmasqCapsRefresh(&driver->dnsmasqCaps, NULL) < 0)
        goto cleanup;

    ret = networkBuildDhcpDaemonCommandLine(network, &cmd, pidfile,
                                            dctx, driver->dnsmasqCaps);
    if (ret < 0)
        goto cleanup;

    ret = dnsmasqSave(dctx);
    if (ret < 0)
        goto cleanup;

    ret = virCommandRun(cmd, NULL);
    if (ret < 0)
        goto cleanup;

    /*
     * There really is no race here - when dnsmasq daemonizes, its
     * leader process stays around until its child has actually
     * written its pidfile. So by time virCommandRun exits it has
     * waitpid'd and guaranteed the proess has started and written a
     * pid
     */

    ret = virPidFileRead(driver->pidDir, network->def->name,
                         &network->dnsmasqPid);
    if (ret < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    VIR_FREE(pidfile);
    virCommandFree(cmd);
    dnsmasqContextFree(dctx);
    return ret;
}

/* networkRefreshDhcpDaemon:
 *  Update dnsmasq config files, then send a SIGHUP so that it rereads
 *  them.   This only works for the dhcp-hostsfile and the
 *  addn-hosts file.
 *
 *  Returns 0 on success, -1 on failure.
 */
static int
networkRefreshDhcpDaemon(virNetworkObjPtr network)
{
    int ret = -1;
    size_t i;
    virNetworkIpDefPtr ipdef, ipv4def, ipv6def;
    dnsmasqContext *dctx = NULL;

    /* if no IP addresses specified, nothing to do */
    if (!virNetworkDefGetIpByIndex(network->def, AF_UNSPEC, 0))
        return 0;

    /* if there's no running dnsmasq, just start it */
    if (network->dnsmasqPid <= 0 || (kill(network->dnsmasqPid, 0) < 0))
        return networkStartDhcpDaemon(network);

    VIR_INFO("Refreshing dnsmasq for network %s", network->def->bridge);
    if (!(dctx = dnsmasqContextNew(network->def->name,
                                   driver->dnsmasqStateDir))) {
        goto cleanup;
    }

    /* Look for first IPv4 address that has dhcp defined.
     * We only support dhcp-host config on one IPv4 subnetwork
     * and on one IPv6 subnetwork.
     */
    ipv4def = NULL;
    for (i = 0;
         (ipdef = virNetworkDefGetIpByIndex(network->def, AF_INET, i));
         i++) {
        if (!ipv4def && (ipdef->nranges || ipdef->nhosts))
            ipv4def = ipdef;
    }

    ipv6def = NULL;
    for (i = 0;
         (ipdef = virNetworkDefGetIpByIndex(network->def, AF_INET6, i));
         i++) {
        if (!ipv6def && (ipdef->nranges || ipdef->nhosts))
            ipv6def = ipdef;
    }

    if (ipv4def && (networkBuildDnsmasqDhcpHostsList(dctx, ipv4def) < 0))
        goto cleanup;

    if (ipv6def && (networkBuildDnsmasqDhcpHostsList(dctx, ipv6def) < 0))
        goto cleanup;

    if (networkBuildDnsmasqHostsList(dctx, &network->def->dns) < 0)
        goto cleanup;

    if ((ret = dnsmasqSave(dctx)) < 0)
        goto cleanup;

    ret = kill(network->dnsmasqPid, SIGHUP);
 cleanup:
    dnsmasqContextFree(dctx);
    return ret;
}

/* networkRestartDhcpDaemon:
 *
 * kill and restart dnsmasq, in order to update any config that is on
 * the dnsmasq commandline (and any placed in separate config files).
 *
 *  Returns 0 on success, -1 on failure.
 */
static int
networkRestartDhcpDaemon(virNetworkObjPtr network)
{
    /* if there is a running dnsmasq, kill it */
    if (network->dnsmasqPid > 0) {
        networkKillDaemon(network->dnsmasqPid, "dnsmasq",
                          network->def->name);
        network->dnsmasqPid = -1;
    }
    /* now start dnsmasq if it should be started */
    return networkStartDhcpDaemon(network);
}

static char radvd1[] = "  AdvOtherConfigFlag off;\n\n";
static char radvd2[] = "    AdvAutonomous off;\n";
static char radvd3[] = "    AdvOnLink on;\n"
                       "    AdvAutonomous on;\n"
                       "    AdvRouterAddr off;\n";

static int
networkRadvdConfContents(virNetworkObjPtr network, char **configstr)
{
    virBuffer configbuf = VIR_BUFFER_INITIALIZER;
    int ret = -1;
    size_t i;
    virNetworkIpDefPtr ipdef;
    bool v6present = false, dhcp6 = false;

    *configstr = NULL;

    /* Check if DHCPv6 is needed */
    for (i = 0;
         (ipdef = virNetworkDefGetIpByIndex(network->def, AF_INET6, i));
         i++) {
        v6present = true;
        if (ipdef->nranges || ipdef->nhosts) {
            dhcp6 = true;
            break;
        }
    }

    /* If there are no IPv6 addresses, then we are done */
    if (!v6present) {
        ret = 0;
        goto cleanup;
    }

    /* create radvd config file appropriate for this network;
     * IgnoreIfMissing allows radvd to start even when the bridge is down
     */
    virBufferAsprintf(&configbuf, "interface %s\n"
                      "{\n"
                      "  AdvSendAdvert on;\n"
                      "  IgnoreIfMissing on;\n"
                      "  AdvManagedFlag %s;\n"
                      "%s",
                      network->def->bridge,
                      dhcp6 ? "on" : "off",
                      dhcp6 ? "\n" : radvd1);

    /* add a section for each IPv6 address in the config */
    for (i = 0;
         (ipdef = virNetworkDefGetIpByIndex(network->def, AF_INET6, i));
         i++) {
        int prefix;
        char *netaddr;

        prefix = virNetworkIpDefPrefix(ipdef);
        if (prefix < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("bridge '%s' has an invalid prefix"),
                           network->def->bridge);
            goto cleanup;
        }
        if (!(netaddr = virSocketAddrFormat(&ipdef->address)))
            goto cleanup;
        virBufferAsprintf(&configbuf,
                          "  prefix %s/%d\n"
                          "  {\n%s  };\n",
                          netaddr, prefix,
                          dhcp6 ? radvd2 : radvd3);
        VIR_FREE(netaddr);
    }

    virBufferAddLit(&configbuf, "};\n");

    if (virBufferCheckError(&configbuf) < 0)
        goto cleanup;

    *configstr = virBufferContentAndReset(&configbuf);

    ret = 0;
 cleanup:
    virBufferFreeAndReset(&configbuf);
    return ret;
}

/* write file and return it's name (which must be freed by caller) */
static int
networkRadvdConfWrite(virNetworkObjPtr network, char **configFile)
{
    int ret = -1;
    char *configStr = NULL;
    char *myConfigFile = NULL;

    if (!configFile)
        configFile = &myConfigFile;

    *configFile = NULL;

    if (networkRadvdConfContents(network, &configStr) < 0)
        goto cleanup;

    if (!configStr) {
        ret = 0;
        goto cleanup;
    }

    /* construct the filename */
    if (!(*configFile = networkRadvdConfigFileName(network->def->name)))
        goto cleanup;
    /* write the file */
    if (virFileWriteStr(*configFile, configStr, 0600) < 0) {
        virReportSystemError(errno,
                             _("couldn't write radvd config file '%s'"),
                             *configFile);
        goto cleanup;
    }

    ret = 0;
 cleanup:
    VIR_FREE(configStr);
    VIR_FREE(myConfigFile);
    return ret;
}

static int
networkStartRadvd(virNetworkObjPtr network)
{
    char *pidfile = NULL;
    char *radvdpidbase = NULL;
    char *configfile = NULL;
    virCommandPtr cmd = NULL;
    int ret = -1;

    network->radvdPid = -1;

    /* Is dnsmasq handling RA? */
    if (DNSMASQ_RA_SUPPORT(driver->dnsmasqCaps)) {
        ret = 0;
        goto cleanup;
    }

    if (!virNetworkDefGetIpByIndex(network->def, AF_INET6, 0)) {
        /* no IPv6 addresses, so we don't need to run radvd */
        ret = 0;
        goto cleanup;
    }

    if (!virFileIsExecutable(RADVD)) {
        virReportSystemError(errno,
                             _("Cannot find %s - "
                               "Possibly the package isn't installed"),
                             RADVD);
        goto cleanup;
    }

    if (virFileMakePath(driver->pidDir) < 0) {
        virReportSystemError(errno,
                             _("cannot create directory %s"),
                             driver->pidDir);
        goto cleanup;
    }
    if (virFileMakePath(driver->radvdStateDir) < 0) {
        virReportSystemError(errno,
                             _("cannot create directory %s"),
                             driver->radvdStateDir);
        goto cleanup;
    }

    /* construct pidfile name */
    if (!(radvdpidbase = networkRadvdPidfileBasename(network->def->name)))
        goto cleanup;
    if (!(pidfile = virPidFileBuildPath(driver->pidDir, radvdpidbase)))
        goto cleanup;

    if (networkRadvdConfWrite(network, &configfile) < 0)
        goto cleanup;

    /* prevent radvd from daemonizing itself with "--debug 1", and use
     * a dummy pidfile name - virCommand will create the pidfile we
     * want to use (this is necessary because radvd's internal
     * daemonization and pidfile creation causes a race, and the
     * virPidFileRead() below will fail if we use them).
     * Unfortunately, it isn't possible to tell radvd to not create
     * its own pidfile, so we just let it do so, with a slightly
     * different name. Unused, but harmless.
     */
    cmd = virCommandNewArgList(RADVD, "--debug", "1",
                               "--config", configfile,
                               "--pidfile", NULL);
    virCommandAddArgFormat(cmd, "%s-bin", pidfile);

    virCommandSetPidFile(cmd, pidfile);
    virCommandDaemonize(cmd);

    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    if (virPidFileRead(driver->pidDir, radvdpidbase, &network->radvdPid) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virCommandFree(cmd);
    VIR_FREE(configfile);
    VIR_FREE(radvdpidbase);
    VIR_FREE(pidfile);
    return ret;
}

static int
networkRefreshRadvd(virNetworkObjPtr network)
{
    char *radvdpidbase;

    /* Is dnsmasq handling RA? */
    if (DNSMASQ_RA_SUPPORT(driver->dnsmasqCaps)) {
        if (network->radvdPid <= 0)
            return 0;
        /* radvd should not be running but in case it is */
        if ((networkKillDaemon(network->radvdPid, "radvd",
                               network->def->name) >= 0) &&
            ((radvdpidbase = networkRadvdPidfileBasename(network->def->name))
             != NULL)) {
            virPidFileDelete(driver->pidDir, radvdpidbase);
            VIR_FREE(radvdpidbase);
        }
        network->radvdPid = -1;
        return 0;
    }

    /* if there's no running radvd, just start it */
    if (network->radvdPid <= 0 || (kill(network->radvdPid, 0) < 0))
        return networkStartRadvd(network);

    if (!virNetworkDefGetIpByIndex(network->def, AF_INET6, 0)) {
        /* no IPv6 addresses, so we don't need to run radvd */
        return 0;
    }

    if (networkRadvdConfWrite(network, NULL) < 0)
        return -1;

    return kill(network->radvdPid, SIGHUP);
}

#if 0
/* currently unused, so it causes a build error unless we #if it out */
static int
networkRestartRadvd(virNetworkObjPtr network)
{
    char *radvdpidbase;

    /* if there is a running radvd, kill it */
    if (network->radvdPid > 0) {
        /* essentially ignore errors from the following two functions,
         * since there's really no better recovery to be done than to
         * just push ahead (and that may be exactly what's needed).
         */
        if ((networkKillDaemon(network->radvdPid, "radvd",
                               network->def->name) >= 0) &&
            ((radvdpidbase = networkRadvdPidfileBasename(network->def->name))
             != NULL)) {
            virPidFileDelete(driver->pidDir, radvdpidbase);
            VIR_FREE(radvdpidbase);
        }
        network->radvdPid = -1;
    }
    /* now start radvd if it should be started */
    return networkStartRadvd(network);
}
#endif /* #if 0 */

/* SIGHUP/restart any dnsmasq or radvd daemons.
 * This should be called when libvirtd is restarted.
 */
static void
networkRefreshDaemons(void)
{
    size_t i;

    VIR_INFO("Refreshing network daemons");

    for (i = 0; i < driver->networks.count; i++) {
        virNetworkObjPtr network = driver->networks.objs[i];

        virNetworkObjLock(network);
        if (virNetworkObjIsActive(network) &&
            ((network->def->forward.type == VIR_NETWORK_FORWARD_NONE) ||
             (network->def->forward.type == VIR_NETWORK_FORWARD_NAT) ||
             (network->def->forward.type == VIR_NETWORK_FORWARD_ROUTE))) {
            /* Only the three L3 network types that are configured by
             * libvirt will have a dnsmasq or radvd daemon associated
             * with them.  Here we send a SIGHUP to an existing
             * dnsmasq and/or radvd, or restart them if they've
             * disappeared.
             */
            networkRefreshDhcpDaemon(network);
            networkRefreshRadvd(network);
        }
        virNetworkObjUnlock(network);
    }
}

static void
networkReloadFirewallRules(void)
{
    size_t i;

    VIR_INFO("Reloading iptables rules");

    for (i = 0; i < driver->networks.count; i++) {
        virNetworkObjPtr network = driver->networks.objs[i];

        virNetworkObjLock(network);
        if (virNetworkObjIsActive(network) &&
            ((network->def->forward.type == VIR_NETWORK_FORWARD_NONE) ||
             (network->def->forward.type == VIR_NETWORK_FORWARD_NAT) ||
             (network->def->forward.type == VIR_NETWORK_FORWARD_ROUTE))) {
            /* Only the three L3 network types that are configured by libvirt
             * need to have iptables rules reloaded.
             */
            networkRemoveFirewallRules(network->def);
            if (networkAddFirewallRules(network->def) < 0) {
                /* failed to add but already logged */
            }
        }
        virNetworkObjUnlock(network);
    }
}

/* Enable IP Forwarding. Return 0 for success, -1 for failure. */
static int
networkEnableIpForwarding(bool enableIPv4, bool enableIPv6)
{
    int ret = 0;
#ifdef HAVE_SYSCTLBYNAME
    int enabled = 1;
    if (enableIPv4)
        ret = sysctlbyname("net.inet.ip.forwarding", NULL, 0,
                           &enabled, sizeof(enabled));
    if (enableIPv6 && ret == 0)
        ret = sysctlbyname("net.inet6.ip6.forwarding", NULL, 0,
                           &enabled, sizeof(enabled));
#else
    if (enableIPv4)
        ret = virFileWriteStr("/proc/sys/net/ipv4/ip_forward", "1\n", 0);
    if (enableIPv6 && ret == 0)
        ret = virFileWriteStr("/proc/sys/net/ipv6/conf/all/forwarding", "1\n", 0);
#endif
    return ret;
}

#define SYSCTL_PATH "/proc/sys"

static int
networkSetIPv6Sysctls(virNetworkObjPtr network)
{
    char *field = NULL;
    int ret = -1;
    bool enableIPv6 =  !!virNetworkDefGetIpByIndex(network->def, AF_INET6, 0);

    /* set disable_ipv6 if there are no ipv6 addresses defined for the
     * network. But also unset it if there *are* ipv6 addresses, as we
     * can't be sure of its default value.
     */
    if (virAsprintf(&field, SYSCTL_PATH "/net/ipv6/conf/%s/disable_ipv6",
                    network->def->bridge) < 0)
       goto cleanup;

    if (access(field, W_OK) < 0 && errno == ENOENT) {
        if (!enableIPv6)
            VIR_DEBUG("ipv6 appears to already be disabled on %s",
                      network->def->bridge);
        ret = 0;
        goto cleanup;
    }

    if (virFileWriteStr(field, enableIPv6 ? "0" : "1", 0) < 0) {
        virReportSystemError(errno,
                             _("cannot write to %s to enable/disable IPv6 "
                               "on bridge %s"), field, network->def->bridge);
        goto cleanup;
    }
    VIR_FREE(field);

    /* The rest of the ipv6 sysctl tunables should always be set the
     * same, whether or not we're using ipv6 on this bridge.
     */

    /* Prevent guests from hijacking the host network by sending out
     * their own router advertisements.
     */
    if (virAsprintf(&field, SYSCTL_PATH "/net/ipv6/conf/%s/accept_ra",
                    network->def->bridge) < 0)
        goto cleanup;

    if (virFileWriteStr(field, "0", 0) < 0) {
        virReportSystemError(errno,
                             _("cannot disable %s"), field);
        goto cleanup;
    }
    VIR_FREE(field);

    /* All interfaces used as a gateway (which is what this is, by
     * definition), must always have autoconf=0.
     */
    if (virAsprintf(&field, SYSCTL_PATH "/net/ipv6/conf/%s/autoconf",
                    network->def->bridge) < 0)
        goto cleanup;

    if (virFileWriteStr(field, "0", 0) < 0) {
        virReportSystemError(errno,
                             _("cannot disable %s"), field);
        goto cleanup;
    }

    ret = 0;
 cleanup:
    VIR_FREE(field);
    return ret;
}

/* add an IP address to a bridge */
static int
networkAddAddrToBridge(virNetworkObjPtr network,
                       virNetworkIpDefPtr ipdef)
{
    int prefix = virNetworkIpDefPrefix(ipdef);

    if (prefix < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("bridge '%s' has an invalid netmask or IP address"),
                       network->def->bridge);
        return -1;
    }

    if (virNetDevSetIPAddress(network->def->bridge,
                              &ipdef->address, prefix) < 0)
        return -1;

    return 0;
}


static int
networkStartHandleMACTableManagerMode(virNetworkObjPtr network,
                                      const char *macTapIfName)
{
    const char *brname = network->def->bridge;

    if (brname &&
        network->def->macTableManager
        == VIR_NETWORK_BRIDGE_MAC_TABLE_MANAGER_LIBVIRT) {
        if (virNetDevBridgeSetVlanFiltering(brname, true) < 0)
            return -1;
        if (macTapIfName) {
            if (virNetDevBridgePortSetLearning(brname, macTapIfName, false) < 0)
                return -1;
            if (virNetDevBridgePortSetUnicastFlood(brname, macTapIfName, false) < 0)
                return -1;
        }
    }
    return 0;
}


/* add an IP (static) route to a bridge */
static int
networkAddRouteToBridge(virNetworkObjPtr network,
                        virNetworkRouteDefPtr routedef)
{
    int prefix = virNetworkRouteDefGetPrefix(routedef);
    unsigned int metric = virNetworkRouteDefGetMetric(routedef);
    virSocketAddrPtr addr = virNetworkRouteDefGetAddress(routedef);
    virSocketAddrPtr gateway = virNetworkRouteDefGetGateway(routedef);

    if (prefix < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("network '%s' has an invalid netmask "
                         "or IP address in route definition"),
                       network->def->name);
        return -1;
    }

    if (virNetDevAddRoute(network->def->bridge, addr,
                          prefix, gateway, metric) < 0) {
        return -1;
    }
    return 0;
}

static int
networkStartNetworkVirtual(virNetworkObjPtr network)
{
    size_t i;
    bool v4present = false, v6present = false;
    virErrorPtr save_err = NULL;
    virNetworkIpDefPtr ipdef;
    virNetworkRouteDefPtr routedef;
    char *macTapIfName = NULL;
    int tapfd = -1;

    /* Check to see if any network IP collides with an existing route */
    if (networkCheckRouteCollision(network->def) < 0)
        return -1;

    /* Create and configure the bridge device */
    if (virNetDevBridgeCreate(network->def->bridge) < 0)
        return -1;

    if (network->def->mac_specified) {
        /* To set a mac for the bridge, we need to define a dummy tap
         * device, set its mac, then attach it to the bridge. As long
         * as its mac address is lower than any other interface that
         * gets attached, the bridge will always maintain this mac
         * address.
         */
        macTapIfName = networkBridgeDummyNicName(network->def->bridge);
        if (!macTapIfName)
            goto err0;
        /* Keep tun fd open and interface up to allow for IPv6 DAD to happen */
        if (virNetDevTapCreateInBridgePort(network->def->bridge,
                                           &macTapIfName, &network->def->mac,
                                           NULL, NULL, &tapfd, 1, NULL, NULL,
                                           VIR_NETDEV_TAP_CREATE_USE_MAC_FOR_BRIDGE |
                                           VIR_NETDEV_TAP_CREATE_IFUP |
                                           VIR_NETDEV_TAP_CREATE_PERSIST) < 0) {
            VIR_FREE(macTapIfName);
            goto err0;
        }
    }

    /* Set bridge options */

    /* delay is configured in seconds, but virNetDevBridgeSetSTPDelay
     * expects milliseconds
     */
    if (virNetDevBridgeSetSTPDelay(network->def->bridge,
                                   network->def->delay * 1000) < 0)
        goto err1;

    if (virNetDevBridgeSetSTP(network->def->bridge,
                              network->def->stp ? true : false) < 0)
        goto err1;

    /* Disable IPv6 on the bridge if there are no IPv6 addresses
     * defined, and set other IPv6 sysctl tunables appropriately.
     */
    if (networkSetIPv6Sysctls(network) < 0)
        goto err1;

    /* Add "once per network" rules */
    if (networkAddFirewallRules(network->def) < 0)
        goto err1;

    for (i = 0;
         (ipdef = virNetworkDefGetIpByIndex(network->def, AF_UNSPEC, i));
         i++) {
        if (VIR_SOCKET_ADDR_IS_FAMILY(&ipdef->address, AF_INET))
            v4present = true;
        if (VIR_SOCKET_ADDR_IS_FAMILY(&ipdef->address, AF_INET6))
            v6present = true;

        /* Add the IP address/netmask to the bridge */
        if (networkAddAddrToBridge(network, ipdef) < 0)
            goto err2;
    }

    if (networkStartHandleMACTableManagerMode(network, macTapIfName) < 0)
        goto err2;

    /* Bring up the bridge interface */
    if (virNetDevSetOnline(network->def->bridge, 1) < 0)
        goto err2;

    for (i = 0; i < network->def->nroutes; i++) {
        virSocketAddrPtr gateway = NULL;

        routedef = network->def->routes[i];
        gateway = virNetworkRouteDefGetGateway(routedef);

        /* Add the IP route to the bridge */
        /* ignore errors, error msg will be generated */
        /* but libvirt will not know and net-destroy will work. */
        if (VIR_SOCKET_ADDR_VALID(gateway)) {
            if (networkAddRouteToBridge(network, routedef) < 0) {
                /* an error occurred adding the static route */
                continue; /* for now, do nothing */
            }
        }
    }

    /* If forward.type != NONE, turn on global IP forwarding */
    if (network->def->forward.type != VIR_NETWORK_FORWARD_NONE &&
        networkEnableIpForwarding(v4present, v6present) < 0) {
        virReportSystemError(errno, "%s",
                             _("failed to enable IP forwarding"));
        goto err3;
    }


    /* start dnsmasq if there are any IP addresses (v4 or v6) */
    if ((v4present || v6present) &&
        networkStartDhcpDaemon(network) < 0)
        goto err3;

    /* start radvd if there are any ipv6 addresses */
    if (v6present && networkStartRadvd(network) < 0)
        goto err4;

    /* DAD has happened (dnsmasq waits for it), dnsmasq is now bound to the
     * bridge's IPv6 address, so we can now set the dummy tun down.
     */
    if (tapfd >= 0) {
        if (virNetDevSetOnline(macTapIfName, false) < 0)
            goto err4;
        VIR_FORCE_CLOSE(tapfd);
    }

    if (virNetDevBandwidthSet(network->def->bridge,
                              network->def->bandwidth, true) < 0)
        goto err5;

    VIR_FREE(macTapIfName);

    return 0;

 err5:
    virNetDevBandwidthClear(network->def->bridge);

 err4:
    if (!save_err)
        save_err = virSaveLastError();

    if (network->dnsmasqPid > 0) {
        kill(network->dnsmasqPid, SIGTERM);
        network->dnsmasqPid = -1;
    }

 err3:
    if (!save_err)
        save_err = virSaveLastError();
    ignore_value(virNetDevSetOnline(network->def->bridge, 0));

 err2:
    if (!save_err)
        save_err = virSaveLastError();
    networkRemoveFirewallRules(network->def);

 err1:
    if (!save_err)
        save_err = virSaveLastError();

    if (macTapIfName) {
        VIR_FORCE_CLOSE(tapfd);
        ignore_value(virNetDevTapDelete(macTapIfName, NULL));
        VIR_FREE(macTapIfName);
    }

 err0:
    if (!save_err)
        save_err = virSaveLastError();
    ignore_value(virNetDevBridgeDelete(network->def->bridge));

    if (save_err) {
        virSetError(save_err);
        virFreeError(save_err);
    }
    /* coverity[leaked_handle] - 'tapfd' is not leaked */
    return -1;
}

static int networkShutdownNetworkVirtual(virNetworkObjPtr network)
{
    virNetDevBandwidthClear(network->def->bridge);

    if (network->radvdPid > 0) {
        char *radvdpidbase;

        kill(network->radvdPid, SIGTERM);
        /* attempt to delete the pidfile we created */
        if ((radvdpidbase = networkRadvdPidfileBasename(network->def->name))) {
            virPidFileDelete(driver->pidDir, radvdpidbase);
            VIR_FREE(radvdpidbase);
        }
    }

    if (network->dnsmasqPid > 0)
        kill(network->dnsmasqPid, SIGTERM);

    if (network->def->mac_specified) {
        char *macTapIfName = networkBridgeDummyNicName(network->def->bridge);
        if (macTapIfName) {
            ignore_value(virNetDevTapDelete(macTapIfName, NULL));
            VIR_FREE(macTapIfName);
        }
    }

    ignore_value(virNetDevSetOnline(network->def->bridge, 0));

    networkRemoveFirewallRules(network->def);

    ignore_value(virNetDevBridgeDelete(network->def->bridge));

    /* See if its still alive and really really kill it */
    if (network->dnsmasqPid > 0 &&
        (kill(network->dnsmasqPid, 0) == 0))
        kill(network->dnsmasqPid, SIGKILL);
    network->dnsmasqPid = -1;

    if (network->radvdPid > 0 &&
        (kill(network->radvdPid, 0) == 0))
        kill(network->radvdPid, SIGKILL);
    network->radvdPid = -1;

    return 0;
}


static int
networkStartNetworkBridge(virNetworkObjPtr network)
{
    /* put anything here that needs to be done each time a network of
     * type BRIDGE, is started. On failure, undo anything you've done,
     * and return -1. On success return 0.
     */
    return networkStartHandleMACTableManagerMode(network, NULL);
}

static int
networkShutdownNetworkBridge(virNetworkObjPtr network ATTRIBUTE_UNUSED)
{
    /* put anything here that needs to be done each time a network of
     * type BRIDGE is shutdown. On failure, undo anything you've done,
     * and return -1. On success return 0.
     */
    return 0;
}


/* networkCreateInterfacePool:
 * @netdef: the original NetDef from the network
 *
 * Creates an implicit interface pool of VF's when a PF dev is given
 */
static int
networkCreateInterfacePool(virNetworkDefPtr netdef)
{
    size_t numVirtFns = 0;
    char **vfNames = NULL;
    virPCIDeviceAddressPtr *virtFns;

    int ret = -1;
    size_t i;

    if (netdef->forward.npfs == 0 || netdef->forward.nifs > 0)
       return 0;

    if ((virNetDevGetVirtualFunctions(netdef->forward.pfs->dev,
                                      &vfNames, &virtFns, &numVirtFns)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not get Virtual functions on %s"),
                       netdef->forward.pfs->dev);
        goto cleanup;
    }

    if (VIR_ALLOC_N(netdef->forward.ifs, numVirtFns) < 0)
        goto cleanup;

    for (i = 0; i < numVirtFns; i++) {
        virPCIDeviceAddressPtr thisVirtFn = virtFns[i];
        const char *thisName = vfNames[i];
        virNetworkForwardIfDefPtr thisIf
            = &netdef->forward.ifs[netdef->forward.nifs];

        switch (netdef->forward.type) {
        case VIR_NETWORK_FORWARD_BRIDGE:
        case VIR_NETWORK_FORWARD_PRIVATE:
        case VIR_NETWORK_FORWARD_VEPA:
        case VIR_NETWORK_FORWARD_PASSTHROUGH:
            if (thisName) {
                if (VIR_STRDUP(thisIf->device.dev, thisName) < 0)
                    goto cleanup;
                thisIf->type = VIR_NETWORK_FORWARD_HOSTDEV_DEVICE_NETDEV;
                netdef->forward.nifs++;
            } else {
                VIR_WARN("VF %zu of SRIOV PF %s couldn't be added to the "
                         "interface pool because it isn't bound "
                         "to a network driver - possibly in use elsewhere",
                         i, netdef->forward.pfs->dev);
            }
            break;

        case VIR_NETWORK_FORWARD_HOSTDEV:
            /* VF's are always PCI devices */
            thisIf->type = VIR_NETWORK_FORWARD_HOSTDEV_DEVICE_PCI;
            thisIf->device.pci.domain = thisVirtFn->domain;
            thisIf->device.pci.bus = thisVirtFn->bus;
            thisIf->device.pci.slot = thisVirtFn->slot;
            thisIf->device.pci.function = thisVirtFn->function;
            netdef->forward.nifs++;
            break;

        case VIR_NETWORK_FORWARD_NONE:
        case VIR_NETWORK_FORWARD_NAT:
        case VIR_NETWORK_FORWARD_ROUTE:
        case VIR_NETWORK_FORWARD_LAST:
            /* by definition these will never be encountered here */
            break;
        }
    }

    if (netdef->forward.nifs == 0) {
        /* If we don't get at least one interface in the pool, declare
         * failure
         */
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("No usable Vf's present on SRIOV PF %s"),
                       netdef->forward.pfs->dev);
        goto cleanup;
    }

    ret = 0;
 cleanup:
    if (ret < 0) {
        /* free all the entries made before error */
        for (i = 0; i < netdef->forward.nifs; i++) {
            if (netdef->forward.ifs[i].type
                == VIR_NETWORK_FORWARD_HOSTDEV_DEVICE_NETDEV)
                VIR_FREE(netdef->forward.ifs[i].device.dev);
        }
        netdef->forward.nifs = 0;
    }
    if (netdef->forward.nifs == 0)
        VIR_FREE(netdef->forward.ifs);

    for (i = 0; i < numVirtFns; i++) {
        VIR_FREE(vfNames[i]);
        VIR_FREE(virtFns[i]);
    }
    VIR_FREE(vfNames);
    VIR_FREE(virtFns);
    return ret;
}


static int
networkStartNetworkExternal(virNetworkObjPtr network)
{
    /* put anything here that needs to be done each time a network of
     * type BRIDGE, PRIVATE, VEPA, HOSTDEV or PASSTHROUGH is started. On
     * failure, undo anything you've done, and return -1. On success
     * return 0.
     */
    return networkCreateInterfacePool(network->def);
}

static int networkShutdownNetworkExternal(virNetworkObjPtr network ATTRIBUTE_UNUSED)
{
    /* put anything here that needs to be done each time a network of
     * type BRIDGE, PRIVATE, VEPA, HOSTDEV or PASSTHROUGH is shutdown. On
     * failure, undo anything you've done, and return -1. On success
     * return 0.
     */
    return 0;
}

static int
networkStartNetwork(virNetworkObjPtr network)
{
    int ret = -1;

    VIR_DEBUG("driver=%p, network=%p", driver, network);

    if (virNetworkObjIsActive(network)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("network is already active"));
        return ret;
    }

    VIR_DEBUG("Beginning network startup process");

    VIR_DEBUG("Setting current network def as transient");
    if (virNetworkObjSetDefTransient(network, true) < 0)
        goto cleanup;

    /* Run an early hook to set-up missing devices.
     * If the script raised an error abort the launch. */
    if (networkRunHook(network, NULL, NULL,
                       VIR_HOOK_NETWORK_OP_START,
                       VIR_HOOK_SUBOP_BEGIN) < 0)
        goto cleanup;

    switch (network->def->forward.type) {

    case VIR_NETWORK_FORWARD_NONE:
    case VIR_NETWORK_FORWARD_NAT:
    case VIR_NETWORK_FORWARD_ROUTE:
        if (networkStartNetworkVirtual(network) < 0)
            goto cleanup;
        break;

    case VIR_NETWORK_FORWARD_BRIDGE:
       if (networkStartNetworkBridge(network) < 0)
          goto cleanup;
       break;

    case VIR_NETWORK_FORWARD_PRIVATE:
    case VIR_NETWORK_FORWARD_VEPA:
    case VIR_NETWORK_FORWARD_PASSTHROUGH:
    case VIR_NETWORK_FORWARD_HOSTDEV:
        if (networkStartNetworkExternal(network) < 0)
            goto cleanup;
        break;
    }

    /* finally we can call the 'started' hook script if any */
    if (networkRunHook(network, NULL, NULL,
                       VIR_HOOK_NETWORK_OP_STARTED,
                       VIR_HOOK_SUBOP_BEGIN) < 0)
        goto cleanup;

    /* Persist the live configuration now that anything autogenerated
     * is setup.
     */
    VIR_DEBUG("Writing network status to disk");
    if (virNetworkSaveStatus(driver->stateDir, network) < 0)
        goto cleanup;

    network->active = 1;
    VIR_INFO("Network '%s' started up", network->def->name);
    ret = 0;

 cleanup:
    if (ret < 0) {
        virNetworkObjUnsetDefTransient(network);
        virErrorPtr save_err = virSaveLastError();
        int save_errno = errno;
        networkShutdownNetwork(network);
        virSetError(save_err);
        virFreeError(save_err);
        errno = save_errno;
    }
    return ret;
}

static int networkShutdownNetwork(virNetworkObjPtr network)
{
    int ret = 0;
    char *stateFile;

    VIR_INFO("Shutting down network '%s'", network->def->name);

    if (!virNetworkObjIsActive(network))
        return 0;

    stateFile = virNetworkConfigFile(driver->stateDir,
                                     network->def->name);
    if (!stateFile)
        return -1;

    unlink(stateFile);
    VIR_FREE(stateFile);

    switch (network->def->forward.type) {

    case VIR_NETWORK_FORWARD_NONE:
    case VIR_NETWORK_FORWARD_NAT:
    case VIR_NETWORK_FORWARD_ROUTE:
        ret = networkShutdownNetworkVirtual(network);
        break;

    case VIR_NETWORK_FORWARD_BRIDGE:
        ret = networkShutdownNetworkBridge(network);
        break;

    case VIR_NETWORK_FORWARD_PRIVATE:
    case VIR_NETWORK_FORWARD_VEPA:
    case VIR_NETWORK_FORWARD_PASSTHROUGH:
    case VIR_NETWORK_FORWARD_HOSTDEV:
        ret = networkShutdownNetworkExternal(network);
        break;
    }

    /* now that we know it's stopped call the hook if present */
    networkRunHook(network, NULL, NULL, VIR_HOOK_NETWORK_OP_STOPPED,
                   VIR_HOOK_SUBOP_END);

    network->active = 0;
    virNetworkObjUnsetDefTransient(network);
    return ret;
}


static virNetworkPtr networkLookupByUUID(virConnectPtr conn,
                                         const unsigned char *uuid)
{
    virNetworkObjPtr network;
    virNetworkPtr ret = NULL;

    networkDriverLock();
    network = virNetworkFindByUUID(&driver->networks, uuid);
    networkDriverUnlock();
    if (!network) {
        virReportError(VIR_ERR_NO_NETWORK,
                       "%s", _("no network with matching uuid"));
        goto cleanup;
    }

    if (virNetworkLookupByUUIDEnsureACL(conn, network->def) < 0)
        goto cleanup;

    ret = virGetNetwork(conn, network->def->name, network->def->uuid);

 cleanup:
    if (network)
        virNetworkObjUnlock(network);
    return ret;
}

static virNetworkPtr networkLookupByName(virConnectPtr conn,
                                         const char *name)
{
    virNetworkObjPtr network;
    virNetworkPtr ret = NULL;

    networkDriverLock();
    network = virNetworkFindByName(&driver->networks, name);
    networkDriverUnlock();
    if (!network) {
        virReportError(VIR_ERR_NO_NETWORK,
                       _("no network with matching name '%s'"), name);
        goto cleanup;
    }

    if (virNetworkLookupByNameEnsureACL(conn, network->def) < 0)
        goto cleanup;

    ret = virGetNetwork(conn, network->def->name, network->def->uuid);

 cleanup:
    if (network)
        virNetworkObjUnlock(network);
    return ret;
}

static virDrvOpenStatus networkOpen(virConnectPtr conn ATTRIBUTE_UNUSED,
                                    virConnectAuthPtr auth ATTRIBUTE_UNUSED,
                                    unsigned int flags)
{
    virCheckFlags(VIR_CONNECT_RO, VIR_DRV_OPEN_ERROR);

    if (!driver)
        return VIR_DRV_OPEN_DECLINED;

    return VIR_DRV_OPEN_SUCCESS;
}

static int networkClose(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return 0;
}

static int networkConnectNumOfNetworks(virConnectPtr conn)
{
    int nactive = 0;
    size_t i;

    if (virConnectNumOfNetworksEnsureACL(conn) < 0)
        return -1;

    networkDriverLock();
    for (i = 0; i < driver->networks.count; i++) {
        virNetworkObjPtr obj = driver->networks.objs[i];
        virNetworkObjLock(obj);
        if (virConnectNumOfNetworksCheckACL(conn, obj->def) &&
            virNetworkObjIsActive(obj))
            nactive++;
        virNetworkObjUnlock(obj);
    }
    networkDriverUnlock();

    return nactive;
}

static int networkConnectListNetworks(virConnectPtr conn, char **const names, int nnames) {
    int got = 0;
    size_t i;

    if (virConnectListNetworksEnsureACL(conn) < 0)
        return -1;

    networkDriverLock();
    for (i = 0; i < driver->networks.count && got < nnames; i++) {
        virNetworkObjPtr obj = driver->networks.objs[i];
        virNetworkObjLock(obj);
        if (virConnectListNetworksCheckACL(conn, obj->def) &&
            virNetworkObjIsActive(obj)) {
            if (VIR_STRDUP(names[got], obj->def->name) < 0) {
                virNetworkObjUnlock(obj);
                goto cleanup;
            }
            got++;
        }
        virNetworkObjUnlock(obj);
    }
    networkDriverUnlock();

    return got;

 cleanup:
    networkDriverUnlock();
    for (i = 0; i < got; i++)
        VIR_FREE(names[i]);
    return -1;
}

static int networkConnectNumOfDefinedNetworks(virConnectPtr conn)
{
    int ninactive = 0;
    size_t i;

    if (virConnectNumOfDefinedNetworksEnsureACL(conn) < 0)
        return -1;

    networkDriverLock();
    for (i = 0; i < driver->networks.count; i++) {
        virNetworkObjPtr obj = driver->networks.objs[i];
        virNetworkObjLock(obj);
        if (virConnectNumOfDefinedNetworksCheckACL(conn, obj->def) &&
            !virNetworkObjIsActive(obj))
            ninactive++;
        virNetworkObjUnlock(obj);
    }
    networkDriverUnlock();

    return ninactive;
}

static int networkConnectListDefinedNetworks(virConnectPtr conn, char **const names, int nnames) {
    int got = 0;
    size_t i;

    if (virConnectListDefinedNetworksEnsureACL(conn) < 0)
        return -1;

    networkDriverLock();
    for (i = 0; i < driver->networks.count && got < nnames; i++) {
        virNetworkObjPtr obj = driver->networks.objs[i];
        virNetworkObjLock(obj);
        if (virConnectListDefinedNetworksCheckACL(conn, obj->def) &&
            !virNetworkObjIsActive(obj)) {
            if (VIR_STRDUP(names[got], obj->def->name) < 0) {
                virNetworkObjUnlock(obj);
                goto cleanup;
            }
            got++;
        }
        virNetworkObjUnlock(obj);
    }
    networkDriverUnlock();
    return got;

 cleanup:
    networkDriverUnlock();
    for (i = 0; i < got; i++)
        VIR_FREE(names[i]);
    return -1;
}

static int
networkConnectListAllNetworks(virConnectPtr conn,
                              virNetworkPtr **nets,
                              unsigned int flags)
{
    int ret = -1;

    virCheckFlags(VIR_CONNECT_LIST_NETWORKS_FILTERS_ALL, -1);

    if (virConnectListAllNetworksEnsureACL(conn) < 0)
        goto cleanup;

    networkDriverLock();
    ret = virNetworkObjListExport(conn, driver->networks, nets,
                                  virConnectListAllNetworksCheckACL,
                                  flags);
    networkDriverUnlock();

 cleanup:
    return ret;
}

static int
networkConnectNetworkEventRegisterAny(virConnectPtr conn,
                                      virNetworkPtr net,
                                      int eventID,
                                      virConnectNetworkEventGenericCallback callback,
                                      void *opaque,
                                      virFreeCallback freecb)
{
    int ret = -1;

    if (virConnectNetworkEventRegisterAnyEnsureACL(conn) < 0)
        goto cleanup;

    if (virNetworkEventStateRegisterID(conn, driver->networkEventState,
                                       net, eventID, callback,
                                       opaque, freecb, &ret) < 0)
        ret = -1;

 cleanup:
    return ret;
}

static int
networkConnectNetworkEventDeregisterAny(virConnectPtr conn,
                                        int callbackID)
{
    int ret = -1;

    if (virConnectNetworkEventDeregisterAnyEnsureACL(conn) < 0)
        goto cleanup;

    if (virObjectEventStateDeregisterID(conn,
                                        driver->networkEventState,
                                        callbackID) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    return ret;
}

static int networkIsActive(virNetworkPtr net)
{
    virNetworkObjPtr obj;
    int ret = -1;

    if (!(obj = networkObjFromNetwork(net)))
        return ret;

    if (virNetworkIsActiveEnsureACL(net->conn, obj->def) < 0)
        goto cleanup;

    ret = virNetworkObjIsActive(obj);

 cleanup:
    if (obj)
        virNetworkObjUnlock(obj);
    return ret;
}

static int networkIsPersistent(virNetworkPtr net)
{
    virNetworkObjPtr obj;
    int ret = -1;

    if (!(obj = networkObjFromNetwork(net)))
        return ret;

    if (virNetworkIsPersistentEnsureACL(net->conn, obj->def) < 0)
        goto cleanup;

    ret = obj->persistent;

 cleanup:
    if (obj)
        virNetworkObjUnlock(obj);
    return ret;
}


static int
networkValidate(virNetworkDefPtr def,
                bool check_active)
{
    size_t i;
    bool vlanUsed, vlanAllowed, badVlanUse = false;
    virPortGroupDefPtr defaultPortGroup = NULL;
    virNetworkIpDefPtr ipdef;
    bool ipv4def = false, ipv6def = false;
    bool bandwidthAllowed = true;

    /* check for duplicate networks */
    if (virNetworkObjIsDuplicate(&driver->networks, def, check_active) < 0)
        return -1;

    /* Only the three L3 network types that are configured by libvirt
     * need to have a bridge device name / mac address provided
     */
    if (def->forward.type == VIR_NETWORK_FORWARD_NONE ||
        def->forward.type == VIR_NETWORK_FORWARD_NAT ||
        def->forward.type == VIR_NETWORK_FORWARD_ROUTE) {

        if (virNetworkSetBridgeName(&driver->networks, def, 1))
            return -1;

        virNetworkSetBridgeMacAddr(def);
    } else {
        /* They are also the only types that currently support setting
         * a MAC or IP address for the host-side device (bridge), DNS
         * configuration, or network-wide bandwidth limits.
         */
        if (def->mac_specified) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Unsupported <mac> element in network %s "
                             "with forward mode='%s'"),
                           def->name,
                           virNetworkForwardTypeToString(def->forward.type));
            return -1;
        }
        if (virNetworkDefGetIpByIndex(def, AF_UNSPEC, 0)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Unsupported <ip> element in network %s "
                             "with forward mode='%s'"),
                           def->name,
                           virNetworkForwardTypeToString(def->forward.type));
            return -1;
        }
        if (def->dns.ntxts || def->dns.nhosts || def->dns.nsrvs) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Unsupported <dns> element in network %s "
                             "with forward mode='%s'"),
                           def->name,
                           virNetworkForwardTypeToString(def->forward.type));
            return -1;
        }
        if (def->domain) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Unsupported <domain> element in network %s "
                             "with forward mode='%s'"),
                           def->name,
                           virNetworkForwardTypeToString(def->forward.type));
            return -1;
        }
        if (def->bandwidth) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Unsupported network-wide <bandwidth> element "
                             "in network %s with forward mode='%s'"),
                           def->name,
                           virNetworkForwardTypeToString(def->forward.type));
            return -1;
        }
        bandwidthAllowed = false;
    }

    /* We only support dhcp on one IPv4 address and
     * on one IPv6 address per defined network
     */
    for (i = 0;
         (ipdef = virNetworkDefGetIpByIndex(def, AF_UNSPEC, i));
         i++) {
        if (VIR_SOCKET_ADDR_IS_FAMILY(&ipdef->address, AF_INET)) {
            if (ipdef->nranges || ipdef->nhosts) {
                if (ipv4def) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("Multiple IPv4 dhcp sections found -- "
                                 "dhcp is supported only for a "
                                 "single IPv4 address on each network"));
                    return -1;
                } else {
                    ipv4def = true;
                }
            }
        }
        if (VIR_SOCKET_ADDR_IS_FAMILY(&ipdef->address, AF_INET6)) {
            if (ipdef->nranges || ipdef->nhosts) {
                if (ipv6def) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("Multiple IPv6 dhcp sections found -- "
                                 "dhcp is supported only for a "
                                 "single IPv6 address on each network"));
                    return -1;
                } else {
                    ipv6def = true;
                }
            }
        }
    }

    /* The only type of networks that currently support transparent
     * vlan configuration are those using hostdev sr-iov devices from
     * a pool, and those using an Open vSwitch bridge.
     */

    vlanAllowed = ((def->forward.type == VIR_NETWORK_FORWARD_BRIDGE &&
                    def->virtPortProfile &&
                    def->virtPortProfile->virtPortType
                    == VIR_NETDEV_VPORT_PROFILE_OPENVSWITCH) ||
                   def->forward.type == VIR_NETWORK_FORWARD_HOSTDEV);

    vlanUsed = def->vlan.nTags > 0;
    for (i = 0; i < def->nPortGroups; i++) {
        if (vlanUsed || def->portGroups[i].vlan.nTags > 0) {
            /* anyone using this portgroup will get a vlan tag. Verify
             * that they will also be using an openvswitch connection,
             * as that is the only type of network that currently
             * supports a vlan tag.
             */
            if (def->portGroups[i].virtPortProfile) {
                if (def->forward.type != VIR_NETWORK_FORWARD_BRIDGE ||
                    def->portGroups[i].virtPortProfile->virtPortType
                    != VIR_NETDEV_VPORT_PROFILE_OPENVSWITCH) {
                    badVlanUse = true;
                }
            } else if (!vlanAllowed) {
                /* virtualport taken from base network definition */
                badVlanUse = true;
            }
        }
        if (def->portGroups[i].isDefault) {
            if (defaultPortGroup) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("network '%s' has multiple default "
                                 "<portgroup> elements (%s and %s), "
                                 "but only one default is allowed"),
                               def->name, defaultPortGroup->name,
                               def->portGroups[i].name);
                return -1;
            }
            defaultPortGroup = &def->portGroups[i];
        }

        if (def->portGroups[i].bandwidth && !bandwidthAllowed) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Unsupported <bandwidth> element in network '%s' "
                             "in portgroup '%s' with forward mode='%s'"),
                           def->name, def->portGroups[i].name,
                           virNetworkForwardTypeToString(def->forward.type));
            return -1;
        }
    }
    if (badVlanUse ||
        (vlanUsed && !vlanAllowed && !defaultPortGroup)) {
        /* NB: if defaultPortGroup is set, we don't directly look at
         * vlanUsed && !vlanAllowed, because the network will never be
         * used without having a portgroup added in, so all necessary
         * checks were done in the loop above.
         */
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("<vlan> element specified for network %s, "
                         "whose type doesn't support vlan configuration"),
                       def->name);
        return -1;
    }
    return 0;
}

static virNetworkPtr networkCreateXML(virConnectPtr conn, const char *xml)
{
    virNetworkDefPtr def;
    virNetworkObjPtr network = NULL;
    virNetworkPtr ret = NULL;
    virObjectEventPtr event = NULL;

    networkDriverLock();

    if (!(def = virNetworkDefParseString(xml)))
        goto cleanup;

    if (virNetworkCreateXMLEnsureACL(conn, def) < 0)
        goto cleanup;

    if (networkValidate(def, true) < 0)
        goto cleanup;

    /* NB: even though this transient network hasn't yet been started,
     * we assign the def with live = true in anticipation that it will
     * be started momentarily.
     */
    if (!(network = virNetworkAssignDef(&driver->networks, def, true)))
        goto cleanup;
    def = NULL;

    if (networkStartNetwork(network) < 0) {
        virNetworkRemoveInactive(&driver->networks,
                                 network);
        network = NULL;
        goto cleanup;
    }

    event = virNetworkEventLifecycleNew(network->def->name,
                                        network->def->uuid,
                                        VIR_NETWORK_EVENT_STARTED,
                                        0);

    VIR_INFO("Creating network '%s'", network->def->name);
    ret = virGetNetwork(conn, network->def->name, network->def->uuid);

 cleanup:
    virNetworkDefFree(def);
    if (event)
        virObjectEventStateQueue(driver->networkEventState, event);
    if (network)
        virNetworkObjUnlock(network);
    networkDriverUnlock();
    return ret;
}

static virNetworkPtr networkDefineXML(virConnectPtr conn, const char *xml)
{
    virNetworkDefPtr def = NULL;
    bool freeDef = true;
    virNetworkObjPtr network = NULL;
    virNetworkPtr ret = NULL;
    virObjectEventPtr event = NULL;

    networkDriverLock();

    if (!(def = virNetworkDefParseString(xml)))
        goto cleanup;

    if (virNetworkDefineXMLEnsureACL(conn, def) < 0)
        goto cleanup;

    if (networkValidate(def, false) < 0)
        goto cleanup;

    if (!(network = virNetworkAssignDef(&driver->networks, def, false)))
        goto cleanup;

    /* def was assigned to network object */
    freeDef = false;

    if (virNetworkSaveConfig(driver->networkConfigDir, def) < 0) {
        if (!virNetworkObjIsActive(network)) {
            virNetworkRemoveInactive(&driver->networks, network);
            network = NULL;
            goto cleanup;
        }
        /* if network was active already, just undo new persistent
         * definition by making it transient.
         * XXX - this isn't necessarily the correct thing to do.
         */
        virNetworkObjAssignDef(network, NULL, false);
        goto cleanup;
    }

    event = virNetworkEventLifecycleNew(def->name, def->uuid,
                                        VIR_NETWORK_EVENT_DEFINED,
                                        0);

    VIR_INFO("Defining network '%s'", def->name);
    ret = virGetNetwork(conn, def->name, def->uuid);

 cleanup:
    if (event)
        virObjectEventStateQueue(driver->networkEventState, event);
    if (freeDef)
        virNetworkDefFree(def);
    if (network)
        virNetworkObjUnlock(network);
    networkDriverUnlock();
    return ret;
}

static int
networkUndefine(virNetworkPtr net)
{
    virNetworkObjPtr network;
    int ret = -1;
    bool active = false;
    virObjectEventPtr event = NULL;

    networkDriverLock();

    network = virNetworkFindByUUID(&driver->networks, net->uuid);
    if (!network) {
        virReportError(VIR_ERR_NO_NETWORK,
                       "%s", _("no network with matching uuid"));
        goto cleanup;
    }

    if (virNetworkUndefineEnsureACL(net->conn, network->def) < 0)
        goto cleanup;

    if (virNetworkObjIsActive(network))
        active = true;

    /* remove autostart link */
    if (virNetworkDeleteConfig(driver->networkConfigDir,
                               driver->networkAutostartDir,
                               network) < 0)
        goto cleanup;
    network->autostart = 0;

    event = virNetworkEventLifecycleNew(network->def->name,
                                        network->def->uuid,
                                        VIR_NETWORK_EVENT_UNDEFINED,
                                        0);

    VIR_INFO("Undefining network '%s'", network->def->name);
    if (!active) {
        if (networkRemoveInactive(network) < 0) {
            network = NULL;
            goto cleanup;
        }
        network = NULL;
    } else {

        /* if the network still exists, it was active, and we need to make
         * it transient (by deleting the persistent def)
         */
        virNetworkObjAssignDef(network, NULL, false);
    }

    ret = 0;

 cleanup:
    if (event)
        virObjectEventStateQueue(driver->networkEventState, event);
    if (network)
        virNetworkObjUnlock(network);
    networkDriverUnlock();
    return ret;
}

static int
networkUpdate(virNetworkPtr net,
              unsigned int command,
              unsigned int section,
              int parentIndex,
              const char *xml,
              unsigned int flags)
{
    virNetworkObjPtr network = NULL;
    int isActive, ret = -1;
    size_t i;
    virNetworkIpDefPtr ipdef;
    bool oldDhcpActive = false;
    bool needFirewallRefresh = false;


    virCheckFlags(VIR_NETWORK_UPDATE_AFFECT_LIVE |
                  VIR_NETWORK_UPDATE_AFFECT_CONFIG,
                  -1);

    networkDriverLock();

    network = virNetworkFindByUUID(&driver->networks, net->uuid);
    if (!network) {
        virReportError(VIR_ERR_NO_NETWORK,
                       "%s", _("no network with matching uuid"));
        goto cleanup;
    }

    if (virNetworkUpdateEnsureACL(net->conn, network->def, flags) < 0)
        goto cleanup;

    /* see if we are listening for dhcp pre-modification */
    for (i = 0;
         (ipdef = virNetworkDefGetIpByIndex(network->def, AF_INET, i));
         i++) {
        if (ipdef->nranges || ipdef->nhosts) {
            oldDhcpActive = true;
            break;
        }
    }

    /* VIR_NETWORK_UPDATE_AFFECT_CURRENT means "change LIVE if network
     * is active, else change CONFIG
     */
    isActive = virNetworkObjIsActive(network);
    if ((flags & (VIR_NETWORK_UPDATE_AFFECT_LIVE |
                  VIR_NETWORK_UPDATE_AFFECT_CONFIG)) ==
        VIR_NETWORK_UPDATE_AFFECT_CURRENT) {
        if (isActive)
            flags |= VIR_NETWORK_UPDATE_AFFECT_LIVE;
        else
            flags |= VIR_NETWORK_UPDATE_AFFECT_CONFIG;
    }

    if (isActive && (flags & VIR_NETWORK_UPDATE_AFFECT_LIVE)) {
        /* Take care of anything that must be done before updating the
         * live NetworkDef.
         */
        if (network->def->forward.type == VIR_NETWORK_FORWARD_NONE ||
            network->def->forward.type == VIR_NETWORK_FORWARD_NAT ||
            network->def->forward.type == VIR_NETWORK_FORWARD_ROUTE) {
            switch (section) {
            case VIR_NETWORK_SECTION_FORWARD:
            case VIR_NETWORK_SECTION_FORWARD_INTERFACE:
            case VIR_NETWORK_SECTION_IP:
            case VIR_NETWORK_SECTION_IP_DHCP_RANGE:
            case VIR_NETWORK_SECTION_IP_DHCP_HOST:
                /* these could affect the firewall rules, so remove the
                 * old rules (and remember to load new ones after the
                 * update).
                 */
                networkRemoveFirewallRules(network->def);
                needFirewallRefresh = true;
                break;
            default:
                break;
            }
        }
    }

    /* update the network config in memory/on disk */
    if (virNetworkObjUpdate(network, command, section, parentIndex, xml, flags) < 0) {
        if (needFirewallRefresh)
            ignore_value(networkAddFirewallRules(network->def));
        goto cleanup;
    }

    if (needFirewallRefresh && networkAddFirewallRules(network->def) < 0)
        goto cleanup;

    if (flags & VIR_NETWORK_UPDATE_AFFECT_CONFIG) {
        /* save updated persistent config to disk */
        if (virNetworkSaveConfig(driver->networkConfigDir,
                                 virNetworkObjGetPersistentDef(network)) < 0) {
            goto cleanup;
        }
    }

    if (isActive && (flags & VIR_NETWORK_UPDATE_AFFECT_LIVE)) {
        /* rewrite dnsmasq host files, restart dnsmasq, update iptables
         * rules, etc, according to which section was modified. Note that
         * some sections require multiple actions, so a single switch
         * statement is inadequate.
         */
        if (section == VIR_NETWORK_SECTION_BRIDGE ||
            section == VIR_NETWORK_SECTION_DOMAIN ||
            section == VIR_NETWORK_SECTION_IP ||
            section == VIR_NETWORK_SECTION_IP_DHCP_RANGE) {
            /* these sections all change things on the dnsmasq commandline,
             * so we need to kill and restart dnsmasq.
             */
            if (networkRestartDhcpDaemon(network) < 0)
                goto cleanup;

        } else if (section == VIR_NETWORK_SECTION_IP_DHCP_HOST) {
            /* if we previously weren't listening for dhcp and now we
             * are (or vice-versa) then we need to do a restart,
             * otherwise we just need to do a refresh (redo the config
             * files and send SIGHUP)
             */
            bool newDhcpActive = false;

            for (i = 0;
                 (ipdef = virNetworkDefGetIpByIndex(network->def, AF_INET, i));
                 i++) {
                if (ipdef->nranges || ipdef->nhosts) {
                    newDhcpActive = true;
                    break;
                }
            }

            if ((newDhcpActive != oldDhcpActive &&
                 networkRestartDhcpDaemon(network) < 0) ||
                networkRefreshDhcpDaemon(network) < 0) {
                goto cleanup;
            }

        } else if (section == VIR_NETWORK_SECTION_DNS_HOST ||
                   section == VIR_NETWORK_SECTION_DNS_TXT ||
                   section == VIR_NETWORK_SECTION_DNS_SRV) {
            /* these sections only change things in config files, so we
             * can just update the config files and send SIGHUP to
             * dnsmasq.
             */
            if (networkRefreshDhcpDaemon(network) < 0)
                goto cleanup;

        }

        if (section == VIR_NETWORK_SECTION_IP) {
            /* only a change in IP addresses will affect radvd, and all of radvd's
             * config is stored in the conf file which will be re-read with a SIGHUP.
             */
            if (networkRefreshRadvd(network) < 0)
                goto cleanup;
        }

        /* save current network state to disk */
        if ((ret = virNetworkSaveStatus(driver->stateDir,
                                        network)) < 0) {
            goto cleanup;
        }
    }
    ret = 0;
 cleanup:
    if (network)
        virNetworkObjUnlock(network);
    networkDriverUnlock();
    return ret;
}

static int networkCreate(virNetworkPtr net)
{
    virNetworkObjPtr network;
    int ret = -1;
    virObjectEventPtr event = NULL;

    networkDriverLock();
    network = virNetworkFindByUUID(&driver->networks, net->uuid);

    if (!network) {
        virReportError(VIR_ERR_NO_NETWORK,
                       "%s", _("no network with matching uuid"));
        goto cleanup;
    }

    if (virNetworkCreateEnsureACL(net->conn, network->def) < 0)
        goto cleanup;

    if ((ret = networkStartNetwork(network)) < 0)
        goto cleanup;

    event = virNetworkEventLifecycleNew(network->def->name,
                                        network->def->uuid,
                                        VIR_NETWORK_EVENT_STARTED,
                                        0);

 cleanup:
    if (event)
        virObjectEventStateQueue(driver->networkEventState, event);
    if (network)
        virNetworkObjUnlock(network);
    networkDriverUnlock();
    return ret;
}

static int networkDestroy(virNetworkPtr net)
{
    virNetworkObjPtr network;
    int ret = -1;
    virObjectEventPtr event = NULL;

    networkDriverLock();
    network = virNetworkFindByUUID(&driver->networks, net->uuid);

    if (!network) {
        virReportError(VIR_ERR_NO_NETWORK,
                       "%s", _("no network with matching uuid"));
        goto cleanup;
    }

    if (virNetworkDestroyEnsureACL(net->conn, network->def) < 0)
        goto cleanup;

    if (!virNetworkObjIsActive(network)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("network '%s' is not active"),
                       network->def->name);
        goto cleanup;
    }

    if ((ret = networkShutdownNetwork(network)) < 0)
        goto cleanup;

    event = virNetworkEventLifecycleNew(network->def->name,
                                        network->def->uuid,
                                        VIR_NETWORK_EVENT_STOPPED,
                                        0);

    if (!network->persistent) {
        if (networkRemoveInactive(network) < 0) {
            network = NULL;
            ret = -1;
            goto cleanup;
        }
        network = NULL;
    }

 cleanup:
    if (event)
        virObjectEventStateQueue(driver->networkEventState, event);
    if (network)
        virNetworkObjUnlock(network);
    networkDriverUnlock();
    return ret;
}

static char *networkGetXMLDesc(virNetworkPtr net,
                               unsigned int flags)
{
    virNetworkObjPtr network;
    virNetworkDefPtr def;
    char *ret = NULL;

    virCheckFlags(VIR_NETWORK_XML_INACTIVE, NULL);

    if (!(network = networkObjFromNetwork(net)))
        return ret;

    if (virNetworkGetXMLDescEnsureACL(net->conn, network->def) < 0)
        goto cleanup;

    if ((flags & VIR_NETWORK_XML_INACTIVE) && network->newDef)
        def = network->newDef;
    else
        def = network->def;

    ret = virNetworkDefFormat(def, flags);

 cleanup:
    if (network)
        virNetworkObjUnlock(network);
    return ret;
}

static char *networkGetBridgeName(virNetworkPtr net) {
    virNetworkObjPtr network;
    char *bridge = NULL;

    if (!(network = networkObjFromNetwork(net)))
        return bridge;

    if (virNetworkGetBridgeNameEnsureACL(net->conn, network->def) < 0)
        goto cleanup;

    if (!(network->def->bridge)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("network '%s' does not have a bridge name."),
                       network->def->name);
        goto cleanup;
    }

    ignore_value(VIR_STRDUP(bridge, network->def->bridge));

 cleanup:
    if (network)
        virNetworkObjUnlock(network);
    return bridge;
}

static int networkGetAutostart(virNetworkPtr net,
                               int *autostart)
{
    virNetworkObjPtr network;
    int ret = -1;

    if (!(network = networkObjFromNetwork(net)))
        return ret;

    if (virNetworkGetAutostartEnsureACL(net->conn, network->def) < 0)
        goto cleanup;

    *autostart = network->autostart;
    ret = 0;

 cleanup:
    if (network)
        virNetworkObjUnlock(network);
    return ret;
}

static int networkSetAutostart(virNetworkPtr net,
                               int autostart)
{
    virNetworkObjPtr network;
    char *configFile = NULL, *autostartLink = NULL;
    int ret = -1;

    networkDriverLock();
    network = virNetworkFindByUUID(&driver->networks, net->uuid);

    if (!network) {
        virReportError(VIR_ERR_NO_NETWORK,
                       "%s", _("no network with matching uuid"));
        goto cleanup;
    }

    if (virNetworkSetAutostartEnsureACL(net->conn, network->def) < 0)
        goto cleanup;

    if (!network->persistent) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("cannot set autostart for transient network"));
        goto cleanup;
    }

    autostart = (autostart != 0);

    if (network->autostart != autostart) {
        if ((configFile = virNetworkConfigFile(driver->networkConfigDir, network->def->name)) == NULL)
            goto cleanup;
        if ((autostartLink = virNetworkConfigFile(driver->networkAutostartDir, network->def->name)) == NULL)
            goto cleanup;

        if (autostart) {
            if (virFileMakePath(driver->networkAutostartDir) < 0) {
                virReportSystemError(errno,
                                     _("cannot create autostart directory '%s'"),
                                     driver->networkAutostartDir);
                goto cleanup;
            }

            if (symlink(configFile, autostartLink) < 0) {
                virReportSystemError(errno,
                                     _("Failed to create symlink '%s' to '%s'"),
                                     autostartLink, configFile);
                goto cleanup;
            }
        } else {
            if (unlink(autostartLink) < 0 && errno != ENOENT && errno != ENOTDIR) {
                virReportSystemError(errno,
                                     _("Failed to delete symlink '%s'"),
                                     autostartLink);
                goto cleanup;
            }
        }

        network->autostart = autostart;
    }
    ret = 0;

 cleanup:
    VIR_FREE(configFile);
    VIR_FREE(autostartLink);
    if (network)
        virNetworkObjUnlock(network);
    networkDriverUnlock();
    return ret;
}

static int
networkGetDHCPLeases(virNetworkPtr network,
                     const char *mac,
                     virNetworkDHCPLeasePtr **leases,
                     unsigned int flags)
{
    size_t i, j;
    size_t nleases = 0;
    int rv = -1;
    int size = 0;
    int custom_lease_file_len = 0;
    bool need_results = !!leases;
    long long currtime = 0;
    long long expirytime_tmp = -1;
    bool ipv6 = false;
    char *lease_entries = NULL;
    char *custom_lease_file = NULL;
    const char *ip_tmp = NULL;
    const char *mac_tmp = NULL;
    virJSONValuePtr lease_tmp = NULL;
    virJSONValuePtr leases_array = NULL;
    virNetworkIpDefPtr ipdef_tmp = NULL;
    virNetworkDHCPLeasePtr lease = NULL;
    virNetworkDHCPLeasePtr *leases_ret = NULL;
    virNetworkObjPtr obj;

    virCheckFlags(0, -1);

    if (!(obj = networkObjFromNetwork(network)))
        return -1;

    if (virNetworkGetDHCPLeasesEnsureACL(network->conn, obj->def) < 0)
        goto cleanup;

    /* Retrieve custom leases file location */
    custom_lease_file = networkDnsmasqLeaseFileNameCustom(obj->def->bridge);

    /* Read entire contents */
    if ((custom_lease_file_len = virFileReadAll(custom_lease_file,
                                                VIR_NETWORK_DHCP_LEASE_FILE_SIZE_MAX,
                                                &lease_entries)) < 0) {
        /* Even though src/network/leaseshelper.c guarantees the existence of
         * leases file (even if no leases are present), and the control reaches
         * here, instead of reporting error, return 0 leases */
        rv = 0;
        goto error;
    }

    if (custom_lease_file_len) {
        if (!(leases_array = virJSONValueFromString(lease_entries))) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("invalid json in file: %s"), custom_lease_file);
            goto error;
        }

        if ((size = virJSONValueArraySize(leases_array)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("couldn't fetch array of leases"));
            goto error;
        }
    }

    currtime = (long long) time(NULL);

    for (i = 0; i < size; i++) {
        if (!(lease_tmp = virJSONValueArrayGet(leases_array, i))) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("failed to parse json"));
            goto error;
        }

        if (!(mac_tmp = virJSONValueObjectGetString(lease_tmp, "mac-address"))) {
            /* leaseshelper program guarantees that lease will be stored only if
             * mac-address is known otherwise not */
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("found lease without mac-address"));
            goto error;
        }

        if (mac && virMacAddrCompare(mac, mac_tmp))
            continue;

        if (virJSONValueObjectGetNumberLong(lease_tmp, "expiry-time", &expirytime_tmp) < 0) {
            /* A lease cannot be present without expiry-time */
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("found lease without expiry-time"));
            goto error;
        }

        /* Do not report expired lease */
        if (expirytime_tmp < currtime)
            continue;

        if (need_results) {
            if (VIR_ALLOC(lease) < 0)
                goto error;

            lease->expirytime = expirytime_tmp;

            if (!(ip_tmp = virJSONValueObjectGetString(lease_tmp, "ip-address"))) {
                /* A lease without ip-address makes no sense */
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("found lease without ip-address"));
                goto error;
            }

            /* Unlike IPv4, IPv6 uses ':' instead of '.' as separator */
            ipv6 = strchr(ip_tmp, ':') ? true : false;
            lease->type = ipv6 ? VIR_IP_ADDR_TYPE_IPV6 : VIR_IP_ADDR_TYPE_IPV4;

            /* Obtain prefix */
            for (j = 0; j < obj->def->nips; j++) {
                ipdef_tmp = &obj->def->ips[j];

                if (ipv6 && VIR_SOCKET_ADDR_IS_FAMILY(&ipdef_tmp->address,
                                                      AF_INET6)) {
                    lease->prefix = ipdef_tmp->prefix;
                    break;
                }
                if (!ipv6 && VIR_SOCKET_ADDR_IS_FAMILY(&ipdef_tmp->address,
                                                      AF_INET)) {
                    lease->prefix = virSocketAddrGetIpPrefix(&ipdef_tmp->address,
                                                             &ipdef_tmp->netmask,
                                                             ipdef_tmp->prefix);
                    break;
                }
            }

            if ((VIR_STRDUP(lease->mac, mac_tmp) < 0) ||
                (VIR_STRDUP(lease->ipaddr, ip_tmp) < 0) ||
                (VIR_STRDUP(lease->iface, obj->def->bridge) < 0))
                goto error;

            /* Fields that can be NULL */
            if ((VIR_STRDUP(lease->iaid,
                            virJSONValueObjectGetString(lease_tmp, "iaid")) < 0) ||
                (VIR_STRDUP(lease->clientid,
                            virJSONValueObjectGetString(lease_tmp, "client-id")) < 0) ||
                (VIR_STRDUP(lease->hostname,
                            virJSONValueObjectGetString(lease_tmp, "hostname")) < 0))
                goto error;

            if (VIR_INSERT_ELEMENT(leases_ret, nleases, nleases, lease) < 0)
                goto error;

        } else {
            nleases++;
        }

        VIR_FREE(lease);
    }

    if (leases_ret) {
        /* NULL terminated array */
        ignore_value(VIR_REALLOC_N(leases_ret, nleases + 1));
        *leases = leases_ret;
        leases_ret = NULL;
    }

    rv = nleases;

 cleanup:
    VIR_FREE(lease);
    VIR_FREE(lease_entries);
    VIR_FREE(custom_lease_file);
    virJSONValueFree(leases_array);

    if (obj)
        virNetworkObjUnlock(obj);

    return rv;

 error:
    if (leases_ret) {
        for (i = 0; i < nleases; i++)
            virNetworkDHCPLeaseFree(leases_ret[i]);
        VIR_FREE(leases_ret);
    }
    goto cleanup;
}


static virNetworkDriver networkDriver = {
    "Network",
    .networkOpen = networkOpen, /* 0.2.0 */
    .networkClose = networkClose, /* 0.2.0 */
    .connectNumOfNetworks = networkConnectNumOfNetworks, /* 0.2.0 */
    .connectListNetworks = networkConnectListNetworks, /* 0.2.0 */
    .connectNumOfDefinedNetworks = networkConnectNumOfDefinedNetworks, /* 0.2.0 */
    .connectListDefinedNetworks = networkConnectListDefinedNetworks, /* 0.2.0 */
    .connectListAllNetworks = networkConnectListAllNetworks, /* 0.10.2 */
    .connectNetworkEventRegisterAny = networkConnectNetworkEventRegisterAny, /* 1.2.1 */
    .connectNetworkEventDeregisterAny = networkConnectNetworkEventDeregisterAny, /* 1.2.1 */
    .networkLookupByUUID = networkLookupByUUID, /* 0.2.0 */
    .networkLookupByName = networkLookupByName, /* 0.2.0 */
    .networkCreateXML = networkCreateXML, /* 0.2.0 */
    .networkDefineXML = networkDefineXML, /* 0.2.0 */
    .networkUndefine = networkUndefine, /* 0.2.0 */
    .networkUpdate = networkUpdate, /* 0.10.2 */
    .networkCreate = networkCreate, /* 0.2.0 */
    .networkDestroy = networkDestroy, /* 0.2.0 */
    .networkGetXMLDesc = networkGetXMLDesc, /* 0.2.0 */
    .networkGetBridgeName = networkGetBridgeName, /* 0.2.0 */
    .networkGetAutostart = networkGetAutostart, /* 0.2.1 */
    .networkSetAutostart = networkSetAutostart, /* 0.2.1 */
    .networkIsActive = networkIsActive, /* 0.7.3 */
    .networkIsPersistent = networkIsPersistent, /* 0.7.3 */
    .networkGetDHCPLeases = networkGetDHCPLeases, /* 1.2.6 */
};

static virStateDriver networkStateDriver = {
    .name = "Network",
    .stateInitialize  = networkStateInitialize,
    .stateAutoStart  = networkStateAutoStart,
    .stateCleanup = networkStateCleanup,
    .stateReload = networkStateReload,
};

int networkRegister(void)
{
    if (virRegisterNetworkDriver(&networkDriver) < 0)
        return -1;
    if (virRegisterStateDriver(&networkStateDriver) < 0)
        return -1;
    return 0;
}

/********************************************************/

/* Private API to deal with logical switch capabilities.
 * These functions are exported so that other parts of libvirt can
 * call them, but are not part of the public API and not in the
 * driver's function table. If we ever have more than one network
 * driver, we will need to present these functions via a second
 * "backend" function table.
 */

/* networkAllocateActualDevice:
 * @dom: domain definition that @iface belongs to
 * @iface: the original NetDef from the domain
 *
 * Looks up the network reference by iface, allocates a physical
 * device from that network (if appropriate), and returns with the
 * virDomainActualNetDef filled in accordingly. If there are no
 * changes to be made in the netdef, then just leave the actualdef
 * empty.
 *
 * Returns 0 on success, -1 on failure.
 */
int
networkAllocateActualDevice(virDomainDefPtr dom,
                            virDomainNetDefPtr iface)
{
    virDomainNetType actualType = iface->type;
    virNetworkObjPtr network = NULL;
    virNetworkDefPtr netdef = NULL;
    virNetDevBandwidthPtr bandwidth = NULL;
    virPortGroupDefPtr portgroup = NULL;
    virNetDevVPortProfilePtr virtport = iface->virtPortProfile;
    virNetDevVlanPtr vlan = NULL;
    virNetworkForwardIfDefPtr dev = NULL;
    size_t i;
    int ret = -1;

    if (iface->type != VIR_DOMAIN_NET_TYPE_NETWORK)
        goto validate;

    virDomainActualNetDefFree(iface->data.network.actual);
    iface->data.network.actual = NULL;

    networkDriverLock();
    network = virNetworkFindByName(&driver->networks, iface->data.network.name);
    networkDriverUnlock();
    if (!network) {
        virReportError(VIR_ERR_NO_NETWORK,
                       _("no network with matching name '%s'"),
                       iface->data.network.name);
        goto error;
    }
    netdef = network->def;

    if (!virNetworkObjIsActive(network)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("network '%s' is not active"),
                       netdef->name);
        goto error;
    }

    if (VIR_ALLOC(iface->data.network.actual) < 0)
        goto error;

    /* portgroup can be present for any type of network, in particular
     * for bandwidth information, so we need to check for that and
     * fill it in appropriately for all forward types.
     */
    portgroup = virPortGroupFindByName(netdef, iface->data.network.portgroup);

    /* If there is already interface-specific bandwidth, just use that
     * (already in NetDef). Otherwise, if there is bandwidth info in
     * the portgroup, fill that into the ActualDef.
     */

    if (iface->bandwidth)
        bandwidth = iface->bandwidth;
    else if (portgroup && portgroup->bandwidth)
        bandwidth = portgroup->bandwidth;

    if (bandwidth && virNetDevBandwidthCopy(&iface->data.network.actual->bandwidth,
                                            bandwidth) < 0)
        goto error;

    /* copy appropriate vlan info to actualNet */
    if (iface->vlan.nTags > 0)
        vlan = &iface->vlan;
    else if (portgroup && portgroup->vlan.nTags > 0)
        vlan = &portgroup->vlan;
    else if (netdef->vlan.nTags > 0)
        vlan = &netdef->vlan;

    if (vlan && virNetDevVlanCopy(&iface->data.network.actual->vlan, vlan) < 0)
        goto error;

    if (iface->trustGuestRxFilters)
       iface->data.network.actual->trustGuestRxFilters
          = iface->trustGuestRxFilters;
    else if (portgroup && portgroup->trustGuestRxFilters)
       iface->data.network.actual->trustGuestRxFilters
          = portgroup->trustGuestRxFilters;
    else if (netdef->trustGuestRxFilters)
       iface->data.network.actual->trustGuestRxFilters
          = netdef->trustGuestRxFilters;

    if ((netdef->forward.type == VIR_NETWORK_FORWARD_NONE) ||
        (netdef->forward.type == VIR_NETWORK_FORWARD_NAT) ||
        (netdef->forward.type == VIR_NETWORK_FORWARD_ROUTE)) {
        /* for these forward types, the actual net type really *is*
         *NETWORK; we just keep the info from the portgroup in
         * iface->data.network.actual
         */
        iface->data.network.actual->type = VIR_DOMAIN_NET_TYPE_NETWORK;

        /* we also store the bridge device and macTableManager settings
         * in iface->data.network.actual->data.bridge for later use
         * after the domain's tap device is created (to attach to the
         * bridge and set flood/learning mode on the tap device)
         */
        if (VIR_STRDUP(iface->data.network.actual->data.bridge.brname,
                       netdef->bridge) < 0)
            goto error;
        iface->data.network.actual->data.bridge.macTableManager
           = netdef->macTableManager;

        if (networkPlugBandwidth(network, iface) < 0)
            goto error;

    } else if ((netdef->forward.type == VIR_NETWORK_FORWARD_BRIDGE) &&
               netdef->bridge) {

        /* <forward type='bridge'/> <bridge name='xxx'/>
         * is VIR_DOMAIN_NET_TYPE_BRIDGE
         */

        iface->data.network.actual->type = actualType = VIR_DOMAIN_NET_TYPE_BRIDGE;
        if (VIR_STRDUP(iface->data.network.actual->data.bridge.brname,
                       netdef->bridge) < 0)
            goto error;
        iface->data.network.actual->data.bridge.macTableManager
           = netdef->macTableManager;

        /* merge virtualports from interface, network, and portgroup to
         * arrive at actual virtualport to use
         */
        if (virNetDevVPortProfileMerge3(&iface->data.network.actual->virtPortProfile,
                                        iface->virtPortProfile,
                                        netdef->virtPortProfile,
                                        portgroup
                                        ? portgroup->virtPortProfile : NULL) < 0) {
            goto error;
        }
        virtport = iface->data.network.actual->virtPortProfile;
        if (virtport) {
            /* only type='openvswitch' is allowed for bridges */
            if (virtport->virtPortType != VIR_NETDEV_VPORT_PROFILE_OPENVSWITCH) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("<virtualport type='%s'> not supported for network "
                                 "'%s' which uses a bridge device"),
                               virNetDevVPortTypeToString(virtport->virtPortType),
                               netdef->name);
                goto error;
            }
        }

    } else if (netdef->forward.type == VIR_NETWORK_FORWARD_HOSTDEV) {

        virDomainHostdevSubsysPCIBackendType backend;

        iface->data.network.actual->type = actualType = VIR_DOMAIN_NET_TYPE_HOSTDEV;
        if (networkCreateInterfacePool(netdef) < 0)
            goto error;

        /* pick first dev with 0 connections */
        for (i = 0; i < netdef->forward.nifs; i++) {
            if (netdef->forward.ifs[i].connections == 0) {
                dev = &netdef->forward.ifs[i];
                break;
            }
        }
        if (!dev) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("network '%s' requires exclusive access "
                             "to interfaces, but none are available"),
                           netdef->name);
            goto error;
        }
        iface->data.network.actual->data.hostdev.def.parent.type = VIR_DOMAIN_DEVICE_NET;
        iface->data.network.actual->data.hostdev.def.parent.data.net = iface;
        iface->data.network.actual->data.hostdev.def.info = &iface->info;
        iface->data.network.actual->data.hostdev.def.mode = VIR_DOMAIN_HOSTDEV_MODE_SUBSYS;
        iface->data.network.actual->data.hostdev.def.managed = netdef->forward.managed ? 1 : 0;
        iface->data.network.actual->data.hostdev.def.source.subsys.type = dev->type;
        iface->data.network.actual->data.hostdev.def.source.subsys.u.pci.addr = dev->device.pci;

        switch (netdef->forward.driverName) {
        case VIR_NETWORK_FORWARD_DRIVER_NAME_DEFAULT:
            backend = VIR_DOMAIN_HOSTDEV_PCI_BACKEND_DEFAULT;
            break;
        case VIR_NETWORK_FORWARD_DRIVER_NAME_KVM:
            backend = VIR_DOMAIN_HOSTDEV_PCI_BACKEND_KVM;
            break;
        case VIR_NETWORK_FORWARD_DRIVER_NAME_VFIO:
            backend = VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO;
            break;
        default:
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unrecognized driver name value %d "
                             " in network '%s'"),
                           netdef->forward.driverName, netdef->name);
            goto error;
        }
        iface->data.network.actual->data.hostdev.def.source.subsys.u.pci.backend
            = backend;

        /* merge virtualports from interface, network, and portgroup to
         * arrive at actual virtualport to use
         */
        if (virNetDevVPortProfileMerge3(&iface->data.network.actual->virtPortProfile,
                                        iface->virtPortProfile,
                                        netdef->virtPortProfile,
                                        portgroup
                                        ? portgroup->virtPortProfile : NULL) < 0) {
            goto error;
        }
        virtport = iface->data.network.actual->virtPortProfile;
        if (virtport) {
            /* make sure type is supported for hostdev connections */
            if (virtport->virtPortType != VIR_NETDEV_VPORT_PROFILE_8021QBG &&
                virtport->virtPortType != VIR_NETDEV_VPORT_PROFILE_8021QBH) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("<virtualport type='%s'> not supported for network "
                                 "'%s' which uses an SR-IOV Virtual Function "
                                 "via PCI passthrough"),
                               virNetDevVPortTypeToString(virtport->virtPortType),
                               netdef->name);
                goto error;
            }
        }

    } else if ((netdef->forward.type == VIR_NETWORK_FORWARD_BRIDGE) ||
               (netdef->forward.type == VIR_NETWORK_FORWARD_PRIVATE) ||
               (netdef->forward.type == VIR_NETWORK_FORWARD_VEPA) ||
               (netdef->forward.type == VIR_NETWORK_FORWARD_PASSTHROUGH)) {

        /* <forward type='bridge|private|vepa|passthrough'> are all
         * VIR_DOMAIN_NET_TYPE_DIRECT.
         */

        /* Set type=direct and appropriate <source mode='xxx'/> */
        iface->data.network.actual->type = actualType = VIR_DOMAIN_NET_TYPE_DIRECT;
        switch (netdef->forward.type) {
        case VIR_NETWORK_FORWARD_BRIDGE:
            iface->data.network.actual->data.direct.mode = VIR_NETDEV_MACVLAN_MODE_BRIDGE;
            break;
        case VIR_NETWORK_FORWARD_PRIVATE:
            iface->data.network.actual->data.direct.mode = VIR_NETDEV_MACVLAN_MODE_PRIVATE;
            break;
        case VIR_NETWORK_FORWARD_VEPA:
            iface->data.network.actual->data.direct.mode = VIR_NETDEV_MACVLAN_MODE_VEPA;
            break;
        case VIR_NETWORK_FORWARD_PASSTHROUGH:
            iface->data.network.actual->data.direct.mode = VIR_NETDEV_MACVLAN_MODE_PASSTHRU;
            break;
        }

        /* merge virtualports from interface, network, and portgroup to
         * arrive at actual virtualport to use
         */
        if (virNetDevVPortProfileMerge3(&iface->data.network.actual->virtPortProfile,
                                        iface->virtPortProfile,
                                        netdef->virtPortProfile,
                                        portgroup
                                        ? portgroup->virtPortProfile : NULL) < 0) {
            goto error;
        }
        virtport = iface->data.network.actual->virtPortProfile;
        if (virtport) {
            /* make sure type is supported for macvtap connections */
            if (virtport->virtPortType != VIR_NETDEV_VPORT_PROFILE_8021QBG &&
                virtport->virtPortType != VIR_NETDEV_VPORT_PROFILE_8021QBH) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("<virtualport type='%s'> not supported for network "
                                 "'%s' which uses a macvtap device"),
                               virNetDevVPortTypeToString(virtport->virtPortType),
                               netdef->name);
                goto error;
            }
        }

        /* If there is only a single device, just return it (caller will detect
         * any error if exclusive use is required but could not be acquired).
         */
        if ((netdef->forward.nifs <= 0) && (netdef->forward.npfs <= 0)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("network '%s' uses a direct mode, but "
                             "has no forward dev and no interface pool"),
                           netdef->name);
            goto error;
        } else {
            /* pick an interface from the pool */

            if (networkCreateInterfacePool(netdef) < 0)
                goto error;

            /* PASSTHROUGH mode, and PRIVATE Mode + 802.1Qbh both
             * require exclusive access to a device, so current
             * connections count must be 0.  Other modes can share, so
             * just search for the one with the lowest number of
             * connections.
             */
            if ((netdef->forward.type == VIR_NETWORK_FORWARD_PASSTHROUGH) ||
                ((netdef->forward.type == VIR_NETWORK_FORWARD_PRIVATE) &&
                 iface->data.network.actual->virtPortProfile &&
                 (iface->data.network.actual->virtPortProfile->virtPortType
                  == VIR_NETDEV_VPORT_PROFILE_8021QBH))) {

                /* pick first dev with 0 connections */
                for (i = 0; i < netdef->forward.nifs; i++) {
                    if (netdef->forward.ifs[i].connections == 0) {
                        dev = &netdef->forward.ifs[i];
                        break;
                    }
                }
            } else {
                /* pick least used dev */
                dev = &netdef->forward.ifs[0];
                for (i = 1; i < netdef->forward.nifs; i++) {
                    if (netdef->forward.ifs[i].connections < dev->connections)
                        dev = &netdef->forward.ifs[i];
                }
            }
            /* dev points at the physical device we want to use */
            if (!dev) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("network '%s' requires exclusive access "
                                 "to interfaces, but none are available"),
                               netdef->name);
                goto error;
            }
            if (VIR_STRDUP(iface->data.network.actual->data.direct.linkdev,
                           dev->device.dev) < 0)
                goto error;
        }
    }

    if (virNetDevVPortProfileCheckComplete(virtport, true) < 0)
        goto error;

 validate:
    /* make sure that everything now specified for the device is
     * actually supported on this type of network. NB: network,
     * netdev, and iface->data.network.actual may all be NULL.
     */

    if (virDomainNetGetActualVlan(iface)) {
        /* vlan configuration via libvirt is only supported for
         * PCI Passthrough SR-IOV devices and openvswitch bridges.
         * otherwise log an error and fail
         */
        if (!(actualType == VIR_DOMAIN_NET_TYPE_HOSTDEV ||
              (actualType == VIR_DOMAIN_NET_TYPE_BRIDGE &&
               virtport && virtport->virtPortType
               == VIR_NETDEV_VPORT_PROFILE_OPENVSWITCH))) {
            if (netdef) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("an interface connecting to network '%s' "
                                 "is requesting a vlan tag, but that is not "
                                 "supported for this type of network"),
                               netdef->name);
            } else {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("an interface of type '%s' "
                                 "is requesting a vlan tag, but that is not "
                                 "supported for this type of connection"),
                               virDomainNetTypeToString(iface->type));
            }
            goto error;
        }
    }

    if (netdef) {
        netdef->connections++;
        VIR_DEBUG("Using network %s, %d connections",
                  netdef->name, netdef->connections);

        if (dev) {
            /* mark the allocation */
            dev->connections++;
            if (actualType != VIR_DOMAIN_NET_TYPE_HOSTDEV) {
                VIR_DEBUG("Using physical device %s, %d connections",
                          dev->device.dev, dev->connections);
            } else {
                VIR_DEBUG("Using physical device %04x:%02x:%02x.%x, connections %d",
                          dev->device.pci.domain, dev->device.pci.bus,
                          dev->device.pci.slot, dev->device.pci.function,
                          dev->connections);
            }
        }

        /* finally we can call the 'plugged' hook script if any */
        if (networkRunHook(network, dom, iface,
                           VIR_HOOK_NETWORK_OP_IFACE_PLUGGED,
                           VIR_HOOK_SUBOP_BEGIN) < 0) {
            /* adjust for failure */
            netdef->connections--;
            if (dev)
                dev->connections--;
            goto error;
        }
    }

    ret = 0;

 cleanup:
    if (network)
        virNetworkObjUnlock(network);
    return ret;

 error:
    if (iface->type == VIR_DOMAIN_NET_TYPE_NETWORK) {
        virDomainActualNetDefFree(iface->data.network.actual);
        iface->data.network.actual = NULL;
    }
    goto cleanup;
}

/* networkNotifyActualDevice:
 * @dom: domain definition that @iface belongs to
 * @iface:  the domain's NetDef with an "actual" device already filled in.
 *
 * Called to notify the network driver when libvirtd is restarted and
 * finds an already running domain. If appropriate it will force an
 * allocation of the actual->direct.linkdev to get everything back in
 * order.
 *
 * Returns 0 on success, -1 on failure.
 */
int
networkNotifyActualDevice(virDomainDefPtr dom,
                          virDomainNetDefPtr iface)
{
    virDomainNetType actualType = virDomainNetGetActualType(iface);
    virNetworkObjPtr network;
    virNetworkDefPtr netdef;
    virNetworkForwardIfDefPtr dev = NULL;
    size_t i;
    int ret = -1;

    if (iface->type != VIR_DOMAIN_NET_TYPE_NETWORK)
        return 0;

    networkDriverLock();
    network = virNetworkFindByName(&driver->networks, iface->data.network.name);
    networkDriverUnlock();
    if (!network) {
        virReportError(VIR_ERR_NO_NETWORK,
                       _("no network with matching name '%s'"),
                       iface->data.network.name);
        goto error;
    }
    netdef = network->def;

    /* if we're restarting libvirtd after an upgrade from a version
     * that didn't save bridge name in actualNetDef for
     * actualType==network, we need to copy it in so that it will be
     * available in all cases
     */
    if (actualType == VIR_DOMAIN_NET_TYPE_NETWORK &&
        !iface->data.network.actual->data.bridge.brname &&
        (VIR_STRDUP(iface->data.network.actual->data.bridge.brname,
                    netdef->bridge) < 0))
            goto error;

    if (!iface->data.network.actual ||
        (actualType != VIR_DOMAIN_NET_TYPE_DIRECT &&
         actualType != VIR_DOMAIN_NET_TYPE_HOSTDEV)) {
        VIR_DEBUG("Nothing to claim from network %s", iface->data.network.name);
        goto success;
    }

    if (networkCreateInterfacePool(netdef) < 0)
        goto error;

    if (netdef->forward.nifs == 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("network '%s' uses a direct or hostdev mode, "
                         "but has no forward dev and no interface pool"),
                       netdef->name);
        goto error;
    }

    if (actualType == VIR_DOMAIN_NET_TYPE_DIRECT) {
        const char *actualDev;

        actualDev = virDomainNetGetActualDirectDev(iface);
        if (!actualDev) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("the interface uses a direct mode, "
                             "but has no source dev"));
            goto error;
        }

        /* find the matching interface and increment its connections */
        for (i = 0; i < netdef->forward.nifs; i++) {
            if (netdef->forward.ifs[i].type
                == VIR_NETWORK_FORWARD_HOSTDEV_DEVICE_NETDEV &&
                STREQ(actualDev, netdef->forward.ifs[i].device.dev)) {
                dev = &netdef->forward.ifs[i];
                break;
            }
        }
        /* dev points at the physical device we want to use */
        if (!dev) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("network '%s' doesn't have dev='%s' "
                             "in use by domain"),
                           netdef->name, actualDev);
            goto error;
        }

        /* PASSTHROUGH mode and PRIVATE Mode + 802.1Qbh both require
         * exclusive access to a device, so current connections count
         * must be 0 in those cases.
         */
        if ((dev->connections > 0) &&
            ((netdef->forward.type == VIR_NETWORK_FORWARD_PASSTHROUGH) ||
             ((netdef->forward.type == VIR_NETWORK_FORWARD_PRIVATE) &&
              iface->data.network.actual->virtPortProfile &&
              (iface->data.network.actual->virtPortProfile->virtPortType
               == VIR_NETDEV_VPORT_PROFILE_8021QBH)))) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("network '%s' claims dev='%s' is already in "
                             "use by a different domain"),
                           netdef->name, actualDev);
            goto error;
        }

        /* we are now assured of success, so mark the allocation */
        dev->connections++;
        VIR_DEBUG("Using physical device %s, connections %d",
                  dev->device.dev, dev->connections);

    }  else /* if (actualType == VIR_DOMAIN_NET_TYPE_HOSTDEV) */ {
        virDomainHostdevDefPtr hostdev;

        hostdev = virDomainNetGetActualHostdev(iface);
        if (!hostdev) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("the interface uses a hostdev mode, "
                             "but has no hostdev"));
            goto error;
        }

        /* find the matching interface and increment its connections */
        for (i = 0; i < netdef->forward.nifs; i++) {
            if (netdef->forward.ifs[i].type
                == VIR_NETWORK_FORWARD_HOSTDEV_DEVICE_PCI &&
                virDevicePCIAddressEqual(&hostdev->source.subsys.u.pci.addr,
                                         &netdef->forward.ifs[i].device.pci)) {
                dev = &netdef->forward.ifs[i];
                break;
            }
        }
        /* dev points at the physical device we want to use */
        if (!dev) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("network '%s' doesn't have "
                             "PCI device %04x:%02x:%02x.%x in use by domain"),
                           netdef->name,
                           hostdev->source.subsys.u.pci.addr.domain,
                           hostdev->source.subsys.u.pci.addr.bus,
                           hostdev->source.subsys.u.pci.addr.slot,
                           hostdev->source.subsys.u.pci.addr.function);
            goto error;
        }

        /* PASSTHROUGH mode, PRIVATE Mode + 802.1Qbh, and hostdev (PCI
         * passthrough) all require exclusive access to a device, so
         * current connections count must be 0 in those cases.
         */
        if ((dev->connections > 0) &&
            netdef->forward.type == VIR_NETWORK_FORWARD_HOSTDEV) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("network '%s' claims the PCI device at "
                             "domain=%d bus=%d slot=%d function=%d "
                             "is already in use by a different domain"),
                           netdef->name,
                           dev->device.pci.domain, dev->device.pci.bus,
                           dev->device.pci.slot, dev->device.pci.function);
            goto error;
        }

        /* we are now assured of success, so mark the allocation */
        dev->connections++;
        VIR_DEBUG("Using physical device %04x:%02x:%02x.%x, connections %d",
                  dev->device.pci.domain, dev->device.pci.bus,
                  dev->device.pci.slot, dev->device.pci.function,
                  dev->connections);
    }

 success:
    netdef->connections++;
    VIR_DEBUG("Using network %s, %d connections",
              netdef->name, netdef->connections);

    /* finally we can call the 'plugged' hook script if any */
    if (networkRunHook(network, dom, iface, VIR_HOOK_NETWORK_OP_IFACE_PLUGGED,
                       VIR_HOOK_SUBOP_BEGIN) < 0) {
        /* adjust for failure */
        if (dev)
            dev->connections--;
        netdef->connections--;
        goto error;
    }

    ret = 0;
 cleanup:
    if (network)
        virNetworkObjUnlock(network);
    return ret;

 error:
    goto cleanup;
}


/* networkReleaseActualDevice:
 * @dom: domain definition that @iface belongs to
 * @iface:  a domain's NetDef (interface definition)
 *
 * Given a domain <interface> element that previously had its <actual>
 * element filled in (and possibly a physical device allocated to it),
 * free up the physical device for use by someone else, and free the
 * virDomainActualNetDef.
 *
 * Returns 0 on success, -1 on failure.
 */
int
networkReleaseActualDevice(virDomainDefPtr dom,
                           virDomainNetDefPtr iface)
{
    virDomainNetType actualType = virDomainNetGetActualType(iface);
    virNetworkObjPtr network;
    virNetworkDefPtr netdef;
    virNetworkForwardIfDefPtr dev = NULL;
    size_t i;
    int ret = -1;

    if (iface->type != VIR_DOMAIN_NET_TYPE_NETWORK)
        return 0;

    networkDriverLock();
    network = virNetworkFindByName(&driver->networks, iface->data.network.name);
    networkDriverUnlock();
    if (!network) {
        virReportError(VIR_ERR_NO_NETWORK,
                       _("no network with matching name '%s'"),
                       iface->data.network.name);
        goto error;
    }
    netdef = network->def;

    if (iface->data.network.actual &&
        (netdef->forward.type == VIR_NETWORK_FORWARD_NONE ||
         netdef->forward.type == VIR_NETWORK_FORWARD_NAT ||
         netdef->forward.type == VIR_NETWORK_FORWARD_ROUTE) &&
        networkUnplugBandwidth(network, iface) < 0)
        goto error;

    if ((!iface->data.network.actual) ||
        ((actualType != VIR_DOMAIN_NET_TYPE_DIRECT) &&
         (actualType != VIR_DOMAIN_NET_TYPE_HOSTDEV))) {
        VIR_DEBUG("Nothing to release to network %s", iface->data.network.name);
        goto success;
    }

    if (netdef->forward.nifs == 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("network '%s' uses a direct/hostdev mode, but "
                         "has no forward dev and no interface pool"),
                       netdef->name);
        goto error;
    }

    if (actualType == VIR_DOMAIN_NET_TYPE_DIRECT) {
        const char *actualDev;

        actualDev = virDomainNetGetActualDirectDev(iface);
        if (!actualDev) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("the interface uses a direct mode, "
                             "but has no source dev"));
            goto error;
        }

        for (i = 0; i < netdef->forward.nifs; i++) {
            if (netdef->forward.ifs[i].type
                == VIR_NETWORK_FORWARD_HOSTDEV_DEVICE_NETDEV &&
                STREQ(actualDev, netdef->forward.ifs[i].device.dev)) {
                dev = &netdef->forward.ifs[i];
                break;
            }
        }

        if (!dev) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("network '%s' doesn't have dev='%s' "
                             "in use by domain"),
                           netdef->name, actualDev);
            goto error;
        }

        dev->connections--;
        VIR_DEBUG("Releasing physical device %s, connections %d",
                  dev->device.dev, dev->connections);

    } else /* if (actualType == VIR_DOMAIN_NET_TYPE_HOSTDEV) */ {
        virDomainHostdevDefPtr hostdev;

        hostdev = virDomainNetGetActualHostdev(iface);
        if (!hostdev) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("the interface uses a hostdev mode, but has no hostdev"));
            goto error;
        }

        for (i = 0; i < netdef->forward.nifs; i++) {
            if (netdef->forward.ifs[i].type
                == VIR_NETWORK_FORWARD_HOSTDEV_DEVICE_PCI &&
                virDevicePCIAddressEqual(&hostdev->source.subsys.u.pci.addr,
                                         &netdef->forward.ifs[i].device.pci)) {
                dev = &netdef->forward.ifs[i];
                break;
            }
        }

        if (!dev) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("network '%s' doesn't have "
                             "PCI device %04x:%02x:%02x.%x in use by domain"),
                           netdef->name,
                           hostdev->source.subsys.u.pci.addr.domain,
                           hostdev->source.subsys.u.pci.addr.bus,
                           hostdev->source.subsys.u.pci.addr.slot,
                           hostdev->source.subsys.u.pci.addr.function);
            goto error;
        }

        dev->connections--;
        VIR_DEBUG("Releasing physical device %04x:%02x:%02x.%x, connections %d",
                  dev->device.pci.domain, dev->device.pci.bus,
                  dev->device.pci.slot, dev->device.pci.function,
                  dev->connections);
    }

 success:
    if (iface->data.network.actual) {
        netdef->connections--;
        VIR_DEBUG("Releasing network %s, %d connections",
                  netdef->name, netdef->connections);

        /* finally we can call the 'unplugged' hook script if any */
        networkRunHook(network, dom, iface, VIR_HOOK_NETWORK_OP_IFACE_UNPLUGGED,
                       VIR_HOOK_SUBOP_BEGIN);
    }
    ret = 0;
 cleanup:
    if (network)
        virNetworkObjUnlock(network);
    if (iface->type == VIR_DOMAIN_NET_TYPE_NETWORK) {
        virDomainActualNetDefFree(iface->data.network.actual);
        iface->data.network.actual = NULL;
    }
    return ret;

 error:
    goto cleanup;
}

/*
 * networkGetNetworkAddress:
 * @netname: the name of a network
 * @netaddr: string representation of IP address for that network.
 *
 * Attempt to return an IP (v4) address associated with the named
 * network. If a libvirt virtual network, that will be provided in the
 * configuration. For host bridge and direct (macvtap) networks, we
 * must do an ioctl to learn the address.
 *
 * Note: This function returns the 1st IPv4 address it finds. It might
 * be useful if it was more flexible, but the current use (getting a
 * listen address for qemu's vnc/spice graphics server) can only use a
 * single address anyway.
 *
 * Returns 0 on success, and puts a string (which must be free'd by
 * the caller) into *netaddr. Returns -1 on failure or -2 if
 * completely unsupported.
 */
int
networkGetNetworkAddress(const char *netname, char **netaddr)
{
    int ret = -1;
    virNetworkObjPtr network;
    virNetworkDefPtr netdef;
    virNetworkIpDefPtr ipdef;
    virSocketAddr addr;
    virSocketAddrPtr addrptr = NULL;
    char *dev_name = NULL;

    *netaddr = NULL;
    networkDriverLock();
    network = virNetworkFindByName(&driver->networks, netname);
    networkDriverUnlock();
    if (!network) {
        virReportError(VIR_ERR_NO_NETWORK,
                       _("no network with matching name '%s'"),
                       netname);
        goto error;
    }
    netdef = network->def;

    switch (netdef->forward.type) {
    case VIR_NETWORK_FORWARD_NONE:
    case VIR_NETWORK_FORWARD_NAT:
    case VIR_NETWORK_FORWARD_ROUTE:
        /* if there's an ipv4def, get it's address */
        ipdef = virNetworkDefGetIpByIndex(netdef, AF_INET, 0);
        if (!ipdef) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("network '%s' doesn't have an IPv4 address"),
                           netdef->name);
            break;
        }
        addrptr = &ipdef->address;
        break;

    case VIR_NETWORK_FORWARD_BRIDGE:
        if ((dev_name = netdef->bridge))
            break;
        /*
         * fall through if netdef->bridge wasn't set, since this is
         * also a direct-mode interface.
         */
    case VIR_NETWORK_FORWARD_PRIVATE:
    case VIR_NETWORK_FORWARD_VEPA:
    case VIR_NETWORK_FORWARD_PASSTHROUGH:
        if ((netdef->forward.nifs > 0) && netdef->forward.ifs)
            dev_name = netdef->forward.ifs[0].device.dev;

        if (!dev_name) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("network '%s' has no associated interface or bridge"),
                           netdef->name);
        }
        break;
    }

    if (dev_name) {
        if (virNetDevGetIPv4Address(dev_name, &addr) < 0)
            goto error;
        addrptr = &addr;
    }

    if (!(addrptr &&
          (*netaddr = virSocketAddrFormat(addrptr)))) {
        goto error;
    }

    ret = 0;
 cleanup:
    if (network)
        virNetworkObjUnlock(network);
    return ret;

 error:
    goto cleanup;
}

/**
 * networkCheckBandwidth:
 * @net: network QoS
 * @iface: interface QoS
 * @new_rate: new rate for non guaranteed class
 *
 * Returns: -1 if plugging would overcommit network QoS
 *           0 if plugging is safe (@new_rate updated)
 *           1 if no QoS is set (@new_rate untouched)
 */
static int
networkCheckBandwidth(virNetworkObjPtr net,
                      virDomainNetDefPtr iface,
                      unsigned long long *new_rate)
{
    int ret = -1;
    virNetDevBandwidthPtr netBand = net->def->bandwidth;
    virNetDevBandwidthPtr ifaceBand = virDomainNetGetActualBandwidth(iface);
    unsigned long long tmp_floor_sum = net->floor_sum;
    unsigned long long tmp_new_rate = 0;
    char ifmac[VIR_MAC_STRING_BUFLEN];

    virMacAddrFormat(&iface->mac, ifmac);

    if (ifaceBand && ifaceBand->in && ifaceBand->in->floor &&
        !(netBand && netBand->in)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("Invalid use of 'floor' on interface with MAC "
                         "address %s - network '%s' has no inbound QoS set"),
                       ifmac, net->def->name);
        return -1;
    }

    if (!ifaceBand || !ifaceBand->in || !ifaceBand->in->floor ||
        !netBand || !netBand->in) {
        /* no QoS required, claim success */
        return 1;
    }

    tmp_new_rate = netBand->in->average;
    tmp_floor_sum += ifaceBand->in->floor;

    /* check against peak */
    if (netBand->in->peak) {
        tmp_new_rate = netBand->in->peak;
        if (tmp_floor_sum > netBand->in->peak) {
            virReportError(VIR_ERR_OPERATION_INVALID,
                           _("Cannot plug '%s' interface into '%s' because it "
                             "would overcommit 'peak' on network '%s'"),
                           ifmac,
                           net->def->bridge,
                           net->def->name);
            goto cleanup;
        }
    } else if (tmp_floor_sum > netBand->in->average) {
        /* tmp_floor_sum can be between 'average' and 'peak' iff 'peak' is set.
         * Otherwise, tmp_floor_sum must be below 'average'. */
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("Cannot plug '%s' interface into '%s' because it "
                         "would overcommit 'average' on network '%s'"),
                       ifmac,
                       net->def->bridge,
                       net->def->name);
        goto cleanup;
    }

    *new_rate = tmp_new_rate;
    ret = 0;

 cleanup:
    return ret;
}

/**
 * networkNextClassID:
 * @net: network object
 *
 * Find next free class ID. @net is supposed
 * to be locked already. If there is a free ID,
 * it is marked as used and returned.
 *
 * Returns next free class ID or -1 if none is available.
 */
static ssize_t
networkNextClassID(virNetworkObjPtr net)
{
    size_t ret = 0;
    bool is_set = false;

    while (virBitmapGetBit(net->class_id, ret, &is_set) == 0 && is_set)
        ret++;

    if (is_set || virBitmapSetBit(net->class_id, ret) < 0)
        return -1;

    return ret;
}

static int
networkPlugBandwidth(virNetworkObjPtr net,
                     virDomainNetDefPtr iface)
{
    int ret = -1;
    int plug_ret;
    unsigned long long new_rate = 0;
    ssize_t class_id = 0;
    char ifmac[VIR_MAC_STRING_BUFLEN];
    virNetDevBandwidthPtr ifaceBand = virDomainNetGetActualBandwidth(iface);

    if ((plug_ret = networkCheckBandwidth(net, iface, &new_rate)) < 0) {
        /* helper reported error */
        goto cleanup;
    }

    if (plug_ret > 0) {
        /* no QoS needs to be set; claim success */
        ret = 0;
        goto cleanup;
    }

    virMacAddrFormat(&iface->mac, ifmac);
    if (iface->type != VIR_DOMAIN_NET_TYPE_NETWORK ||
        !iface->data.network.actual) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Cannot set bandwidth on interface '%s' of type %d"),
                       ifmac, iface->type);
        goto cleanup;
    }

    /* generate new class_id */
    if ((class_id = networkNextClassID(net)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not generate next class ID"));
        goto cleanup;
    }

    plug_ret = virNetDevBandwidthPlug(net->def->bridge, net->def->bandwidth,
                                      &iface->mac, ifaceBand, class_id);
    if (plug_ret < 0) {
        ignore_value(virNetDevBandwidthUnplug(net->def->bridge, class_id));
        goto cleanup;
    }

    /* QoS was set, generate new class ID */
    iface->data.network.actual->class_id = class_id;
    /* update sum of 'floor'-s of attached NICs */
    net->floor_sum += ifaceBand->in->floor;
    /* update status file */
    if (virNetworkSaveStatus(driver->stateDir, net) < 0) {
        ignore_value(virBitmapClearBit(net->class_id, class_id));
        net->floor_sum -= ifaceBand->in->floor;
        iface->data.network.actual->class_id = 0;
        ignore_value(virNetDevBandwidthUnplug(net->def->bridge, class_id));
        goto cleanup;
    }
    /* update rate for non guaranteed NICs */
    new_rate -= net->floor_sum;
    if (virNetDevBandwidthUpdateRate(net->def->bridge, "1:2",
                                     net->def->bandwidth, new_rate) < 0)
        VIR_WARN("Unable to update rate for 1:2 class on %s bridge",
                 net->def->bridge);

    ret = 0;

 cleanup:
    return ret;
}

static int
networkUnplugBandwidth(virNetworkObjPtr net,
                       virDomainNetDefPtr iface)
{
    int ret = 0;
    unsigned long long new_rate;
    virNetDevBandwidthPtr ifaceBand = virDomainNetGetActualBandwidth(iface);

    if (iface->data.network.actual &&
        iface->data.network.actual->class_id) {
        if (!net->def->bandwidth || !net->def->bandwidth->in) {
            VIR_WARN("Network %s has no bandwidth but unplug requested",
                     net->def->name);
            goto cleanup;
        }
        /* we must remove class from bridge */
        new_rate = net->def->bandwidth->in->average;

        if (net->def->bandwidth->in->peak > 0)
            new_rate = net->def->bandwidth->in->peak;

        ret = virNetDevBandwidthUnplug(net->def->bridge,
                                       iface->data.network.actual->class_id);
        if (ret < 0)
            goto cleanup;
        /* update sum of 'floor'-s of attached NICs */
        net->floor_sum -= ifaceBand->in->floor;
        /* return class ID */
        ignore_value(virBitmapClearBit(net->class_id,
                                       iface->data.network.actual->class_id));
        /* update status file */
        if (virNetworkSaveStatus(driver->stateDir, net) < 0) {
            net->floor_sum += ifaceBand->in->floor;
            ignore_value(virBitmapSetBit(net->class_id,
                                         iface->data.network.actual->class_id));
            goto cleanup;
        }
        /* update rate for non guaranteed NICs */
        new_rate -= net->floor_sum;
        if (virNetDevBandwidthUpdateRate(net->def->bridge, "1:2",
                                         net->def->bandwidth, new_rate) < 0)
            VIR_WARN("Unable to update rate for 1:2 class on %s bridge",
                     net->def->bridge);
        /* no class is associated any longer */
        iface->data.network.actual->class_id = 0;
    }

 cleanup:
    return ret;
}

static void
networkNetworkObjTaint(virNetworkObjPtr net,
                       virNetworkTaintFlags taint)
{
    if (virNetworkObjTaint(net, taint)) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(net->def->uuid, uuidstr);

        VIR_WARN("Network name='%s' uuid=%s is tainted: %s",
                 net->def->name,
                 uuidstr,
                 virNetworkTaintTypeToString(taint));
    }
}
