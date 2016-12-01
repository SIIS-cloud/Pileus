/*
 * qemu_cgroup.c: QEMU cgroup management
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

#include "qemu_cgroup.h"
#include "qemu_domain.h"
#include "qemu_process.h"
#include "vircgroup.h"
#include "virlog.h"
#include "viralloc.h"
#include "virerror.h"
#include "domain_audit.h"
#include "virscsi.h"
#include "virstring.h"
#include "virfile.h"
#include "virtypedparam.h"
#include "virnuma.h"

#define VIR_FROM_THIS VIR_FROM_QEMU

VIR_LOG_INIT("qemu.qemu_cgroup");

static const char *const defaultDeviceACL[] = {
    "/dev/null", "/dev/full", "/dev/zero",
    "/dev/random", "/dev/urandom",
    "/dev/ptmx", "/dev/kvm", "/dev/kqemu",
    "/dev/rtc", "/dev/hpet", "/dev/vfio/vfio",
    NULL,
};
#define DEVICE_PTY_MAJOR 136
#define DEVICE_SND_MAJOR 116

static int
qemuSetImageCgroupInternal(virDomainObjPtr vm,
                           virStorageSourcePtr src,
                           bool deny,
                           bool forceReadonly)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int perms = VIR_CGROUP_DEVICE_READ;
    int ret;

    if (!virCgroupHasController(priv->cgroup,
                                VIR_CGROUP_CONTROLLER_DEVICES))
        return 0;

    if (!src->path || !virStorageSourceIsLocalStorage(src)) {
        VIR_DEBUG("Not updating cgroups for disk path '%s', type: %s",
                  NULLSTR(src->path), virStorageTypeToString(src->type));
        return 0;
    }

    if (deny) {
        perms |= VIR_CGROUP_DEVICE_WRITE | VIR_CGROUP_DEVICE_MKNOD;

        VIR_DEBUG("Deny path %s", src->path);

        ret = virCgroupDenyDevicePath(priv->cgroup, src->path, perms);
    } else {
        if (!src->readonly && !forceReadonly)
            perms |= VIR_CGROUP_DEVICE_WRITE;

        VIR_DEBUG("Allow path %s, perms: %s",
                  src->path, virCgroupGetDevicePermsString(perms));

        ret = virCgroupAllowDevicePath(priv->cgroup, src->path, perms);
    }

    virDomainAuditCgroupPath(vm, priv->cgroup,
                             deny ? "deny" : "allow",
                             src->path,
                             virCgroupGetDevicePermsString(perms),
                             ret == 0);

    /* Get this for root squash NFS */
    if (ret < 0 &&
        virLastErrorIsSystemErrno(EACCES)) {
        VIR_DEBUG("Ignoring EACCES for %s", src->path);
        virResetLastError();
        ret = 0;
    }

    return ret;
}


int
qemuSetImageCgroup(virDomainObjPtr vm,
                   virStorageSourcePtr src,
                   bool deny)
{
    return qemuSetImageCgroupInternal(vm, src, deny, false);
}


int
qemuSetupDiskCgroup(virDomainObjPtr vm,
                    virDomainDiskDefPtr disk)
{
    virStorageSourcePtr next;
    bool forceReadonly = false;

    for (next = disk->src; next; next = next->backingStore) {
        if (qemuSetImageCgroupInternal(vm, next, false, forceReadonly) < 0)
            return -1;

        /* setup only the top level image for read-write */
        forceReadonly = true;
    }

    return 0;
}


int
qemuTeardownDiskCgroup(virDomainObjPtr vm,
                       virDomainDiskDefPtr disk)
{
    virStorageSourcePtr next;

    for (next = disk->src; next; next = next->backingStore) {
        if (qemuSetImageCgroup(vm, next, true) < 0)
            return -1;
    }

    return 0;
}


static int
qemuSetupChrSourceCgroup(virDomainDefPtr def ATTRIBUTE_UNUSED,
                         virDomainChrSourceDefPtr dev,
                         void *opaque)
{
    virDomainObjPtr vm = opaque;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int ret;

    if (dev->type != VIR_DOMAIN_CHR_TYPE_DEV)
        return 0;

    VIR_DEBUG("Process path '%s' for device", dev->data.file.path);

    ret = virCgroupAllowDevicePath(priv->cgroup, dev->data.file.path,
                                   VIR_CGROUP_DEVICE_RW);
    virDomainAuditCgroupPath(vm, priv->cgroup, "allow",
                             dev->data.file.path, "rw", ret == 0);

    return ret;
}

static int
qemuSetupChardevCgroup(virDomainDefPtr def,
                       virDomainChrDefPtr dev,
                       void *opaque)
{
    return qemuSetupChrSourceCgroup(def, &dev->source, opaque);
}


static int
qemuSetupTPMCgroup(virDomainDefPtr def,
                   virDomainTPMDefPtr dev,
                   void *opaque)
{
    int ret = 0;

    switch (dev->type) {
    case VIR_DOMAIN_TPM_TYPE_PASSTHROUGH:
        ret = qemuSetupChrSourceCgroup(def, &dev->data.passthrough.source,
                                       opaque);
        break;
    case VIR_DOMAIN_TPM_TYPE_LAST:
        break;
    }

    return ret;
}


static int
qemuSetupHostUSBDeviceCgroup(virUSBDevicePtr dev ATTRIBUTE_UNUSED,
                             const char *path,
                             void *opaque)
{
    virDomainObjPtr vm = opaque;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int ret;

    VIR_DEBUG("Process path '%s' for USB device", path);
    ret = virCgroupAllowDevicePath(priv->cgroup, path,
                                   VIR_CGROUP_DEVICE_RW);
    virDomainAuditCgroupPath(vm, priv->cgroup, "allow", path, "rw", ret == 0);

    return ret;
}

static int
qemuSetupHostSCSIDeviceCgroup(virSCSIDevicePtr dev ATTRIBUTE_UNUSED,
                              const char *path,
                              void *opaque)
{
    virDomainObjPtr vm = opaque;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int ret;

    VIR_DEBUG("Process path '%s' for SCSI device", path);

    ret = virCgroupAllowDevicePath(priv->cgroup, path,
                                   virSCSIDeviceGetReadonly(dev) ?
                                   VIR_CGROUP_DEVICE_READ :
                                   VIR_CGROUP_DEVICE_RW);

    virDomainAuditCgroupPath(vm, priv->cgroup, "allow", path,
                             virSCSIDeviceGetReadonly(dev) ? "r" : "rw", ret == 0);

    return ret;
}

int
qemuSetupHostdevCGroup(virDomainObjPtr vm,
                       virDomainHostdevDefPtr dev)
{
    int ret = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainHostdevSubsysUSBPtr usbsrc = &dev->source.subsys.u.usb;
    virDomainHostdevSubsysPCIPtr pcisrc = &dev->source.subsys.u.pci;
    virDomainHostdevSubsysSCSIPtr scsisrc = &dev->source.subsys.u.scsi;
    virPCIDevicePtr pci = NULL;
    virUSBDevicePtr usb = NULL;
    virSCSIDevicePtr scsi = NULL;
    char *path = NULL;

    /* currently this only does something for PCI devices using vfio
     * for device assignment, but it is called for *all* hostdev
     * devices.
     */

    if (!virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_DEVICES))
        return 0;

    if (dev->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS) {

        switch (dev->source.subsys.type) {
        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI:
            if (pcisrc->backend == VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO) {
                int rv;

                pci = virPCIDeviceNew(pcisrc->addr.domain,
                                      pcisrc->addr.bus,
                                      pcisrc->addr.slot,
                                      pcisrc->addr.function);
                if (!pci)
                    goto cleanup;

                if (!(path = virPCIDeviceGetIOMMUGroupDev(pci)))
                    goto cleanup;

                VIR_DEBUG("Cgroup allow %s for PCI device assignment", path);
                rv = virCgroupAllowDevicePath(priv->cgroup, path,
                                              VIR_CGROUP_DEVICE_RW);
                virDomainAuditCgroupPath(vm, priv->cgroup,
                                         "allow", path, "rw", rv == 0);
                if (rv < 0)
                    goto cleanup;
            }
            break;

        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB:
            /* NB: hostdev->missing wasn't previously checked in the
             * case of hotplug, only when starting a domain. Now it is
             * always checked, and the cgroup setup skipped if true.
             */
            if (dev->missing)
                break;
            if ((usb = virUSBDeviceNew(usbsrc->bus, usbsrc->device,
                                       NULL)) == NULL) {
                goto cleanup;
            }

            /* oddly, qemuSetupHostUSBDeviceCgroup doesn't ever
             * reference the usb object we just created
             */
            if (virUSBDeviceFileIterate(usb, qemuSetupHostUSBDeviceCgroup,
                                        vm) < 0) {
                goto cleanup;
            }
            break;

        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI: {
            if (scsisrc->protocol ==
                VIR_DOMAIN_HOSTDEV_SCSI_PROTOCOL_TYPE_ISCSI) {
                virDomainHostdevSubsysSCSIiSCSIPtr iscsisrc = &scsisrc->u.iscsi;
                /* Follow qemuSetupDiskCgroup() and qemuSetImageCgroupInternal()
                 * which does nothing for non local storage
                 */
                VIR_DEBUG("Not updating cgroups for hostdev iSCSI path '%s'",
                          iscsisrc->path);
            } else {
                virDomainHostdevSubsysSCSIHostPtr scsihostsrc =
                    &scsisrc->u.host;
                if ((scsi = virSCSIDeviceNew(NULL,
                                             scsihostsrc->adapter,
                                             scsihostsrc->bus,
                                             scsihostsrc->target,
                                             scsihostsrc->unit,
                                             dev->readonly,
                                             dev->shareable)) == NULL)
                    goto cleanup;

                if (virSCSIDeviceFileIterate(scsi,
                                             qemuSetupHostSCSIDeviceCgroup,
                                             vm) < 0)
                    goto cleanup;
            }
            break;
        }

        default:
            break;
        }
    }

    ret = 0;
 cleanup:
    virPCIDeviceFree(pci);
    virUSBDeviceFree(usb);
    virSCSIDeviceFree(scsi);
    VIR_FREE(path);
    return ret;
}

int
qemuTeardownHostdevCgroup(virDomainObjPtr vm,
                       virDomainHostdevDefPtr dev)
{
    int ret = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainHostdevSubsysPCIPtr pcisrc = &dev->source.subsys.u.pci;
    virPCIDevicePtr pci = NULL;
    char *path = NULL;

    /* currently this only does something for PCI devices using vfio
     * for device assignment, but it is called for *all* hostdev
     * devices.
     */

    if (!virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_DEVICES))
        return 0;

    if (dev->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS) {

        switch (dev->source.subsys.type) {
        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI:
            if (pcisrc->backend == VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO) {
                int rv;

                pci = virPCIDeviceNew(pcisrc->addr.domain,
                                      pcisrc->addr.bus,
                                      pcisrc->addr.slot,
                                      pcisrc->addr.function);
                if (!pci)
                    goto cleanup;

                if (!(path = virPCIDeviceGetIOMMUGroupDev(pci)))
                    goto cleanup;

                VIR_DEBUG("Cgroup deny %s for PCI device assignment", path);
                rv = virCgroupDenyDevicePath(priv->cgroup, path,
                                             VIR_CGROUP_DEVICE_RWM);
                virDomainAuditCgroupPath(vm, priv->cgroup,
                                         "deny", path, "rwm", rv == 0);
                if (rv < 0)
                    goto cleanup;
            }
            break;
        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB:
            /* nothing to tear down for USB */
            break;
        default:
            break;
        }
    }

    ret = 0;
 cleanup:
    virPCIDeviceFree(pci);
    VIR_FREE(path);
    return ret;
}

static int
qemuSetupBlkioCgroup(virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    size_t i;

    if (!virCgroupHasController(priv->cgroup,
                                VIR_CGROUP_CONTROLLER_BLKIO)) {
        if (vm->def->blkio.weight || vm->def->blkio.ndevices) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Block I/O tuning is not available on this host"));
            return -1;
        } else {
            return 0;
        }
    }

    if (vm->def->blkio.weight != 0 &&
        virCgroupSetBlkioWeight(priv->cgroup, vm->def->blkio.weight) < 0)
        return -1;

    if (vm->def->blkio.ndevices) {
        for (i = 0; i < vm->def->blkio.ndevices; i++) {
            virBlkioDevicePtr dev = &vm->def->blkio.devices[i];
            if (dev->weight &&
                (virCgroupSetBlkioDeviceWeight(priv->cgroup, dev->path,
                                               dev->weight) < 0))
                return -1;

            if (dev->riops &&
                (virCgroupSetBlkioDeviceReadIops(priv->cgroup, dev->path,
                                                 dev->riops) < 0))
                return -1;

            if (dev->wiops &&
                (virCgroupSetBlkioDeviceWriteIops(priv->cgroup, dev->path,
                                                  dev->wiops) < 0))
                return -1;

            if (dev->rbps &&
                (virCgroupSetBlkioDeviceReadBps(priv->cgroup, dev->path,
                                                dev->rbps) < 0))
                return -1;

            if (dev->wbps &&
                (virCgroupSetBlkioDeviceWriteBps(priv->cgroup, dev->path,
                                                 dev->wbps) < 0))
                return -1;
        }
    }

    return 0;
}


static int
qemuSetupMemoryCgroup(virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (!virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_MEMORY)) {
        if (vm->def->mem.hard_limit != 0 ||
            vm->def->mem.soft_limit != 0 ||
            vm->def->mem.swap_hard_limit != 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Memory cgroup is not available on this host"));
            return -1;
        } else {
            return 0;
        }
    }

    if (vm->def->mem.hard_limit != 0 &&
        virCgroupSetMemoryHardLimit(priv->cgroup, vm->def->mem.hard_limit) < 0)
        return -1;

    if (vm->def->mem.soft_limit != 0 &&
        virCgroupSetMemorySoftLimit(priv->cgroup, vm->def->mem.soft_limit) < 0)
        return -1;

    if (vm->def->mem.swap_hard_limit != 0 &&
        virCgroupSetMemSwapHardLimit(priv->cgroup, vm->def->mem.swap_hard_limit) < 0)
        return -1;

    return 0;
}


static int
qemuSetupDevicesCgroup(virQEMUDriverPtr driver,
                       virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virQEMUDriverConfigPtr cfg = NULL;
    const char *const *deviceACL = NULL;
    int rv = -1;
    int ret = -1;
    size_t i;

    if (!virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_DEVICES))
        return 0;

    rv = virCgroupDenyAllDevices(priv->cgroup);
    virDomainAuditCgroup(vm, priv->cgroup, "deny", "all", rv == 0);
    if (rv < 0) {
        if (virLastErrorIsSystemErrno(EPERM)) {
            virResetLastError();
            VIR_WARN("Group devices ACL is not accessible, disabling whitelisting");
            return 0;
        }

        goto cleanup;
    }

    for (i = 0; i < vm->def->ndisks; i++) {
        if (qemuSetupDiskCgroup(vm, vm->def->disks[i]) < 0)
            goto cleanup;
    }

    rv = virCgroupAllowDeviceMajor(priv->cgroup, 'c', DEVICE_PTY_MAJOR,
                                   VIR_CGROUP_DEVICE_RW);
    virDomainAuditCgroupMajor(vm, priv->cgroup, "allow", DEVICE_PTY_MAJOR,
                              "pty", "rw", rv == 0);
    if (rv < 0)
        goto cleanup;

    cfg = virQEMUDriverGetConfig(driver);
    deviceACL = cfg->cgroupDeviceACL ?
                (const char *const *)cfg->cgroupDeviceACL :
                defaultDeviceACL;

    if (vm->def->nsounds &&
        ((!vm->def->ngraphics && cfg->nogfxAllowHostAudio) ||
         (vm->def->graphics &&
          ((vm->def->graphics[0]->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC &&
           cfg->vncAllowHostAudio) ||
           (vm->def->graphics[0]->type == VIR_DOMAIN_GRAPHICS_TYPE_SDL))))) {
        rv = virCgroupAllowDeviceMajor(priv->cgroup, 'c', DEVICE_SND_MAJOR,
                                       VIR_CGROUP_DEVICE_RW);
        virDomainAuditCgroupMajor(vm, priv->cgroup, "allow", DEVICE_SND_MAJOR,
                                  "sound", "rw", rv == 0);
        if (rv < 0)
            goto cleanup;
    }

    for (i = 0; deviceACL[i] != NULL; i++) {
        if (!virFileExists(deviceACL[i])) {
            VIR_DEBUG("Ignoring non-existent device %s", deviceACL[i]);
            continue;
        }

        rv = virCgroupAllowDevicePath(priv->cgroup, deviceACL[i],
                                      VIR_CGROUP_DEVICE_RW);
        virDomainAuditCgroupPath(vm, priv->cgroup, "allow", deviceACL[i], "rw", rv == 0);
        if (rv < 0 &&
            !virLastErrorIsSystemErrno(ENOENT))
            goto cleanup;
    }

    if (virDomainChrDefForeach(vm->def,
                               true,
                               qemuSetupChardevCgroup,
                               vm) < 0)
        goto cleanup;

    if (vm->def->tpm &&
        (qemuSetupTPMCgroup(vm->def,
                            vm->def->tpm,
                            vm) < 0))
        goto cleanup;

    for (i = 0; i < vm->def->nhostdevs; i++) {
        if (qemuSetupHostdevCGroup(vm, vm->def->hostdevs[i]) < 0)
            goto cleanup;
    }

    for (i = 0; i < vm->def->nrngs; i++) {
        if (vm->def->rngs[i]->backend == VIR_DOMAIN_RNG_BACKEND_RANDOM) {
            VIR_DEBUG("Setting Cgroup ACL for RNG device");
            rv = virCgroupAllowDevicePath(priv->cgroup,
                                          vm->def->rngs[i]->source.file,
                                          VIR_CGROUP_DEVICE_RW);
            virDomainAuditCgroupPath(vm, priv->cgroup, "allow",
                                     vm->def->rngs[i]->source.file,
                                     "rw", rv == 0);
            if (rv < 0 &&
                !virLastErrorIsSystemErrno(ENOENT))
                goto cleanup;
        }
    }

    ret = 0;
 cleanup:
    virObjectUnref(cfg);
    return ret;
}


int
qemuSetupCpusetMems(virDomainObjPtr vm)
{
    virCgroupPtr cgroup_temp = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    char *mem_mask = NULL;
    int ret = -1;

    if (!virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET))
        return 0;

    if (virDomainNumatuneGetMode(vm->def->numatune, -1) !=
        VIR_DOMAIN_NUMATUNE_MEM_STRICT)
        return 0;

    if (virDomainNumatuneMaybeFormatNodeset(vm->def->numatune,
                                            priv->autoNodeset,
                                            &mem_mask, -1) < 0)
        goto cleanup;

    if (mem_mask)
        if (virCgroupNewEmulator(priv->cgroup, false, &cgroup_temp) < 0 ||
            virCgroupSetCpusetMems(cgroup_temp, mem_mask) < 0)
            goto cleanup;

    ret = 0;
 cleanup:
    VIR_FREE(mem_mask);
    virCgroupFree(&cgroup_temp);
    return ret;
}


static int
qemuSetupCpusetCgroup(virDomainObjPtr vm,
                      virCapsPtr caps)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    char *cpu_mask = NULL;
    int ret = -1;

    if (!virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET))
        return 0;

    if (vm->def->cpumask ||
        (vm->def->placement_mode == VIR_DOMAIN_CPU_PLACEMENT_MODE_AUTO)) {

        if (vm->def->placement_mode == VIR_DOMAIN_CPU_PLACEMENT_MODE_AUTO) {
            virBitmapPtr cpumap;
            if (!(cpumap = virCapabilitiesGetCpusForNodemask(caps, priv->autoNodeset)))
                goto cleanup;
            cpu_mask = virBitmapFormat(cpumap);
            virBitmapFree(cpumap);
        } else {
            cpu_mask = virBitmapFormat(vm->def->cpumask);
        }

        if (!cpu_mask)
            goto cleanup;

        if (virCgroupSetCpusetCpus(priv->cgroup, cpu_mask) < 0)
            goto cleanup;
    }

    ret = 0;
 cleanup:
    VIR_FREE(cpu_mask);
    return ret;
}


static int
qemuSetupCpuCgroup(virQEMUDriverPtr driver,
                   virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virObjectEventPtr event = NULL;
    virTypedParameterPtr eventParams = NULL;
    int eventNparams = 0;
    int eventMaxparams = 0;

    if (!virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPU)) {
       if (vm->def->cputune.sharesSpecified) {
           virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                          _("CPU tuning is not available on this host"));
           return -1;
       } else {
           return 0;
       }
    }

    if (vm->def->cputune.sharesSpecified) {
        unsigned long long val;
        if (virCgroupSetCpuShares(priv->cgroup, vm->def->cputune.shares) < 0)
            return -1;

        if (virCgroupGetCpuShares(priv->cgroup, &val) < 0)
            return -1;
        if (vm->def->cputune.shares != val) {
            vm->def->cputune.shares = val;
            if (virTypedParamsAddULLong(&eventParams, &eventNparams,
                                        &eventMaxparams,
                                        VIR_DOMAIN_TUNABLE_CPU_CPU_SHARES,
                                        val) < 0)
                return -1;

            event = virDomainEventTunableNewFromObj(vm, eventParams, eventNparams);
        }

        if (event)
            qemuDomainEventQueue(driver, event);
    }

    return 0;
}


static int
qemuInitCgroup(virQEMUDriverPtr driver,
               virDomainObjPtr vm)
{
    int ret = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    if (!cfg->privileged)
        goto done;

    if (!virCgroupAvailable())
        goto done;

    virCgroupFree(&priv->cgroup);

    if (!vm->def->resource) {
        virDomainResourceDefPtr res;

        if (VIR_ALLOC(res) < 0)
            goto cleanup;

        if (VIR_STRDUP(res->partition, "/machine") < 0) {
            VIR_FREE(res);
            goto cleanup;
        }

        vm->def->resource = res;
    }

    if (vm->def->resource->partition[0] != '/') {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Resource partition '%s' must start with '/'"),
                       vm->def->resource->partition);
        goto cleanup;
    }

    if (virCgroupNewMachine(vm->def->name,
                            "qemu",
                            cfg->privileged,
                            vm->def->uuid,
                            NULL,
                            vm->pid,
                            false,
                            0, NULL,
                            vm->def->resource->partition,
                            cfg->cgroupControllers,
                            &priv->cgroup) < 0) {
        if (virCgroupNewIgnoreError())
            goto done;

        goto cleanup;
    }

 done:
    ret = 0;
 cleanup:
    virObjectUnref(cfg);
    return ret;
}

static void
qemuRestoreCgroupState(virDomainObjPtr vm)
{
    char *mem_mask;
    int empty = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virBitmapPtr all_nodes;

    if (!(all_nodes = virNumaGetHostNodeset()))
        goto error;

    if (!(mem_mask = virBitmapFormat(all_nodes)))
        goto error;

    if ((empty = virCgroupHasEmptyTasks(priv->cgroup,
                                        VIR_CGROUP_CONTROLLER_CPUSET)) <= 0)
        goto error;

    if (virCgroupSetCpusetMems(priv->cgroup, mem_mask) < 0)
        goto error;

 cleanup:
    VIR_FREE(mem_mask);
    virBitmapFree(all_nodes);
    return;

 error:
    virResetLastError();
    VIR_DEBUG("Couldn't restore cgroups to meaningful state");
    goto cleanup;
}

int
qemuConnectCgroup(virQEMUDriverPtr driver,
                  virDomainObjPtr vm)
{
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int ret = -1;

    if (!cfg->privileged)
        goto done;

    if (!virCgroupAvailable())
        goto done;

    virCgroupFree(&priv->cgroup);

    if (virCgroupNewDetectMachine(vm->def->name,
                                  "qemu",
                                  vm->pid,
                                  vm->def->resource ?
                                  vm->def->resource->partition :
                                  NULL,
                                  cfg->cgroupControllers,
                                  &priv->cgroup) < 0)
        goto cleanup;

    qemuRestoreCgroupState(vm);

 done:
    ret = 0;
 cleanup:
    virObjectUnref(cfg);
    return ret;
}

int
qemuSetupCgroup(virQEMUDriverPtr driver,
                virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virCapsPtr caps = NULL;
    int ret = -1;

    if (!vm->pid) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot setup cgroups until process is started"));
        return -1;
    }

    if (qemuInitCgroup(driver, vm) < 0)
        return -1;

    if (!priv->cgroup)
        return 0;

    if (!(caps = virQEMUDriverGetCapabilities(driver, false)))
        goto cleanup;

    if (qemuSetupDevicesCgroup(driver, vm) < 0)
        goto cleanup;

    if (qemuSetupBlkioCgroup(vm) < 0)
        goto cleanup;

    if (qemuSetupMemoryCgroup(vm) < 0)
        goto cleanup;

    if (qemuSetupCpuCgroup(driver, vm) < 0)
        goto cleanup;

    if (qemuSetupCpusetCgroup(vm, caps) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virObjectUnref(caps);
    return ret;
}

int
qemuSetupCgroupVcpuBW(virCgroupPtr cgroup,
                      unsigned long long period,
                      long long quota)
{
    unsigned long long old_period;

    if (period == 0 && quota == 0)
        return 0;

    if (period) {
        /* get old period, and we can rollback if set quota failed */
        if (virCgroupGetCpuCfsPeriod(cgroup, &old_period) < 0)
            return -1;

        if (virCgroupSetCpuCfsPeriod(cgroup, period) < 0)
            return -1;
    }

    if (quota &&
        virCgroupSetCpuCfsQuota(cgroup, quota) < 0)
        goto error;

    return 0;

 error:
    if (period) {
        virErrorPtr saved = virSaveLastError();
        ignore_value(virCgroupSetCpuCfsPeriod(cgroup, old_period));
        if (saved) {
            virSetError(saved);
            virFreeError(saved);
        }
    }

    return -1;
}

int
qemuSetupCgroupVcpuPin(virCgroupPtr cgroup,
                       virDomainVcpuPinDefPtr *vcpupin,
                       int nvcpupin,
                       int vcpuid)
{
    size_t i;

    for (i = 0; i < nvcpupin; i++) {
        if (vcpuid == vcpupin[i]->vcpuid)
            return qemuSetupCgroupEmulatorPin(cgroup, vcpupin[i]->cpumask);
    }

    return -1;
}

int
qemuSetupCgroupIOThreadsPin(virCgroupPtr cgroup,
                            virDomainVcpuPinDefPtr *iothreadspin,
                            int niothreadspin,
                            int iothreadid)
{
    size_t i;

    for (i = 0; i < niothreadspin; i++) {
        if (iothreadid == iothreadspin[i]->vcpuid)
            return qemuSetupCgroupEmulatorPin(cgroup, iothreadspin[i]->cpumask);
    }

    return -1;
}

int
qemuSetupCgroupEmulatorPin(virCgroupPtr cgroup,
                           virBitmapPtr cpumask)
{
    int ret = -1;
    char *new_cpus = NULL;

    if (!(new_cpus = virBitmapFormat(cpumask)))
        goto cleanup;

    if (virCgroupSetCpusetCpus(cgroup, new_cpus) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    VIR_FREE(new_cpus);
    return ret;
}

int
qemuSetupCgroupForVcpu(virDomainObjPtr vm)
{
    virCgroupPtr cgroup_vcpu = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDefPtr def = vm->def;
    size_t i, j;
    unsigned long long period = vm->def->cputune.period;
    long long quota = vm->def->cputune.quota;
    char *mem_mask = NULL;

    if ((period || quota) &&
        !virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPU)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("cgroup cpu is required for scheduler tuning"));
        return -1;
    }

    /*
     * If CPU cgroup controller is not initialized here, then we need
     * neither period nor quota settings.  And if CPUSET controller is
     * not initialized either, then there's nothing to do anyway.
     */
    if (!virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPU) &&
        !virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET))
        return 0;

    /* We are trying to setup cgroups for CPU pinning, which can also be done
     * with virProcessSetAffinity, thus the lack of cgroups is not fatal here.
     */
    if (priv->cgroup == NULL)
        return 0;

    if (priv->nvcpupids == 0 || priv->vcpupids[0] == vm->pid) {
        /* If we don't know VCPU<->PID mapping or all vcpu runs in the same
         * thread, we cannot control each vcpu.
         */
        VIR_WARN("Unable to get vcpus' pids.");
        return 0;
    }

    if (virDomainNumatuneGetMode(vm->def->numatune, -1) ==
        VIR_DOMAIN_NUMATUNE_MEM_STRICT &&
        virDomainNumatuneMaybeFormatNodeset(vm->def->numatune,
                                            priv->autoNodeset,
                                            &mem_mask, -1) < 0)
        goto cleanup;

    for (i = 0; i < priv->nvcpupids; i++) {
        if (virCgroupNewVcpu(priv->cgroup, i, true, &cgroup_vcpu) < 0)
            goto cleanup;

        /* move the thread for vcpu to sub dir */
        if (virCgroupAddTask(cgroup_vcpu, priv->vcpupids[i]) < 0)
            goto cleanup;

        if (mem_mask &&
            virCgroupSetCpusetMems(cgroup_vcpu, mem_mask) < 0)
            goto cleanup;

        if (period || quota) {
            if (qemuSetupCgroupVcpuBW(cgroup_vcpu, period, quota) < 0)
                goto cleanup;
        }

        /* Set vcpupin in cgroup if vcpupin xml is provided */
        if (virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET)) {
            /* find the right CPU to pin, otherwise
             * qemuSetupCgroupVcpuPin will fail. */
            for (j = 0; j < def->cputune.nvcpupin; j++) {
                if (def->cputune.vcpupin[j]->vcpuid != i)
                    continue;

                if (qemuSetupCgroupVcpuPin(cgroup_vcpu,
                                           def->cputune.vcpupin,
                                           def->cputune.nvcpupin,
                                           i) < 0)
                    goto cleanup;

                break;
            }
        }

        virCgroupFree(&cgroup_vcpu);
    }
    VIR_FREE(mem_mask);

    return 0;

 cleanup:
    if (cgroup_vcpu) {
        virCgroupRemove(cgroup_vcpu);
        virCgroupFree(&cgroup_vcpu);
    }
    VIR_FREE(mem_mask);

    return -1;
}

int
qemuSetupCgroupForEmulator(virQEMUDriverPtr driver,
                           virDomainObjPtr vm)
{
    virBitmapPtr cpumask = NULL;
    virBitmapPtr cpumap = NULL;
    virCgroupPtr cgroup_emulator = NULL;
    virDomainDefPtr def = vm->def;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    unsigned long long period = vm->def->cputune.emulator_period;
    long long quota = vm->def->cputune.emulator_quota;

    if ((period || quota) &&
        !virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPU)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("cgroup cpu is required for scheduler tuning"));
        return -1;
    }

    /*
     * If CPU cgroup controller is not initialized here, then we need
     * neither period nor quota settings.  And if CPUSET controller is
     * not initialized either, then there's nothing to do anyway.
     */
    if (!virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPU) &&
        !virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET))
        return 0;

    if (priv->cgroup == NULL)
        return 0; /* Not supported, so claim success */

    if (virCgroupNewEmulator(priv->cgroup, true, &cgroup_emulator) < 0)
        goto cleanup;

    if (virCgroupMoveTask(priv->cgroup, cgroup_emulator) < 0)
        goto cleanup;

    if (def->placement_mode == VIR_DOMAIN_CPU_PLACEMENT_MODE_AUTO) {
        if (!(cpumap = qemuPrepareCpumap(driver, priv->autoNodeset)))
            goto cleanup;
        cpumask = cpumap;
    } else if (def->cputune.emulatorpin) {
        cpumask = def->cputune.emulatorpin->cpumask;
    } else if (def->cpumask) {
        cpumask = def->cpumask;
    }

    if (cpumask) {
        if (virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET) &&
            qemuSetupCgroupEmulatorPin(cgroup_emulator, cpumask) < 0)
            goto cleanup;
    }

    if (period || quota) {
        if (virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPU) &&
            qemuSetupCgroupVcpuBW(cgroup_emulator, period,
                                  quota) < 0)
            goto cleanup;
    }

    virCgroupFree(&cgroup_emulator);
    virBitmapFree(cpumap);
    return 0;

 cleanup:
    virBitmapFree(cpumap);

    if (cgroup_emulator) {
        virCgroupRemove(cgroup_emulator);
        virCgroupFree(&cgroup_emulator);
    }

    return -1;
}

int
qemuSetupCgroupForIOThreads(virDomainObjPtr vm)
{
    virCgroupPtr cgroup_iothread = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDefPtr def = vm->def;
    size_t i, j;
    unsigned long long period = vm->def->cputune.period;
    long long quota = vm->def->cputune.quota;
    char *mem_mask = NULL;

    if ((period || quota) &&
        !virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPU)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("cgroup cpu is required for scheduler tuning"));
        return -1;
    }

    /*
     * If CPU cgroup controller is not initialized here, then we need
     * neither period nor quota settings.  And if CPUSET controller is
     * not initialized either, then there's nothing to do anyway.
     */
    if (!virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPU) &&
        !virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET))
        return 0;

    /* We are trying to setup cgroups for CPU pinning, which can also be done
     * with virProcessSetAffinity, thus the lack of cgroups is not fatal here.
     */
    if (priv->cgroup == NULL)
        return 0;

    if (def->iothreads && priv->niothreadpids == 0) {
        VIR_WARN("Unable to get iothreads' pids.");
        return 0;
    }

    if (virDomainNumatuneGetMode(vm->def->numatune, -1) ==
        VIR_DOMAIN_NUMATUNE_MEM_STRICT &&
        virDomainNumatuneMaybeFormatNodeset(vm->def->numatune,
                                            priv->autoNodeset,
                                            &mem_mask, -1) < 0)
        goto cleanup;

    for (i = 0; i < priv->niothreadpids; i++) {
        /* IOThreads are numbered 1..n, although the array is 0..n-1,
         * so we will account for that here
         */
        if (virCgroupNewIOThread(priv->cgroup, i + 1, true,
                                 &cgroup_iothread) < 0)
            goto cleanup;

        /* move the thread for iothread to sub dir */
        if (virCgroupAddTask(cgroup_iothread, priv->iothreadpids[i]) < 0)
            goto cleanup;

        if (period || quota) {
            if (qemuSetupCgroupVcpuBW(cgroup_iothread, period, quota) < 0)
                goto cleanup;
        }

        if (mem_mask &&
            virCgroupSetCpusetMems(cgroup_iothread, mem_mask) < 0)
            goto cleanup;

        /* Set iothreadpin in cgroup if iothreadpin xml is provided */
        if (virCgroupHasController(priv->cgroup,
                                   VIR_CGROUP_CONTROLLER_CPUSET)) {
            /* find the right CPU to pin, otherwise
             * qemuSetupCgroupIOThreadsPin will fail. */
            for (j = 0; j < def->cputune.niothreadspin; j++) {
                /* IOThreads are numbered/named 1..n */
                if (def->cputune.iothreadspin[j]->vcpuid != i + 1)
                    continue;

                if (qemuSetupCgroupIOThreadsPin(cgroup_iothread,
                                                def->cputune.iothreadspin,
                                                def->cputune.niothreadspin,
                                                i + 1) < 0)
                    goto cleanup;

                break;
            }
        }

        virCgroupFree(&cgroup_iothread);
    }
    VIR_FREE(mem_mask);

    return 0;

 cleanup:
    if (cgroup_iothread) {
        virCgroupRemove(cgroup_iothread);
        virCgroupFree(&cgroup_iothread);
    }
    VIR_FREE(mem_mask);

    return -1;
}

int
qemuRemoveCgroup(virQEMUDriverPtr driver,
                 virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    if (priv->cgroup == NULL)
        return 0; /* Not supported, so claim success */

    if (virCgroupTerminateMachine(vm->def->name,
                                  "qemu",
                                  cfg->privileged) < 0) {
        if (!virCgroupNewIgnoreError())
            VIR_DEBUG("Failed to terminate cgroup for %s", vm->def->name);
    }

    virObjectUnref(cfg);

    return virCgroupRemove(priv->cgroup);
}

int
qemuAddToCgroup(virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (priv->cgroup == NULL)
        return 0; /* Not supported, so claim success */

    return 0;
}
