/*
 * Copyright (C) 2010-2014 Red Hat, Inc.
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
 * POSIX DAC security driver
 */

#include <config.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifdef  __FreeBSD__
# include <sys/sysctl.h>
# include <sys/user.h>
#endif

#include "security_dac.h"
#include "virerror.h"
#include "virfile.h"
#include "viralloc.h"
#include "virlog.h"
#include "virpci.h"
#include "virusb.h"
#include "virscsi.h"
#include "virstoragefile.h"
#include "virstring.h"
#include "virutil.h"

#define VIR_FROM_THIS VIR_FROM_SECURITY

VIR_LOG_INIT("security.security_dac");

#define SECURITY_DAC_NAME "dac"

typedef struct _virSecurityDACData virSecurityDACData;
typedef virSecurityDACData *virSecurityDACDataPtr;

struct _virSecurityDACData {
    uid_t user;
    gid_t group;
    gid_t *groups;
    int ngroups;
    bool dynamicOwnership;
    char *baselabel;
    virSecurityManagerDACChownCallback chownCallback;
};

typedef struct _virSecurityDACCallbackData virSecurityDACCallbackData;
typedef virSecurityDACCallbackData *virSecurityDACCallbackDataPtr;

struct _virSecurityDACCallbackData {
    virSecurityManagerPtr manager;
    virSecurityLabelDefPtr secdef;
};

/* returns -1 on error, 0 on success */
int
virSecurityDACSetUserAndGroup(virSecurityManagerPtr mgr,
                              uid_t user,
                              gid_t group)
{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    priv->user = user;
    priv->group = group;

    if (virAsprintf(&priv->baselabel, "+%u:+%u",
                    (unsigned int) user,
                    (unsigned int) group) < 0)
        return -1;

    return 0;
}

void
virSecurityDACSetDynamicOwnership(virSecurityManagerPtr mgr,
                                  bool dynamicOwnership)
{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    priv->dynamicOwnership = dynamicOwnership;
}

void
virSecurityDACSetChownCallback(virSecurityManagerPtr mgr,
                               virSecurityManagerDACChownCallback chownCallback)
{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    priv->chownCallback = chownCallback;
}

/* returns 1 if label isn't found, 0 on success, -1 on error */
static int
ATTRIBUTE_NONNULL(2) ATTRIBUTE_NONNULL(3)
virSecurityDACParseIds(virSecurityLabelDefPtr seclabel,
                       uid_t *uidPtr, gid_t *gidPtr)
{
    if (!seclabel || !seclabel->label)
        return 1;

    if (virParseOwnershipIds(seclabel->label, uidPtr, gidPtr) < 0)
        return -1;

    return 0;
}

static int
ATTRIBUTE_NONNULL(3) ATTRIBUTE_NONNULL(4)
virSecurityDACGetIds(virSecurityLabelDefPtr seclabel,
                     virSecurityDACDataPtr priv,
                     uid_t *uidPtr, gid_t *gidPtr,
                     gid_t **groups, int *ngroups)
{
    int ret;

    if (groups)
        *groups = priv ? priv->groups : NULL;
    if (ngroups)
        *ngroups = priv ? priv->ngroups : 0;

    if ((ret = virSecurityDACParseIds(seclabel, uidPtr, gidPtr)) <= 0)
        return ret;

    if (!priv) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("DAC seclabel couldn't be determined"));
        return -1;
    }

    *uidPtr = priv->user;
    *gidPtr = priv->group;

    return 0;
}


/* returns 1 if label isn't found, 0 on success, -1 on error */
static int
ATTRIBUTE_NONNULL(2) ATTRIBUTE_NONNULL(3)
virSecurityDACParseImageIds(virSecurityLabelDefPtr seclabel,
                            uid_t *uidPtr, gid_t *gidPtr)
{
    if (!seclabel || !seclabel->imagelabel)
        return 1;

    if (virParseOwnershipIds(seclabel->imagelabel, uidPtr, gidPtr) < 0)
        return -1;

    return 0;
}

static int
ATTRIBUTE_NONNULL(3) ATTRIBUTE_NONNULL(4)
virSecurityDACGetImageIds(virSecurityLabelDefPtr seclabel,
                          virSecurityDACDataPtr priv,
                          uid_t *uidPtr, gid_t *gidPtr)
{
    int ret;

    if ((ret = virSecurityDACParseImageIds(seclabel, uidPtr, gidPtr)) <= 0)
        return ret;

    if (!priv) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("DAC imagelabel couldn't be determined"));
        return -1;
    }

    *uidPtr = priv->user;
    *gidPtr = priv->group;

    return 0;
}


static virSecurityDriverStatus
virSecurityDACProbe(const char *virtDriver ATTRIBUTE_UNUSED)
{
    return SECURITY_DRIVER_ENABLE;
}

static int
virSecurityDACOpen(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED)
{
    return 0;
}

static int
virSecurityDACClose(virSecurityManagerPtr mgr)
{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    VIR_FREE(priv->groups);
    VIR_FREE(priv->baselabel);
    return 0;
}


static const char *
virSecurityDACGetModel(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED)
{
    return SECURITY_DAC_NAME;
}

static const char *
virSecurityDACGetDOI(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED)
{
    return "0";
}

static int
virSecurityDACPreFork(virSecurityManagerPtr mgr)
{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    int ngroups;

    VIR_FREE(priv->groups);
    priv->ngroups = 0;
    if ((ngroups = virGetGroupList(priv->user, priv->group,
                                   &priv->groups)) < 0)
        return -1;
    priv->ngroups = ngroups;
    return 0;
}

static int
virSecurityDACSetOwnershipInternal(virSecurityDACDataPtr priv,
                                   virStorageSourcePtr src,
                                   const char *path,
                                   uid_t uid,
                                   gid_t gid)
{
    int rc;
    int chown_errno;

    VIR_INFO("Setting DAC user and group on '%s' to '%ld:%ld'",
             NULLSTR(src ? src->path : path), (long) uid, (long) gid);

    if (priv && src && priv->chownCallback) {
        rc = priv->chownCallback(src, uid, gid);
        /* here path is used only for error messages */
        path = NULLSTR(src->path);

        /* on -2 returned an error was already reported */
        if (rc == -2)
            return -1;

        /* on -1 only errno was set */
        chown_errno = errno;
    } else {
        struct stat sb;

        if (!path) {
            if (!src || !src->path)
                return 0;

            if (!virStorageSourceIsLocalStorage(src))
                return 0;

            path = src->path;
        }

        rc = chown(path, uid, gid);
        chown_errno = errno;

        if (rc < 0 &&
            stat(path, &sb) >= 0) {
            if (sb.st_uid == uid &&
                sb.st_gid == gid) {
                /* It's alright, there's nothing to change anyway. */
                return 0;
            }
        }
    }

    if (rc < 0) {
        if (chown_errno == EOPNOTSUPP || chown_errno == EINVAL) {
            VIR_INFO("Setting user and group to '%ld:%ld' on '%s' not "
                     "supported by filesystem",
                     (long) uid, (long) gid, path);
        } else if (chown_errno == EPERM) {
            VIR_INFO("Setting user and group to '%ld:%ld' on '%s' not "
                     "permitted",
                     (long) uid, (long) gid, path);
        } else if (chown_errno == EROFS) {
            VIR_INFO("Setting user and group to '%ld:%ld' on '%s' not "
                     "possible on readonly filesystem",
                     (long) uid, (long) gid, path);
        } else {
            virReportSystemError(chown_errno,
                                 _("unable to set user and group to '%ld:%ld' "
                                   "on '%s'"),
                                 (long) uid, (long) gid, path);
            return -1;
        }
    }
    return 0;
}


static int
virSecurityDACSetOwnership(const char *path, uid_t uid, gid_t gid)
{
    return virSecurityDACSetOwnershipInternal(NULL, NULL, path, uid, gid);
}


static int
virSecurityDACRestoreSecurityFileLabelInternal(virSecurityDACDataPtr priv,
                                               virStorageSourcePtr src,
                                               const char *path)
{
    VIR_INFO("Restoring DAC user and group on '%s'",
             NULLSTR(src ? src->path : path));

    /* XXX record previous ownership */
    return virSecurityDACSetOwnershipInternal(priv, src, path, 0, 0);
}


static int
virSecurityDACRestoreSecurityFileLabel(const char *path)
{
    return virSecurityDACRestoreSecurityFileLabelInternal(NULL, NULL, path);
}


static int
virSecurityDACSetSecurityImageLabel(virSecurityManagerPtr mgr,
                                    virDomainDefPtr def,
                                    virStorageSourcePtr src)
{
    virSecurityLabelDefPtr secdef;
    virSecurityDeviceLabelDefPtr disk_seclabel;
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    uid_t user;
    gid_t group;

    if (!priv->dynamicOwnership)
        return 0;

    secdef = virDomainDefGetSecurityLabelDef(def, SECURITY_DAC_NAME);
    if (secdef && !secdef->relabel)
        return 0;

    disk_seclabel = virStorageSourceGetSecurityLabelDef(src,
                                                        SECURITY_DAC_NAME);
    if (disk_seclabel && !disk_seclabel->relabel)
        return 0;

    if (disk_seclabel && disk_seclabel->label) {
        if (virParseOwnershipIds(disk_seclabel->label, &user, &group) < 0)
            return -1;
    } else {
        if (virSecurityDACGetImageIds(secdef, priv, &user, &group))
            return -1;
    }

    return virSecurityDACSetOwnershipInternal(priv, src, NULL, user, group);
}


static int
virSecurityDACSetSecurityDiskLabel(virSecurityManagerPtr mgr,
                                   virDomainDefPtr def,
                                   virDomainDiskDefPtr disk)

{
    virStorageSourcePtr next;

    for (next = disk->src; next; next = next->backingStore) {
        if (virSecurityDACSetSecurityImageLabel(mgr, def, next) < 0)
            return -1;
    }

    return 0;
}


static int
virSecurityDACRestoreSecurityImageLabelInt(virSecurityManagerPtr mgr,
                                           virDomainDefPtr def,
                                           virStorageSourcePtr src,
                                           bool migrated)
{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    virSecurityLabelDefPtr secdef;
    virSecurityDeviceLabelDefPtr disk_seclabel;

    if (!priv->dynamicOwnership)
        return 0;

    /* Don't restore labels on readoly/shared disks, because other VMs may
     * still be accessing these. Alternatively we could iterate over all
     * running domains and try to figure out if it is in use, but this would
     * not work for clustered filesystems, since we can't see running VMs using
     * the file on other nodes. Safest bet is thus to skip the restore step. */
    if (src->readonly || src->shared)
        return 0;

    secdef = virDomainDefGetSecurityLabelDef(def, SECURITY_DAC_NAME);
    if (secdef && !secdef->relabel)
        return 0;

    disk_seclabel = virStorageSourceGetSecurityLabelDef(src,
                                                        SECURITY_DAC_NAME);
    if (disk_seclabel && !disk_seclabel->relabel)
        return 0;

    /* If we have a shared FS and are doing migration, we must not change
     * ownership, because that kills access on the destination host which is
     * sub-optimal for the guest VM's I/O attempts :-) */
    if (migrated) {
        int rc = 1;

        if (virStorageSourceIsLocalStorage(src)) {
            if (!src->path)
                return 0;

            if ((rc = virFileIsSharedFS(src->path)) < 0)
                return -1;
        }

        if (rc == 1) {
            VIR_DEBUG("Skipping image label restore on %s because FS is shared",
                      src->path);
            return 0;
        }
    }

    return virSecurityDACRestoreSecurityFileLabelInternal(priv, src, NULL);
}


static int
virSecurityDACRestoreSecurityImageLabel(virSecurityManagerPtr mgr,
                                        virDomainDefPtr def,
                                        virStorageSourcePtr src)
{
    return virSecurityDACRestoreSecurityImageLabelInt(mgr, def, src, false);
}


static int
virSecurityDACRestoreSecurityDiskLabel(virSecurityManagerPtr mgr,
                                       virDomainDefPtr def,
                                       virDomainDiskDefPtr disk)
{
    return virSecurityDACRestoreSecurityImageLabelInt(mgr, def, disk->src, false);
}


static int
virSecurityDACSetSecurityHostdevLabelHelper(const char *file,
                                            void *opaque)
{
    virSecurityDACCallbackDataPtr cbdata = opaque;
    virSecurityManagerPtr mgr = cbdata->manager;
    virSecurityLabelDefPtr secdef = cbdata->secdef;
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    uid_t user;
    gid_t group;

    if (virSecurityDACGetIds(secdef, priv, &user, &group, NULL, NULL))
        return -1;

    return virSecurityDACSetOwnership(file, user, group);
}


static int
virSecurityDACSetSecurityPCILabel(virPCIDevicePtr dev ATTRIBUTE_UNUSED,
                                  const char *file,
                                  void *opaque)
{
    return virSecurityDACSetSecurityHostdevLabelHelper(file, opaque);
}


static int
virSecurityDACSetSecurityUSBLabel(virUSBDevicePtr dev ATTRIBUTE_UNUSED,
                                  const char *file,
                                  void *opaque)
{
    return virSecurityDACSetSecurityHostdevLabelHelper(file, opaque);
}


static int
virSecurityDACSetSecuritySCSILabel(virSCSIDevicePtr dev ATTRIBUTE_UNUSED,
                                   const char *file,
                                   void *opaque)
{
    return virSecurityDACSetSecurityHostdevLabelHelper(file, opaque);
}


static int
virSecurityDACSetSecurityHostdevLabel(virSecurityManagerPtr mgr,
                                      virDomainDefPtr def,
                                      virDomainHostdevDefPtr dev,
                                      const char *vroot)
{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    virSecurityDACCallbackData cbdata;
    virDomainHostdevSubsysUSBPtr usbsrc = &dev->source.subsys.u.usb;
    virDomainHostdevSubsysPCIPtr pcisrc = &dev->source.subsys.u.pci;
    virDomainHostdevSubsysSCSIPtr scsisrc = &dev->source.subsys.u.scsi;
    int ret = -1;

    if (!priv->dynamicOwnership)
        return 0;

    if (dev->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS)
        return 0;

    /* Like virSecurityDACSetSecurityImageLabel() for a networked disk,
     * do nothing for an iSCSI hostdev
     */
    if (dev->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI &&
        scsisrc->protocol == VIR_DOMAIN_HOSTDEV_SCSI_PROTOCOL_TYPE_ISCSI)
        return 0;

    cbdata.manager = mgr;
    cbdata.secdef = virDomainDefGetSecurityLabelDef(def, SECURITY_DAC_NAME);

    if (cbdata.secdef && !cbdata.secdef->relabel)
        return 0;

    switch ((virDomainHostdevSubsysType) dev->source.subsys.type) {
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB: {
        virUSBDevicePtr usb;

        if (dev->missing)
            return 0;

        if (!(usb = virUSBDeviceNew(usbsrc->bus, usbsrc->device, vroot)))
            goto done;

        ret = virUSBDeviceFileIterate(usb,
                                      virSecurityDACSetSecurityUSBLabel,
                                      &cbdata);
        virUSBDeviceFree(usb);
        break;
    }

    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI: {
        virPCIDevicePtr pci =
            virPCIDeviceNew(pcisrc->addr.domain, pcisrc->addr.bus,
                            pcisrc->addr.slot, pcisrc->addr.function);

        if (!pci)
            goto done;

        if (pcisrc->backend == VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO) {
            char *vfioGroupDev = virPCIDeviceGetIOMMUGroupDev(pci);

            if (!vfioGroupDev) {
                virPCIDeviceFree(pci);
                goto done;
            }
            ret = virSecurityDACSetSecurityPCILabel(pci, vfioGroupDev, &cbdata);
            VIR_FREE(vfioGroupDev);
        } else {
            ret = virPCIDeviceFileIterate(pci,
                                          virSecurityDACSetSecurityPCILabel,
                                          &cbdata);
        }

        virPCIDeviceFree(pci);
        break;
    }

    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI: {
        virDomainHostdevSubsysSCSIHostPtr scsihostsrc = &scsisrc->u.host;
        virSCSIDevicePtr scsi =
            virSCSIDeviceNew(NULL,
                             scsihostsrc->adapter, scsihostsrc->bus,
                             scsihostsrc->target, scsihostsrc->unit,
                             dev->readonly, dev->shareable);

        if (!scsi)
            goto done;

        ret = virSCSIDeviceFileIterate(scsi,
                                       virSecurityDACSetSecuritySCSILabel,
                                       &cbdata);
        virSCSIDeviceFree(scsi);

        break;
    }

    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_LAST:
        ret = 0;
        break;
    }

 done:
    return ret;
}


static int
virSecurityDACRestoreSecurityPCILabel(virPCIDevicePtr dev ATTRIBUTE_UNUSED,
                                      const char *file,
                                      void *opaque ATTRIBUTE_UNUSED)
{
    return virSecurityDACRestoreSecurityFileLabel(file);
}


static int
virSecurityDACRestoreSecurityUSBLabel(virUSBDevicePtr dev ATTRIBUTE_UNUSED,
                                      const char *file,
                                      void *opaque ATTRIBUTE_UNUSED)
{
    return virSecurityDACRestoreSecurityFileLabel(file);
}


static int
virSecurityDACRestoreSecuritySCSILabel(virSCSIDevicePtr dev ATTRIBUTE_UNUSED,
                                       const char *file,
                                       void *opaque ATTRIBUTE_UNUSED)
{
    return virSecurityDACRestoreSecurityFileLabel(file);
}


static int
virSecurityDACRestoreSecurityHostdevLabel(virSecurityManagerPtr mgr,
                                          virDomainDefPtr def,
                                          virDomainHostdevDefPtr dev,
                                          const char *vroot)

{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    virSecurityLabelDefPtr secdef;
    virDomainHostdevSubsysUSBPtr usbsrc = &dev->source.subsys.u.usb;
    virDomainHostdevSubsysPCIPtr pcisrc = &dev->source.subsys.u.pci;
    virDomainHostdevSubsysSCSIPtr scsisrc = &dev->source.subsys.u.scsi;
    int ret = -1;

    secdef = virDomainDefGetSecurityLabelDef(def, SECURITY_DAC_NAME);

    if (!priv->dynamicOwnership || (secdef && !secdef->relabel))
        return 0;

    if (dev->mode != VIR_DOMAIN_HOSTDEV_MODE_SUBSYS)
        return 0;

    /* Like virSecurityDACRestoreSecurityImageLabelInt() for a networked disk,
     * do nothing for an iSCSI hostdev
     */
    if (dev->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI &&
        scsisrc->protocol == VIR_DOMAIN_HOSTDEV_SCSI_PROTOCOL_TYPE_ISCSI)
        return 0;

    switch ((virDomainHostdevSubsysType) dev->source.subsys.type) {
    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB: {
        virUSBDevicePtr usb;

        if (dev->missing)
            return 0;

        if (!(usb = virUSBDeviceNew(usbsrc->bus, usbsrc->device, vroot)))
            goto done;

        ret = virUSBDeviceFileIterate(usb, virSecurityDACRestoreSecurityUSBLabel, mgr);
        virUSBDeviceFree(usb);

        break;
    }

    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI: {
        virPCIDevicePtr pci =
            virPCIDeviceNew(pcisrc->addr.domain, pcisrc->addr.bus,
                            pcisrc->addr.slot, pcisrc->addr.function);

        if (!pci)
            goto done;

        if (pcisrc->backend == VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO) {
            char *vfioGroupDev = virPCIDeviceGetIOMMUGroupDev(pci);

            if (!vfioGroupDev) {
                virPCIDeviceFree(pci);
                goto done;
            }
            ret = virSecurityDACRestoreSecurityPCILabel(pci, vfioGroupDev, mgr);
            VIR_FREE(vfioGroupDev);
        } else {
            ret = virPCIDeviceFileIterate(pci, virSecurityDACRestoreSecurityPCILabel, mgr);
        }
        virPCIDeviceFree(pci);
        break;
    }

    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI: {
        virDomainHostdevSubsysSCSIHostPtr scsihostsrc = &scsisrc->u.host;
        virSCSIDevicePtr scsi =
            virSCSIDeviceNew(NULL,
                             scsihostsrc->adapter, scsihostsrc->bus,
                             scsihostsrc->target, scsihostsrc->unit,
                             dev->readonly, dev->shareable);

        if (!scsi)
            goto done;

        ret = virSCSIDeviceFileIterate(scsi, virSecurityDACRestoreSecuritySCSILabel, mgr);
        virSCSIDeviceFree(scsi);

        break;
    }

    case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_LAST:
        ret = 0;
        break;
    }

 done:
    return ret;
}


static int
virSecurityDACSetChardevLabel(virSecurityManagerPtr mgr,
                              virDomainDefPtr def,
                              virDomainChrDefPtr dev,
                              virDomainChrSourceDefPtr dev_source)

{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    virSecurityLabelDefPtr seclabel;
    virSecurityDeviceLabelDefPtr chr_seclabel = NULL;
    char *in = NULL, *out = NULL;
    int ret = -1;
    uid_t user;
    gid_t group;

    seclabel = virDomainDefGetSecurityLabelDef(def, SECURITY_DAC_NAME);

    if (dev)
        chr_seclabel = virDomainChrDefGetSecurityLabelDef(dev,
                                                          SECURITY_DAC_NAME);

    if (chr_seclabel && !chr_seclabel->relabel)
        return 0;

    if (chr_seclabel && chr_seclabel->label) {
        if (virParseOwnershipIds(chr_seclabel->label, &user, &group) < 0)
            return -1;
    } else {
        if (virSecurityDACGetIds(seclabel, priv, &user, &group, NULL, NULL) < 0)
            return -1;
    }

    switch ((virDomainChrType) dev_source->type) {
    case VIR_DOMAIN_CHR_TYPE_DEV:
    case VIR_DOMAIN_CHR_TYPE_FILE:
        ret = virSecurityDACSetOwnership(dev_source->data.file.path,
                                         user, group);
        break;

    case VIR_DOMAIN_CHR_TYPE_PIPE:
        if ((virAsprintf(&in, "%s.in", dev_source->data.file.path) < 0) ||
            (virAsprintf(&out, "%s.out", dev_source->data.file.path) < 0))
            goto done;
        if (virFileExists(in) && virFileExists(out)) {
            if ((virSecurityDACSetOwnership(in, user, group) < 0) ||
                (virSecurityDACSetOwnership(out, user, group) < 0)) {
                goto done;
            }
        } else if (virSecurityDACSetOwnership(dev_source->data.file.path,
                                              user, group) < 0) {
            goto done;
        }
        ret = 0;
        break;

    case VIR_DOMAIN_CHR_TYPE_SPICEPORT:
    case VIR_DOMAIN_CHR_TYPE_NULL:
    case VIR_DOMAIN_CHR_TYPE_VC:
    case VIR_DOMAIN_CHR_TYPE_PTY:
    case VIR_DOMAIN_CHR_TYPE_STDIO:
    case VIR_DOMAIN_CHR_TYPE_UDP:
    case VIR_DOMAIN_CHR_TYPE_TCP:
    case VIR_DOMAIN_CHR_TYPE_UNIX:
    case VIR_DOMAIN_CHR_TYPE_SPICEVMC:
    case VIR_DOMAIN_CHR_TYPE_NMDM:
    case VIR_DOMAIN_CHR_TYPE_LAST:
        ret = 0;
        break;
    }

 done:
    VIR_FREE(in);
    VIR_FREE(out);
    return ret;
}

static int
virSecurityDACRestoreChardevLabel(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                  virDomainDefPtr def ATTRIBUTE_UNUSED,
                                  virDomainChrDefPtr dev,
                                  virDomainChrSourceDefPtr dev_source)
{
    virSecurityDeviceLabelDefPtr chr_seclabel = NULL;
    char *in = NULL, *out = NULL;
    int ret = -1;

    if (dev)
        chr_seclabel = virDomainChrDefGetSecurityLabelDef(dev,
                                                          SECURITY_DAC_NAME);

    if (chr_seclabel && !chr_seclabel->relabel)
        return 0;

    switch ((virDomainChrType) dev_source->type) {
    case VIR_DOMAIN_CHR_TYPE_DEV:
    case VIR_DOMAIN_CHR_TYPE_FILE:
        ret = virSecurityDACRestoreSecurityFileLabel(dev_source->data.file.path);
        break;

    case VIR_DOMAIN_CHR_TYPE_PIPE:
        if ((virAsprintf(&out, "%s.out", dev_source->data.file.path) < 0) ||
            (virAsprintf(&in, "%s.in", dev_source->data.file.path) < 0))
            goto done;
        if (virFileExists(in) && virFileExists(out)) {
            if ((virSecurityDACRestoreSecurityFileLabel(out) < 0) ||
                (virSecurityDACRestoreSecurityFileLabel(in) < 0)) {
                goto done;
            }
        } else if (virSecurityDACRestoreSecurityFileLabel(dev_source->data.file.path) < 0) {
            goto done;
        }
        ret = 0;
        break;

    case VIR_DOMAIN_CHR_TYPE_NULL:
    case VIR_DOMAIN_CHR_TYPE_VC:
    case VIR_DOMAIN_CHR_TYPE_PTY:
    case VIR_DOMAIN_CHR_TYPE_STDIO:
    case VIR_DOMAIN_CHR_TYPE_UDP:
    case VIR_DOMAIN_CHR_TYPE_TCP:
    case VIR_DOMAIN_CHR_TYPE_UNIX:
    case VIR_DOMAIN_CHR_TYPE_SPICEVMC:
    case VIR_DOMAIN_CHR_TYPE_SPICEPORT:
    case VIR_DOMAIN_CHR_TYPE_NMDM:
    case VIR_DOMAIN_CHR_TYPE_LAST:
        ret = 0;
        break;
    }

 done:
    VIR_FREE(in);
    VIR_FREE(out);
    return ret;
}


static int
virSecurityDACRestoreChardevCallback(virDomainDefPtr def,
                                     virDomainChrDefPtr dev,
                                     void *opaque)
{
    virSecurityManagerPtr mgr = opaque;

    return virSecurityDACRestoreChardevLabel(mgr, def, dev, &dev->source);
}


static int
virSecurityDACSetSecurityTPMFileLabel(virSecurityManagerPtr mgr,
                                      virDomainDefPtr def,
                                      virDomainTPMDefPtr tpm)
{
    int ret = 0;

    switch (tpm->type) {
    case VIR_DOMAIN_TPM_TYPE_PASSTHROUGH:
        ret = virSecurityDACSetChardevLabel(mgr, def, NULL,
                                            &tpm->data.passthrough.source);
        break;
    case VIR_DOMAIN_TPM_TYPE_LAST:
        break;
    }

    return ret;
}


static int
virSecurityDACRestoreSecurityTPMFileLabel(virSecurityManagerPtr mgr,
                                          virDomainDefPtr def,
                                          virDomainTPMDefPtr tpm)
{
    int ret = 0;

    switch (tpm->type) {
    case VIR_DOMAIN_TPM_TYPE_PASSTHROUGH:
        ret = virSecurityDACRestoreChardevLabel(mgr, def, NULL,
                                          &tpm->data.passthrough.source);
        break;
    case VIR_DOMAIN_TPM_TYPE_LAST:
        break;
    }

    return ret;
}


static int
virSecurityDACRestoreSecurityAllLabel(virSecurityManagerPtr mgr,
                                      virDomainDefPtr def,
                                      bool migrated)
{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    virSecurityLabelDefPtr secdef;
    size_t i;
    int rc = 0;

    secdef = virDomainDefGetSecurityLabelDef(def, SECURITY_DAC_NAME);

    if (!priv->dynamicOwnership || (secdef && !secdef->relabel))
        return 0;

    VIR_DEBUG("Restoring security label on %s migrated=%d",
              def->name, migrated);

    for (i = 0; i < def->nhostdevs; i++) {
        if (virSecurityDACRestoreSecurityHostdevLabel(mgr,
                                                      def,
                                                      def->hostdevs[i],
                                                      NULL) < 0)
            rc = -1;
    }
    for (i = 0; i < def->ndisks; i++) {
        if (virSecurityDACRestoreSecurityImageLabelInt(mgr,
                                                       def,
                                                       def->disks[i]->src,
                                                       migrated) < 0)
            rc = -1;
    }

    if (virDomainChrDefForeach(def,
                               false,
                               virSecurityDACRestoreChardevCallback,
                               mgr) < 0)
        rc = -1;

    if (def->tpm) {
        if (virSecurityDACRestoreSecurityTPMFileLabel(mgr,
                                                      def,
                                                      def->tpm) < 0)
            rc = -1;
    }

    if (def->os.loader && def->os.loader->nvram &&
        virSecurityDACRestoreSecurityFileLabel(def->os.loader->nvram) < 0)
        rc = -1;

    if (def->os.kernel &&
        virSecurityDACRestoreSecurityFileLabel(def->os.kernel) < 0)
        rc = -1;

    if (def->os.initrd &&
        virSecurityDACRestoreSecurityFileLabel(def->os.initrd) < 0)
        rc = -1;

    if (def->os.dtb &&
        virSecurityDACRestoreSecurityFileLabel(def->os.dtb) < 0)
        rc = -1;

    return rc;
}


static int
virSecurityDACSetChardevCallback(virDomainDefPtr def,
                                 virDomainChrDefPtr dev,
                                 void *opaque)
{
    virSecurityManagerPtr mgr = opaque;

    return virSecurityDACSetChardevLabel(mgr, def, dev, &dev->source);
}


static int
virSecurityDACSetSecurityAllLabel(virSecurityManagerPtr mgr,
                                  virDomainDefPtr def,
                                  const char *stdin_path ATTRIBUTE_UNUSED)
{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    virSecurityLabelDefPtr secdef;
    size_t i;
    uid_t user;
    gid_t group;

    secdef = virDomainDefGetSecurityLabelDef(def, SECURITY_DAC_NAME);

    if (!priv->dynamicOwnership || (secdef && !secdef->relabel))
        return 0;

    for (i = 0; i < def->ndisks; i++) {
        /* XXX fixme - we need to recursively label the entire tree :-( */
        if (virDomainDiskGetType(def->disks[i]) == VIR_STORAGE_TYPE_DIR)
            continue;
        if (virSecurityDACSetSecurityDiskLabel(mgr,
                                               def,
                                               def->disks[i]) < 0)
            return -1;
    }
    for (i = 0; i < def->nhostdevs; i++) {
        if (virSecurityDACSetSecurityHostdevLabel(mgr,
                                                  def,
                                                  def->hostdevs[i],
                                                  NULL) < 0)
            return -1;
    }

    if (virDomainChrDefForeach(def,
                               true,
                               virSecurityDACSetChardevCallback,
                               mgr) < 0)
        return -1;

    if (def->tpm) {
        if (virSecurityDACSetSecurityTPMFileLabel(mgr,
                                                  def,
                                                  def->tpm) < 0)
            return -1;
    }

    if (virSecurityDACGetImageIds(secdef, priv, &user, &group))
        return -1;

    if (def->os.loader && def->os.loader->nvram &&
        virSecurityDACSetOwnership(def->os.loader->nvram, user, group) < 0)
        return -1;

    if (def->os.kernel &&
        virSecurityDACSetOwnership(def->os.kernel, user, group) < 0)
        return -1;

    if (def->os.initrd &&
        virSecurityDACSetOwnership(def->os.initrd, user, group) < 0)
        return -1;

    if (def->os.dtb &&
        virSecurityDACSetOwnership(def->os.dtb, user, group) < 0)
        return -1;

    return 0;
}


static int
virSecurityDACSetSavedStateLabel(virSecurityManagerPtr mgr,
                                 virDomainDefPtr def,
                                 const char *savefile)
{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    virSecurityLabelDefPtr secdef;
    uid_t user;
    gid_t group;

    secdef = virDomainDefGetSecurityLabelDef(def, SECURITY_DAC_NAME);

    if (virSecurityDACGetImageIds(secdef, priv, &user, &group) < 0)
        return -1;

    return virSecurityDACSetOwnership(savefile, user, group);
}


static int
virSecurityDACRestoreSavedStateLabel(virSecurityManagerPtr mgr,
                                     virDomainDefPtr def ATTRIBUTE_UNUSED,
                                     const char *savefile)
{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);

    if (!priv->dynamicOwnership)
        return 0;

    return virSecurityDACRestoreSecurityFileLabel(savefile);
}


static int
virSecurityDACSetProcessLabel(virSecurityManagerPtr mgr,
                              virDomainDefPtr def)
{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    virSecurityLabelDefPtr secdef;
    uid_t user;
    gid_t group;
    gid_t *groups;
    int ngroups;

    secdef = virDomainDefGetSecurityLabelDef(def, SECURITY_DAC_NAME);

    if (virSecurityDACGetIds(secdef, priv, &user, &group, &groups, &ngroups) < 0)
        return -1;

    VIR_DEBUG("Dropping privileges of DEF to %u:%u, %d supplemental groups",
              (unsigned int) user, (unsigned int) group, ngroups);

    if (virSetUIDGID(user, group, groups, ngroups) < 0)
        return -1;

    return 0;
}


static int
virSecurityDACSetChildProcessLabel(virSecurityManagerPtr mgr,
                                   virDomainDefPtr def,
                                   virCommandPtr cmd)
{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    virSecurityLabelDefPtr secdef;
    uid_t user;
    gid_t group;

    secdef = virDomainDefGetSecurityLabelDef(def, SECURITY_DAC_NAME);

    if (virSecurityDACGetIds(secdef, priv, &user, &group, NULL, NULL))
        return -1;

    VIR_DEBUG("Setting child to drop privileges of DEF to %u:%u",
              (unsigned int) user, (unsigned int) group);

    virCommandSetUID(cmd, user);
    virCommandSetGID(cmd, group);
    return 0;
}


static int
virSecurityDACVerify(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                     virDomainDefPtr def ATTRIBUTE_UNUSED)
{
    return 0;
}

static int
virSecurityDACGenLabel(virSecurityManagerPtr mgr,
                       virDomainDefPtr def)
{
    int rc = -1;
    virSecurityLabelDefPtr seclabel;
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);

    seclabel = virDomainDefGetSecurityLabelDef(def, SECURITY_DAC_NAME);
    if (seclabel == NULL)
        return rc;

    if (seclabel->imagelabel) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("security image label already "
                         "defined for VM"));
        return rc;
    }

    if (seclabel->model
        && STRNEQ(seclabel->model, SECURITY_DAC_NAME)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("security label model %s is not supported "
                         "with selinux"),
                       seclabel->model);
            return rc;
    }

    switch ((virDomainSeclabelType) seclabel->type) {
    case VIR_DOMAIN_SECLABEL_STATIC:
        if (seclabel->label == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("missing label for static security "
                             "driver in domain %s"), def->name);
            return rc;
        }
        break;
    case VIR_DOMAIN_SECLABEL_DYNAMIC:
        if (virAsprintf(&seclabel->label, "+%u:+%u",
                        (unsigned int) priv->user,
                        (unsigned int) priv->group) < 0)
            return rc;
        if (seclabel->label == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot generate dac user and group id "
                             "for domain %s"), def->name);
            return rc;
        }
        break;
    case VIR_DOMAIN_SECLABEL_NONE:
        /* no op */
        return 0;
    case VIR_DOMAIN_SECLABEL_DEFAULT:
    case VIR_DOMAIN_SECLABEL_LAST:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected security label type '%s'"),
                       virDomainSeclabelTypeToString(seclabel->type));
        return rc;
    }

    if (seclabel->relabel && !seclabel->imagelabel &&
        VIR_STRDUP(seclabel->imagelabel, seclabel->label) < 0) {
        VIR_FREE(seclabel->label);
        return rc;
    }

    return 0;
}

static int
virSecurityDACReleaseLabel(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                           virDomainDefPtr def ATTRIBUTE_UNUSED)
{
    return 0;
}

static int
virSecurityDACReserveLabel(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                           virDomainDefPtr def ATTRIBUTE_UNUSED,
                           pid_t pid ATTRIBUTE_UNUSED)
{
    return 0;
}

#ifdef __linux__
static int
virSecurityDACGetProcessLabelInternal(pid_t pid,
                                      virSecurityLabelPtr seclabel)
{
    struct stat sb;
    char *path = NULL;
    int ret = -1;

    VIR_DEBUG("Getting DAC user and group on process '%d'", pid);

    if (virAsprintf(&path, "/proc/%d", (int) pid) < 0)
        goto cleanup;

    if (lstat(path, &sb) < 0) {
        virReportSystemError(errno,
                             _("unable to get uid and gid for PID %d via procfs"),
                             pid);
        goto cleanup;
    }

    snprintf(seclabel->label, VIR_SECURITY_LABEL_BUFLEN,
             "+%u:+%u", (unsigned int) sb.st_uid, (unsigned int) sb.st_gid);
    ret = 0;

 cleanup:
    VIR_FREE(path);
    return ret;
}
#elif defined(__FreeBSD__)
static int
virSecurityDACGetProcessLabelInternal(pid_t pid,
                                      virSecurityLabelPtr seclabel)
{
    struct kinfo_proc p;
    int mib[4];
    size_t len = 4;

    sysctlnametomib("kern.proc.pid", mib, &len);

    len = sizeof(struct kinfo_proc);
    mib[3] = pid;

    if (sysctl(mib, 4, &p, &len, NULL, 0) < 0) {
        virReportSystemError(errno,
                             _("unable to get PID %d uid and gid via sysctl"),
                             pid);
        return -1;
    }

    snprintf(seclabel->label, VIR_SECURITY_LABEL_BUFLEN,
             "+%u:+%u", (unsigned int) p.ki_uid, (unsigned int) p.ki_groups[0]);

    return 0;
}
#else
static int
virSecurityDACGetProcessLabelInternal(pid_t pid ATTRIBUTE_UNUSED,
                                      virSecurityLabelPtr seclabel ATTRIBUTE_UNUSED)
{
    virReportSystemError(ENOSYS, "%s",
                         _("Cannot get process uid and gid on this platform"));
    return -1;
}
#endif

static int
virSecurityDACGetProcessLabel(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                              virDomainDefPtr def,
                              pid_t pid,
                              virSecurityLabelPtr seclabel)
{
    virSecurityLabelDefPtr secdef =
        virDomainDefGetSecurityLabelDef(def, SECURITY_DAC_NAME);

    if (secdef == NULL) {
        VIR_DEBUG("missing label for DAC security "
                  "driver in domain %s", def->name);

        if (virSecurityDACGetProcessLabelInternal(pid, seclabel) < 0)
            return -1;
        return 0;
    }

    if (secdef->label)
        ignore_value(virStrcpy(seclabel->label, secdef->label,
                               VIR_SECURITY_LABEL_BUFLEN));

    return 0;
}

static int
virSecurityDACSetDaemonSocketLabel(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                   virDomainDefPtr vm ATTRIBUTE_UNUSED)
{
    return 0;
}


static int
virSecurityDACSetSocketLabel(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                             virDomainDefPtr def ATTRIBUTE_UNUSED)
{
    return 0;
}


static int
virSecurityDACClearSocketLabel(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                               virDomainDefPtr def ATTRIBUTE_UNUSED)
{
    return 0;
}

static int
virSecurityDACSetImageFDLabel(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                              virDomainDefPtr def ATTRIBUTE_UNUSED,
                              int fd ATTRIBUTE_UNUSED)
{
    return 0;
}

static int
virSecurityDACSetTapFDLabel(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                            virDomainDefPtr def ATTRIBUTE_UNUSED,
                            int fd ATTRIBUTE_UNUSED)
{
    return 0;
}

static char *
virSecurityDACGetMountOptions(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                              virDomainDefPtr vm ATTRIBUTE_UNUSED)
{
    return NULL;
}

static const char *
virSecurityDACGetBaseLabel(virSecurityManagerPtr mgr,
                           int virt ATTRIBUTE_UNUSED)
{
    virSecurityDACDataPtr priv = virSecurityManagerGetPrivateData(mgr);
    return priv->baselabel;
}

virSecurityDriver virSecurityDriverDAC = {
    .privateDataLen                     = sizeof(virSecurityDACData),
    .name                               = SECURITY_DAC_NAME,
    .probe                              = virSecurityDACProbe,
    .open                               = virSecurityDACOpen,
    .close                              = virSecurityDACClose,

    .getModel                           = virSecurityDACGetModel,
    .getDOI                             = virSecurityDACGetDOI,

    .preFork                            = virSecurityDACPreFork,

    .domainSecurityVerify               = virSecurityDACVerify,

    .domainSetSecurityDiskLabel         = virSecurityDACSetSecurityDiskLabel,
    .domainRestoreSecurityDiskLabel     = virSecurityDACRestoreSecurityDiskLabel,

    .domainSetSecurityImageLabel        = virSecurityDACSetSecurityImageLabel,
    .domainRestoreSecurityImageLabel    = virSecurityDACRestoreSecurityImageLabel,

    .domainSetSecurityDaemonSocketLabel = virSecurityDACSetDaemonSocketLabel,
    .domainSetSecuritySocketLabel       = virSecurityDACSetSocketLabel,
    .domainClearSecuritySocketLabel     = virSecurityDACClearSocketLabel,

    .domainGenSecurityLabel             = virSecurityDACGenLabel,
    .domainReserveSecurityLabel         = virSecurityDACReserveLabel,
    .domainReleaseSecurityLabel         = virSecurityDACReleaseLabel,

    .domainGetSecurityProcessLabel      = virSecurityDACGetProcessLabel,
    .domainSetSecurityProcessLabel      = virSecurityDACSetProcessLabel,
    .domainSetSecurityChildProcessLabel = virSecurityDACSetChildProcessLabel,

    .domainSetSecurityAllLabel          = virSecurityDACSetSecurityAllLabel,
    .domainRestoreSecurityAllLabel      = virSecurityDACRestoreSecurityAllLabel,

    .domainSetSecurityHostdevLabel      = virSecurityDACSetSecurityHostdevLabel,
    .domainRestoreSecurityHostdevLabel  = virSecurityDACRestoreSecurityHostdevLabel,

    .domainSetSavedStateLabel           = virSecurityDACSetSavedStateLabel,
    .domainRestoreSavedStateLabel       = virSecurityDACRestoreSavedStateLabel,

    .domainSetSecurityImageFDLabel      = virSecurityDACSetImageFDLabel,
    .domainSetSecurityTapFDLabel        = virSecurityDACSetTapFDLabel,

    .domainGetSecurityMountOptions      = virSecurityDACGetMountOptions,

    .getBaseLabel                       = virSecurityDACGetBaseLabel,
};
