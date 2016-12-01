/*
 * Copyright (C) 2007-2014 Red Hat, Inc.
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
 * Authors:
 *     Mark McLoughlin <markmc@redhat.com>
 *     Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include "virmacaddr.h"
#include "virnetdevtap.h"
#include "virnetdev.h"
#include "virnetdevbridge.h"
#include "virnetdevopenvswitch.h"
#include "virerror.h"
#include "virfile.h"
#include "viralloc.h"
#include "virlog.h"
#include "virstring.h"

#include <dirent.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <fcntl.h>
#ifdef __linux__
# include <linux/if_tun.h>    /* IFF_TUN, IFF_NO_PI */
#elif defined(__FreeBSD__)
# include <net/if_tap.h>
#endif

#define VIR_FROM_THIS VIR_FROM_NONE

VIR_LOG_INIT("util.netdevtap");

/**
 * virNetDevTapGetName:
 * @tapfd: a tun/tap file descriptor
 * @ifname: a pointer that will receive the interface name
 *
 * Retrieve the interface name given a file descriptor for a tun/tap
 * interface.
 *
 * Returns 0 if the interface name is successfully queried, -1 otherwise
 */
int
virNetDevTapGetName(int tapfd ATTRIBUTE_UNUSED, char **ifname ATTRIBUTE_UNUSED)
{
#ifdef TUNGETIFF
    struct ifreq ifr;

    if (ioctl(tapfd, TUNGETIFF, &ifr) < 0) {
        virReportSystemError(errno, "%s",
                             _("Unable to query tap interface name"));
        return -1;
    }

    return VIR_STRDUP(*ifname, ifr.ifr_name) < 0 ? -1 : 0;
#else
    return -1;
#endif
}

/**
 * virNetDevTapGetRealDeviceName:
 * @ifname: the interface name
 *
 * Lookup real interface name (i.e. name of the device entry in /dev),
 * because e.g. on FreeBSD if we rename tap device to vnetN its device
 * entry still remains unchanged (/dev/tapX), but bhyve needs a name
 * that matches /dev entry.
 *
 * Returns the proper interface name or NULL if no corresponding interface
 * found.
 */
char*
virNetDevTapGetRealDeviceName(char *ifname ATTRIBUTE_UNUSED)
{
#ifdef TAPGIFNAME
    char *ret = NULL;
    struct dirent *dp;
    char *devpath = NULL;
    int fd;

    DIR *dirp = opendir("/dev");
    if (dirp == NULL) {
        virReportSystemError(errno,
                             _("Failed to opendir path '%s'"),
                             "/dev");
        return NULL;
    }

    while (virDirRead(dirp, &dp, "/dev") > 0) {
        if (STRPREFIX(dp->d_name, "tap")) {
            struct ifreq ifr;
            if (virAsprintf(&devpath, "/dev/%s", dp->d_name) < 0)
                goto cleanup;
            if ((fd = open(devpath, O_RDWR)) < 0) {
                if (errno == EBUSY) {
                    VIR_FREE(devpath);
                    continue;
                }

                virReportSystemError(errno, _("Unable to open '%s'"), devpath);
                goto cleanup;
            }

            if (ioctl(fd, TAPGIFNAME, (void *)&ifr) < 0) {
                virReportSystemError(errno, "%s",
                                     _("Unable to query tap interface name"));
                goto cleanup;
            }

            if (STREQ(ifname, ifr.ifr_name)) {
                /* we can ignore the return value
                 * because we still have nothing
                 * to do but return;
                 */
                ignore_value(VIR_STRDUP(ret, dp->d_name));
                goto cleanup;
            }

            VIR_FREE(devpath);
            VIR_FORCE_CLOSE(fd);
        }
    }

 cleanup:
    VIR_FREE(devpath);
    VIR_FORCE_CLOSE(fd);
    closedir(dirp);
    return ret;
#else
    return NULL;
#endif
}


/**
 * virNetDevProbeVnetHdr:
 * @tapfd: a tun/tap file descriptor
 *
 * Check whether it is safe to enable the IFF_VNET_HDR flag on the
 * tap interface.
 *
 * Setting IFF_VNET_HDR enables QEMU's virtio_net driver to allow
 * guests to pass larger (GSO) packets, with partial checksums, to
 * the host. This greatly increases the achievable throughput.
 *
 * It is only useful to enable this when we're setting up a virtio
 * interface. And it is only *safe* to enable it when we know for
 * sure that a) qemu has support for IFF_VNET_HDR and b) the running
 * kernel implements the TUNGETIFF ioctl(), which qemu needs to query
 * the supplied tapfd.
 *
 * Returns 1 if VnetHdr is supported, 0 if not supported
 */
#ifdef IFF_VNET_HDR
static int
virNetDevProbeVnetHdr(int tapfd)
{
# if defined(IFF_VNET_HDR) && defined(TUNGETFEATURES) && defined(TUNGETIFF)
    unsigned int features;
    struct ifreq dummy;

    if (ioctl(tapfd, TUNGETFEATURES, &features) != 0) {
        VIR_INFO("Not enabling IFF_VNET_HDR; "
                 "TUNGETFEATURES ioctl() not implemented");
        return 0;
    }

    if (!(features & IFF_VNET_HDR)) {
        VIR_INFO("Not enabling IFF_VNET_HDR; "
                 "TUNGETFEATURES ioctl() reports no IFF_VNET_HDR");
        return 0;
    }

    /* The kernel will always return -1 at this point.
     * If TUNGETIFF is not implemented then errno == EBADFD.
     */
    if (ioctl(tapfd, TUNGETIFF, &dummy) != -1 || errno != EBADFD) {
        VIR_INFO("Not enabling IFF_VNET_HDR; "
                 "TUNGETIFF ioctl() not implemented");
        return 0;
    }

    VIR_INFO("Enabling IFF_VNET_HDR");

    return 1;
# else
    (void) tapfd;
    VIR_INFO("Not enabling IFF_VNET_HDR; disabled at build time");
    return 0;
# endif
}
#endif


#ifdef TUNSETIFF
/**
 * virNetDevTapCreate:
 * @ifname: the interface name
 * @tunpath: path to the tun device (if NULL, /dev/net/tun is used)
 * @tapfds: array of file descriptors return value for the new tap device
 * @tapfdSize: number of file descriptors in @tapfd
 * @flags: OR of virNetDevTapCreateFlags. Only one flag is recognized:
 *
 *   VIR_NETDEV_TAP_CREATE_VNET_HDR
 *     - Enable IFF_VNET_HDR on the tap device
 *   VIR_NETDEV_TAP_CREATE_PERSIST
 *     - The device will persist after the file descriptor is closed
 *
 * Creates a tap interface. The caller must use virNetDevTapDelete to
 * remove a persistent TAP device when it is no longer needed. In case
 * @tapfdSize is greater than one, multiqueue extension is requested
 * from kernel.
 *
 * Returns 0 in case of success or -1 on failure.
 */
int virNetDevTapCreate(char **ifname,
                       const char *tunpath,
                       int *tapfd,
                       size_t tapfdSize,
                       unsigned int flags)
{
    size_t i;
    struct ifreq ifr;
    int ret = -1;
    int fd;

    if (!tunpath)
        tunpath = "/dev/net/tun";

    memset(&ifr, 0, sizeof(ifr));
    for (i = 0; i < tapfdSize; i++) {
        if ((fd = open(tunpath, O_RDWR)) < 0) {
            virReportSystemError(errno,
                                 _("Unable to open %s, is tun module loaded?"),
                                 tunpath);
            goto cleanup;
        }

        memset(&ifr, 0, sizeof(ifr));

        ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
        /* If tapfdSize is greater than one, request multiqueue */
        if (tapfdSize > 1) {
# ifdef IFF_MULTI_QUEUE
            ifr.ifr_flags |= IFF_MULTI_QUEUE;
# else
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Multiqueue devices are not supported on this system"));
            goto cleanup;
# endif
        }

# ifdef IFF_VNET_HDR
        if ((flags &  VIR_NETDEV_TAP_CREATE_VNET_HDR) &&
            virNetDevProbeVnetHdr(fd))
            ifr.ifr_flags |= IFF_VNET_HDR;
# endif

        if (virStrcpyStatic(ifr.ifr_name, *ifname) == NULL) {
            virReportSystemError(ERANGE,
                                 _("Network interface name '%s' is too long"),
                                 *ifname);
            goto cleanup;

        }

        if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
            virReportSystemError(errno,
                                 _("Unable to create tap device %s"),
                                 NULLSTR(*ifname));
            goto cleanup;
        }

        if (i == 0) {
            /* In case we are looping more than once, set other
             * TAPs to have the same name */
            VIR_FREE(*ifname);
            if (VIR_STRDUP(*ifname, ifr.ifr_name) < 0)
                goto cleanup;
        }

        if ((flags & VIR_NETDEV_TAP_CREATE_PERSIST) &&
            ioctl(fd, TUNSETPERSIST, 1) < 0) {
            virReportSystemError(errno,
                                 _("Unable to set tap device %s to persistent"),
                                 NULLSTR(*ifname));
            goto cleanup;
        }
        tapfd[i] = fd;
    }

    ret = 0;

 cleanup:
    if (ret < 0) {
        VIR_FORCE_CLOSE(fd);
        while (i--)
            VIR_FORCE_CLOSE(tapfd[i]);
    }

    return ret;
}


int virNetDevTapDelete(const char *ifname,
                       const char *tunpath)
{
    struct ifreq try;
    int fd;
    int ret = -1;

    if (!tunpath)
        tunpath = "/dev/net/tun";

    if ((fd = open(tunpath, O_RDWR)) < 0) {
        virReportSystemError(errno,
                             _("Unable to open %s, is tun module loaded?"),
                             tunpath);
        return -1;
    }

    memset(&try, 0, sizeof(struct ifreq));
    try.ifr_flags = IFF_TAP|IFF_NO_PI;

    if (virStrcpyStatic(try.ifr_name, ifname) == NULL) {
        virReportSystemError(ERANGE,
                             _("Network interface name '%s' is too long"),
                             ifname);
        goto cleanup;
    }

    if (ioctl(fd, TUNSETIFF, &try) < 0) {
        virReportSystemError(errno, "%s",
                             _("Unable to associate TAP device"));
        goto cleanup;
    }

    if (ioctl(fd, TUNSETPERSIST, 0) < 0) {
        virReportSystemError(errno, "%s",
                             _("Unable to make TAP device non-persistent"));
        goto cleanup;
    }

    ret = 0;

 cleanup:
    VIR_FORCE_CLOSE(fd);
    return ret;
}
#elif defined(SIOCIFCREATE2) && defined(SIOCIFDESTROY) && defined(IF_MAXUNIT)
int virNetDevTapCreate(char **ifname,
                       const char *tunpath ATTRIBUTE_UNUSED,
                       int *tapfd,
                       size_t tapfdSize,
                       unsigned int flags ATTRIBUTE_UNUSED)
{
    int s;
    struct ifreq ifr;
    int ret = -1;
    char *newifname = NULL;

    if (tapfdSize > 1) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Multiqueue devices are not supported on this system"));
        goto cleanup;
    }

    /* As FreeBSD determines interface type by name,
     * we have to create 'tap' interface first and
     * then rename it to 'vnet'
     */
    if ((s = virNetDevSetupControl("tap", &ifr)) < 0)
        return -1;

    if (ioctl(s, SIOCIFCREATE2, &ifr) < 0) {
        virReportSystemError(errno, "%s",
                             _("Unable to create tap device"));
        goto cleanup;
    }

    /* In case we were given exact interface name (e.g. 'vnetN'),
     * we just rename to it. If we have format string like
     * 'vnet%d', we need to find the first available name that
     * matches this pattern
     */
    if (strstr(*ifname, "%d") != NULL) {
        size_t i;
        for (i = 0; i <= IF_MAXUNIT; i++) {
            char *newname;
            if (virAsprintf(&newname, *ifname, i) < 0)
                goto cleanup;

            if (virNetDevExists(newname) == 0) {
                newifname = newname;
                break;
            }

            VIR_FREE(newname);
        }
        if (newifname) {
            VIR_FREE(*ifname);
            *ifname = newifname;
        } else {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Failed to generate new name for interface %s"),
                           ifr.ifr_name);
            goto cleanup;
        }
    }

    if (tapfd) {
        char *dev_path = NULL;
        if (virAsprintf(&dev_path, "/dev/%s", ifr.ifr_name) < 0)
            goto cleanup;

        if ((*tapfd = open(dev_path, O_RDWR)) < 0) {
            virReportSystemError(errno,
                                 _("Unable to open %s"),
                                 dev_path);
            VIR_FREE(dev_path);
            goto cleanup;
        }

        VIR_FREE(dev_path);
    }

    if (virNetDevSetName(ifr.ifr_name, *ifname) == -1)
        goto cleanup;


    ret = 0;
 cleanup:
    VIR_FORCE_CLOSE(s);

    return ret;
}

int virNetDevTapDelete(const char *ifname,
                       const char *tunpath ATTRIBUTE_UNUSED)
{
    int s;
    struct ifreq ifr;
    int ret = -1;

    if ((s = virNetDevSetupControl(ifname, &ifr)) < 0)
        return -1;

    if (ioctl(s, SIOCIFDESTROY, &ifr) < 0) {
        virReportSystemError(errno,
                             _("Unable to remove tap device %s"),
                             ifname);
        goto cleanup;
    }

    ret = 0;
 cleanup:
    VIR_FORCE_CLOSE(s);
    return ret;
}

#else
int virNetDevTapCreate(char **ifname ATTRIBUTE_UNUSED,
                       const char *tunpath ATTRIBUTE_UNUSED,
                       int *tapfd ATTRIBUTE_UNUSED,
                       size_t tapfdSize ATTRIBUTE_UNUSED,
                       unsigned int flags ATTRIBUTE_UNUSED)
{
    virReportSystemError(ENOSYS, "%s",
                         _("Unable to create TAP devices on this platform"));
    return -1;
}
int virNetDevTapDelete(const char *ifname ATTRIBUTE_UNUSED,
                       const char *tunpath ATTRIBUTE_UNUSED)
{
    virReportSystemError(ENOSYS, "%s",
                         _("Unable to delete TAP devices on this platform"));
    return -1;
}
#endif


/**
 * virNetDevTapCreateInBridgePort:
 * @brname: the bridge name
 * @ifname: the interface name (or name template)
 * @macaddr: desired MAC address
 * @tunpath: path to the tun device (if NULL, /dev/net/tun is used)
 * @tapfd: array of file descriptor return value for the new tap device
 * @tapfdSize: number of file descriptors in @tapfd
 * @virtPortProfile: bridge/port specific configuration
 * @flags: OR of virNetDevTapCreateFlags:

 *   VIR_NETDEV_TAP_CREATE_IFUP
 *     - Bring the interface up
 *   VIR_NETDEV_TAP_CREATE_VNET_HDR
 *     - Enable IFF_VNET_HDR on the tap device
 *   VIR_NETDEV_TAP_CREATE_USE_MAC_FOR_BRIDGE
 *     - Set this interface's MAC as the bridge's MAC address
 *   VIR_NETDEV_TAP_CREATE_PERSIST
 *     - The device will persist after the file descriptor is closed
 *
 * This function creates a new tap device on a bridge. @ifname can be either
 * a fixed name or a name template with '%d' for dynamic name allocation.
 * in either case the final name for the bridge will be stored in @ifname.
 * If the @tapfd parameter is supplied, the open tap device file descriptor
 * will be returned, otherwise the TAP device will be closed. The caller must
 * use virNetDevTapDelete to remove a persistent TAP device when it is no
 * longer needed.
 *
 * Returns 0 in case of success or -1 on failure
 */
int virNetDevTapCreateInBridgePort(const char *brname,
                                   char **ifname,
                                   const virMacAddr *macaddr,
                                   const unsigned char *vmuuid,
                                   const char *tunpath,
                                   int *tapfd,
                                   size_t tapfdSize,
                                   virNetDevVPortProfilePtr virtPortProfile,
                                   virNetDevVlanPtr virtVlan,
                                   unsigned int flags)
{
    virMacAddr tapmac;
    char macaddrstr[VIR_MAC_STRING_BUFLEN];
    size_t i;

    if (virNetDevTapCreate(ifname, tunpath, tapfd, tapfdSize, flags) < 0)
        return -1;

    /* We need to set the interface MAC before adding it
     * to the bridge, because the bridge assumes the lowest
     * MAC of all enslaved interfaces & we don't want it
     * seeing the kernel allocate random MAC for the TAP
     * device before we set our static MAC.
     */
    virMacAddrSet(&tapmac, macaddr);
    if (!(flags & VIR_NETDEV_TAP_CREATE_USE_MAC_FOR_BRIDGE)) {
        if (macaddr->addr[0] == 0xFE) {
            /* For normal use, the tap device's MAC address cannot
             * match the MAC address used by the guest. This results
             * in "received packet on vnetX with own address as source
             * address" error logs from the kernel.
             */
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Unable to use MAC address starting with "
                             "reserved value 0xFE - '%s' - "),
                           virMacAddrFormat(macaddr, macaddrstr));
            goto error;
        }
        tapmac.addr[0] = 0xFE; /* Discourage bridge from using TAP dev MAC */
    }

    if (virNetDevSetMAC(*ifname, &tapmac) < 0)
        goto error;

    /* We need to set the interface MTU before adding it
     * to the bridge, because the bridge will have its
     * MTU adjusted automatically when we add the new interface.
     */
    if (virNetDevSetMTUFromDevice(*ifname, brname) < 0)
        goto error;

    if (virtPortProfile) {
        if (virNetDevOpenvswitchAddPort(brname, *ifname, macaddr, vmuuid,
                                        virtPortProfile, virtVlan) < 0) {
            goto error;
        }
    } else {
        if (virNetDevBridgeAddPort(brname, *ifname) < 0)
            goto error;
    }

    if (virNetDevSetOnline(*ifname, !!(flags & VIR_NETDEV_TAP_CREATE_IFUP)) < 0)
        goto error;

    return 0;

 error:
    for (i = 0; i < tapfdSize && tapfd[i] >= 0; i++)
        VIR_FORCE_CLOSE(tapfd[i]);

    return -1;
}
