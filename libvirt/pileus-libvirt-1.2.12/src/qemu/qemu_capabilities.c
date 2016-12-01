/*
 * qemu_capabilities.c: QEMU capabilities generation
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

#include "qemu_capabilities.h"
#include "viralloc.h"
#include "vircrypto.h"
#include "virlog.h"
#include "virerror.h"
#include "virfile.h"
#include "virpidfile.h"
#include "virprocess.h"
#include "nodeinfo.h"
#include "cpu/cpu.h"
#include "domain_conf.h"
#include "vircommand.h"
#include "virbitmap.h"
#include "virnodesuspend.h"
#include "virnuma.h"
#include "qemu_monitor.h"
#include "virstring.h"
#include "qemu_hostdev.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdarg.h>

#define VIR_FROM_THIS VIR_FROM_QEMU

VIR_LOG_INIT("qemu.qemu_capabilities");

/* While not public, these strings must not change. They
 * are used in domain status files which are read on
 * daemon restarts
 */
VIR_ENUM_IMPL(virQEMUCaps, QEMU_CAPS_LAST,
              "kqemu",  /* 0 */
              "vnc-colon",
              "no-reboot",
              "drive",
              "drive-boot",

              "name", /* 5 */
              "uuid",
              "domid",
              "vnet-hdr",
              "migrate-kvm-stdio",

              "migrate-qemu-tcp", /* 10 */
              "migrate-qemu-exec",
              "drive-cache-v2",
              "kvm",
              "drive-format",

              "vga", /* 15 */
              "0.10",
              "pci-device",
              "mem-path",
              "drive-serial",

              "xen-domid", /* 20 */
              "migrate-qemu-unix",
              "chardev",
              "enable-kvm",
              "monitor-json",

              "balloon", /* 25 */
              "device",
              "sdl",
              "smp-topology",
              "netdev",

              "rtc", /* 30 */
              "vhost-net",
              "rtc-td-hack",
              "no-hpet",
              "no-kvm-pit",

              "tdf", /* 35 */
              "pci-configfd",
              "nodefconfig",
              "boot-menu",
              "enable-kqemu",

              "fsdev", /* 40 */
              "nesting",
              "name-process",
              "drive-readonly",
              "smbios-type",

              "vga-qxl", /* 45 */
              "spice",
              "vga-none",
              "migrate-qemu-fd",
              "boot-index",

              "hda-duplex", /* 50 */
              "drive-aio",
              "pci-multibus",
              "pci-bootindex",
              "ccid-emulated",

              "ccid-passthru", /* 55 */
              "chardev-spicevmc",
              "device-spicevmc",
              "virtio-tx-alg",
              "device-qxl-vga",

              "pci-multifunction", /* 60 */
              "virtio-blk-pci.ioeventfd",
              "sga",
              "virtio-blk-pci.event_idx",
              "virtio-net-pci.event_idx",

              "cache-directsync", /* 65 */
              "piix3-usb-uhci",
              "piix4-usb-uhci",
              "usb-ehci",
              "ich9-usb-ehci1",

              "vt82c686b-usb-uhci", /* 70 */
              "pci-ohci",
              "usb-redir",
              "usb-hub",
              "no-shutdown",

              "cache-unsafe", /* 75 */
              "rombar",
              "ich9-ahci",
              "no-acpi",
              "fsdev-readonly",

              "virtio-blk-pci.scsi", /* 80 */
              "blk-sg-io",
              "drive-copy-on-read",
              "cpu-host",
              "fsdev-writeout",

              "drive-iotune", /* 85 */
              "system_wakeup",
              "scsi-disk.channel",
              "scsi-block",
              "transaction",

              "block-job-sync", /* 90 */
              "block-job-async",
              "scsi-cd",
              "ide-cd",
              "no-user-config",

              "hda-micro", /* 95 */
              "dump-guest-memory",
              "nec-usb-xhci",
              "virtio-s390",
              "balloon-event",

              "bridge", /* 100 */
              "lsi",
              "virtio-scsi-pci",
              "blockio",
              "disable-s3",

              "disable-s4", /* 105 */
              "usb-redir.filter",
              "ide-drive.wwn",
              "scsi-disk.wwn",
              "seccomp-sandbox",

              "reboot-timeout", /* 110 */
              "dump-guest-core",
              "seamless-migration",
              "block-commit",
              "vnc",

              "drive-mirror", /* 115 */
              "usb-redir.bootindex",
              "usb-host.bootindex",
              "blockdev-snapshot-sync",
              "qxl",

              "VGA", /* 120 */
              "cirrus-vga",
              "vmware-svga",
              "device-video-primary",
              "s390-sclp",

              "usb-serial", /* 125 */
              "usb-net",
              "add-fd",
              "nbd-server",
              "virtio-rng",

              "rng-random", /* 130 */
              "rng-egd",
              "virtio-ccw",
              "dtb",
              "megasas",

              "ipv6-migration", /* 135 */
              "machine-opt",
              "machine-usb-opt",
              "tpm-passthrough",
              "tpm-tis",

              "nvram",  /* 140 */
              "pci-bridge",
              "vfio-pci",
              "vfio-pci.bootindex",
              "scsi-generic",

              "scsi-generic.bootindex", /* 145 */
              "mem-merge",
              "vnc-websocket",
              "drive-discard",
              "mlock",

              "vnc-share-policy", /* 150 */
              "device-del-event",
              "dmi-to-pci-bridge",
              "i440fx-pci-hole64-size",
              "q35-pci-hole64-size",

              "usb-storage", /* 155 */
              "usb-storage.removable",
              "virtio-mmio",
              "ich9-intel-hda",
              "kvm-pit-lost-tick-policy",

              "boot-strict", /* 160 */
              "pvpanic",
              "enable-fips",
              "spice-file-xfer-disable",
              "spiceport",

              "usb-kbd", /* 165 */
              "host-pci-multidomain",
              "msg-timestamp",
              "active-commit",
              "change-backing-file",

              "memory-backend-ram", /* 170 */
              "numa",
              "memory-backend-file",
              "usb-audio",
              "rtc-reset-reinjection",

              "splash-timeout", /* 175 */
              "iothread",
              "migrate-rdma",
              "ivshmem",
              "drive-iotune-max",

              "VGA.vgamem_mb", /* 180 */
              "vmware-svga.vgamem_mb",
              "qxl.vgamem_mb",
              "qxl-vga.vgamem_mb",
    );


/*
 * Update the XML parser/formatter when adding more
 * information to this struct so that it gets cached
 * correctly. It does not have to be ABI-stable, as
 * the cache will be discarded & repopulated if the
 * timestamp on the libvirtd binary changes.
 */
struct _virQEMUCaps {
    virObject object;

    bool usedQMP;

    char *binary;
    time_t ctime;

    virBitmapPtr flags;

    unsigned int version;
    unsigned int kvmVersion;

    virArch arch;

    size_t ncpuDefinitions;
    char **cpuDefinitions;

    size_t nmachineTypes;
    char **machineTypes;
    char **machineAliases;
    unsigned int *machineMaxCpus;
};

struct _virQEMUCapsCache {
    virMutex lock;
    virHashTablePtr binaries;
    char *libDir;
    char *cacheDir;
    uid_t runUid;
    gid_t runGid;
};

struct virQEMUCapsSearchData {
    virArch arch;
};


static virClassPtr virQEMUCapsClass;
static void virQEMUCapsDispose(void *obj);

static int virQEMUCapsOnceInit(void)
{
    if (!(virQEMUCapsClass = virClassNew(virClassForObject(),
                                         "virQEMUCaps",
                                         sizeof(virQEMUCaps),
                                         virQEMUCapsDispose)))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(virQEMUCaps)

static virArch virQEMUCapsArchFromString(const char *arch)
{
    if (STREQ(arch, "i386"))
        return VIR_ARCH_I686;
    if (STREQ(arch, "arm"))
        return VIR_ARCH_ARMV7L;

    return virArchFromString(arch);
}


static const char *virQEMUCapsArchToString(virArch arch)
{
    if (arch == VIR_ARCH_I686)
        return "i386";
    else if (arch == VIR_ARCH_ARMV7L)
        return "arm";

    return virArchToString(arch);
}


static virCommandPtr
virQEMUCapsProbeCommand(const char *qemu,
                        virQEMUCapsPtr qemuCaps,
                        uid_t runUid, gid_t runGid)
{
    virCommandPtr cmd = virCommandNew(qemu);

    if (qemuCaps) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NO_USER_CONFIG))
            virCommandAddArg(cmd, "-no-user-config");
        else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NODEFCONFIG))
            virCommandAddArg(cmd, "-nodefconfig");
    }

    virCommandAddEnvPassCommon(cmd);
    virCommandClearCaps(cmd);
    virCommandSetGID(cmd, runGid);
    virCommandSetUID(cmd, runUid);

    return cmd;
}


static void
virQEMUCapsSetDefaultMachine(virQEMUCapsPtr qemuCaps,
                             size_t defIdx)
{
    char *name = qemuCaps->machineTypes[defIdx];
    char *alias = qemuCaps->machineAliases[defIdx];
    unsigned int maxCpus = qemuCaps->machineMaxCpus[defIdx];

    memmove(qemuCaps->machineTypes + 1,
            qemuCaps->machineTypes,
            sizeof(qemuCaps->machineTypes[0]) * defIdx);
    memmove(qemuCaps->machineAliases + 1,
            qemuCaps->machineAliases,
            sizeof(qemuCaps->machineAliases[0]) * defIdx);
    memmove(qemuCaps->machineMaxCpus + 1,
            qemuCaps->machineMaxCpus,
            sizeof(qemuCaps->machineMaxCpus[0]) * defIdx);
    qemuCaps->machineTypes[0] = name;
    qemuCaps->machineAliases[0] = alias;
    qemuCaps->machineMaxCpus[0] = maxCpus;
}

/* Format is:
 * <machine> <desc> [(default)|(alias of <canonical>)]
 */
static int
virQEMUCapsParseMachineTypesStr(const char *output,
                                virQEMUCapsPtr qemuCaps)
{
    const char *p = output;
    const char *next;
    size_t defIdx = 0;

    do {
        const char *t;
        char *name;
        char *canonical = NULL;

        if ((next = strchr(p, '\n')))
            ++next;

        if (STRPREFIX(p, "Supported machines are:"))
            continue;

        if (!(t = strchr(p, ' ')) || (next && t >= next))
            continue;

        if (VIR_STRNDUP(name, p, t - p) < 0)
            return -1;

        p = t;
        if ((t = strstr(p, "(default)")) && (!next || t < next))
            defIdx = qemuCaps->nmachineTypes;

        if ((t = strstr(p, "(alias of ")) && (!next || t < next)) {
            p = t + strlen("(alias of ");
            if (!(t = strchr(p, ')')) || (next && t >= next)) {
                VIR_FREE(name);
                continue;
            }

            if (VIR_STRNDUP(canonical, p, t - p) < 0) {
                VIR_FREE(name);
                return -1;
            }
        }

        if (VIR_REALLOC_N(qemuCaps->machineTypes, qemuCaps->nmachineTypes + 1) < 0 ||
            VIR_REALLOC_N(qemuCaps->machineAliases, qemuCaps->nmachineTypes + 1) < 0 ||
            VIR_REALLOC_N(qemuCaps->machineMaxCpus, qemuCaps->nmachineTypes + 1) < 0) {
            VIR_FREE(name);
            VIR_FREE(canonical);
            return -1;
        }
        qemuCaps->nmachineTypes++;
        if (canonical) {
            qemuCaps->machineTypes[qemuCaps->nmachineTypes-1] = canonical;
            qemuCaps->machineAliases[qemuCaps->nmachineTypes-1] = name;
        } else {
            qemuCaps->machineTypes[qemuCaps->nmachineTypes-1] = name;
            qemuCaps->machineAliases[qemuCaps->nmachineTypes-1] = NULL;
        }
        /* When parsing from command line we don't have information about maxCpus */
        qemuCaps->machineMaxCpus[qemuCaps->nmachineTypes-1] = 0;
    } while ((p = next));


    if (defIdx)
        virQEMUCapsSetDefaultMachine(qemuCaps, defIdx);

    return 0;
}

static int
virQEMUCapsProbeMachineTypes(virQEMUCapsPtr qemuCaps,
                             uid_t runUid, gid_t runGid)
{
    char *output;
    int ret = -1;
    virCommandPtr cmd;
    int status;

    /* Make sure the binary we are about to try exec'ing exists.
     * Technically we could catch the exec() failure, but that's
     * in a sub-process so it's hard to feed back a useful error.
     */
    if (!virFileIsExecutable(qemuCaps->binary)) {
        virReportSystemError(errno, _("Cannot find QEMU binary %s"),
                             qemuCaps->binary);
        return -1;
    }

    cmd = virQEMUCapsProbeCommand(qemuCaps->binary, qemuCaps, runUid, runGid);
    virCommandAddArgList(cmd, "-M", "?", NULL);
    virCommandSetOutputBuffer(cmd, &output);

    /* Ignore failure from older qemu that did not understand '-M ?'.  */
    if (virCommandRun(cmd, &status) < 0)
        goto cleanup;

    if (virQEMUCapsParseMachineTypesStr(output, qemuCaps) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_FREE(output);
    virCommandFree(cmd);

    return ret;
}


typedef int
(*virQEMUCapsParseCPUModels)(const char *output,
                             virQEMUCapsPtr qemuCaps);

/* Format:
 *      <arch> <model>
 * qemu-0.13 encloses some model names in []:
 *      <arch> [<model>]
 */
static int
virQEMUCapsParseX86Models(const char *output,
                          virQEMUCapsPtr qemuCaps)
{
    const char *p = output;
    const char *next;
    int ret = -1;

    do {
        const char *t;
        size_t len;

        if ((next = strchr(p, '\n')))
            next++;

        if (!(t = strchr(p, ' ')) || (next && t >= next))
            continue;

        if (!STRPREFIX(p, "x86"))
            continue;

        p = t;
        while (*p == ' ')
            p++;

        if (*p == '\0' || *p == '\n')
            continue;

        if (VIR_EXPAND_N(qemuCaps->cpuDefinitions, qemuCaps->ncpuDefinitions, 1) < 0)
            goto cleanup;

        if (next)
            len = next - p - 1;
        else
            len = strlen(p);

        if (len > 2 && *p == '[' && p[len - 1] == ']') {
            p++;
            len -= 2;
        }

        if (VIR_STRNDUP(qemuCaps->cpuDefinitions[qemuCaps->ncpuDefinitions - 1], p, len) < 0)
            goto cleanup;
    } while ((p = next));

    ret = 0;

 cleanup:
    return ret;
}

/* ppc64 parser.
 * Format : PowerPC <machine> <description>
 */
static int
virQEMUCapsParsePPCModels(const char *output,
                          virQEMUCapsPtr qemuCaps)
{
    const char *p = output;
    const char *next;
    int ret = -1;

    do {
        const char *t;
        size_t len;

        if ((next = strchr(p, '\n')))
            next++;

        if (!STRPREFIX(p, "PowerPC "))
            continue;

        /* Skip the preceding sub-string "PowerPC " */
        p += 8;

        /*Malformed string, does not obey the format 'PowerPC <model> <desc>'*/
        if (!(t = strchr(p, ' ')) || (next && t >= next))
            continue;

        if (*p == '\0')
            break;

        if (*p == '\n')
            continue;

        if (VIR_EXPAND_N(qemuCaps->cpuDefinitions, qemuCaps->ncpuDefinitions, 1) < 0)
            goto cleanup;

        len = t - p - 1;

        if (VIR_STRNDUP(qemuCaps->cpuDefinitions[qemuCaps->ncpuDefinitions - 1], p, len) < 0)
            goto cleanup;
    } while ((p = next));

    ret = 0;

 cleanup:
    return ret;
}

static int
virQEMUCapsProbeCPUModels(virQEMUCapsPtr qemuCaps, uid_t runUid, gid_t runGid)
{
    char *output = NULL;
    int ret = -1;
    virQEMUCapsParseCPUModels parse;
    virCommandPtr cmd;

    if (qemuCaps->arch == VIR_ARCH_I686 ||
        qemuCaps->arch == VIR_ARCH_X86_64) {
        parse = virQEMUCapsParseX86Models;
    } else if ARCH_IS_PPC64(qemuCaps->arch) {
        parse = virQEMUCapsParsePPCModels;
    } else {
        VIR_DEBUG("don't know how to parse %s CPU models",
                  virArchToString(qemuCaps->arch));
        return 0;
    }

    cmd = virQEMUCapsProbeCommand(qemuCaps->binary, qemuCaps, runUid, runGid);
    virCommandAddArgList(cmd, "-cpu", "?", NULL);
    virCommandSetOutputBuffer(cmd, &output);

    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    if (parse(output, qemuCaps) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_FREE(output);
    virCommandFree(cmd);

    return ret;
}


static char *
virQEMUCapsFindBinaryForArch(virArch hostarch,
                             virArch guestarch)
{
    char *ret;
    const char *archstr = virQEMUCapsArchToString(guestarch);
    char *binary;

    if (virAsprintf(&binary, "qemu-system-%s", archstr) < 0)
        return NULL;

    ret = virFindFileInPath(binary);
    VIR_FREE(binary);
    if (ret && !virFileIsExecutable(ret))
        VIR_FREE(ret);

    if (guestarch == VIR_ARCH_I686 &&
        !ret &&
        hostarch == VIR_ARCH_X86_64) {
        ret = virFindFileInPath("qemu-system-x86_64");
        if (ret && !virFileIsExecutable(ret))
            VIR_FREE(ret);
    }

    if (guestarch == VIR_ARCH_I686 &&
        !ret) {
        ret = virFindFileInPath("qemu");
        if (ret && !virFileIsExecutable(ret))
            VIR_FREE(ret);
    }

    return ret;
}


static bool
virQEMUCapsIsValidForKVM(virArch hostarch,
                         virArch guestarch)
{
    if (hostarch == guestarch)
        return true;
    if (hostarch == VIR_ARCH_X86_64 &&
        guestarch == VIR_ARCH_I686)
        return true;
    return false;
}

static int
virQEMUCapsInitGuest(virCapsPtr caps,
                     virQEMUCapsCachePtr cache,
                     virArch hostarch,
                     virArch guestarch)
{
    size_t i;
    char *kvmbin = NULL;
    char *binary = NULL;
    virQEMUCapsPtr qemubinCaps = NULL;
    virQEMUCapsPtr kvmbinCaps = NULL;
    int ret = -1;

    /* Check for existence of base emulator, or alternate base
     * which can be used with magic cpu choice
     */
    binary = virQEMUCapsFindBinaryForArch(hostarch, guestarch);

    /* Ignore binary if extracting version info fails */
    if (binary) {
        if (!(qemubinCaps = virQEMUCapsCacheLookup(cache, binary))) {
            virResetLastError();
            VIR_FREE(binary);
        }
    }

    /* qemu-kvm/kvm binaries can only be used if
     *  - host & guest arches match
     * Or
     *  - hostarch is x86_64 and guest arch is i686
     * The latter simply needs "-cpu qemu32"
     */
    if (virQEMUCapsIsValidForKVM(hostarch, guestarch)) {
        const char *const kvmbins[] = { "/usr/libexec/qemu-kvm", /* RHEL */
                                        "qemu-kvm", /* Fedora */
                                        "kvm" }; /* Upstream .spec */

        for (i = 0; i < ARRAY_CARDINALITY(kvmbins); ++i) {
            kvmbin = virFindFileInPath(kvmbins[i]);

            if (!kvmbin)
                continue;

            if (!(kvmbinCaps = virQEMUCapsCacheLookup(cache, kvmbin))) {
                virResetLastError();
                VIR_FREE(kvmbin);
                continue;
            }

            if (!binary) {
                binary = kvmbin;
                qemubinCaps = kvmbinCaps;
                kvmbin = NULL;
                kvmbinCaps = NULL;
            }
            break;
        }
    }

    ret = virQEMUCapsInitGuestFromBinary(caps,
                                         binary, qemubinCaps,
                                         kvmbin, kvmbinCaps,
                                         guestarch);

    VIR_FREE(binary);
    VIR_FREE(kvmbin);
    virObjectUnref(qemubinCaps);
    virObjectUnref(kvmbinCaps);

    return ret;
}

int
virQEMUCapsInitGuestFromBinary(virCapsPtr caps,
                               const char *binary,
                               virQEMUCapsPtr qemubinCaps,
                               const char *kvmbin,
                               virQEMUCapsPtr kvmbinCaps,
                               virArch guestarch)
{
    virCapsGuestPtr guest;
    bool haskvm = false;
    bool haskqemu = false;
    virCapsGuestMachinePtr *machines = NULL;
    size_t nmachines = 0;
    int ret = -1;
    bool hasdisksnapshot = false;

    if (!binary)
        return 0;

    if (virFileExists("/dev/kvm") &&
        (virQEMUCapsGet(qemubinCaps, QEMU_CAPS_KVM) ||
         virQEMUCapsGet(qemubinCaps, QEMU_CAPS_ENABLE_KVM) ||
         kvmbin))
        haskvm = true;

    if (virFileExists("/dev/kqemu") &&
        virQEMUCapsGet(qemubinCaps, QEMU_CAPS_KQEMU))
        haskqemu = true;

    if (virQEMUCapsGetMachineTypesCaps(qemubinCaps, &nmachines, &machines) < 0)
        goto cleanup;

    /* We register kvm as the base emulator too, since we can
     * just give -no-kvm to disable acceleration if required */
    if ((guest = virCapabilitiesAddGuest(caps,
                                         "hvm",
                                         guestarch,
                                         binary,
                                         NULL,
                                         nmachines,
                                         machines)) == NULL)
        goto cleanup;

    machines = NULL;
    nmachines = 0;

    if (caps->host.cpu &&
        caps->host.cpu->model &&
        virQEMUCapsGetCPUDefinitions(qemubinCaps, NULL) > 0 &&
        !virCapabilitiesAddGuestFeature(guest, "cpuselection", true, false))
        goto cleanup;

    if (virQEMUCapsGet(qemubinCaps, QEMU_CAPS_BOOTINDEX) &&
        !virCapabilitiesAddGuestFeature(guest, "deviceboot", true, false))
        goto cleanup;

    if (virQEMUCapsGet(qemubinCaps, QEMU_CAPS_DISK_SNAPSHOT))
        hasdisksnapshot = true;

    if (!virCapabilitiesAddGuestFeature(guest, "disksnapshot", hasdisksnapshot,
                                        false))
        goto cleanup;

    if (virCapabilitiesAddGuestDomain(guest,
                                      "qemu",
                                      NULL,
                                      NULL,
                                      0,
                                      NULL) == NULL)
        goto cleanup;

    if (haskqemu &&
        virCapabilitiesAddGuestDomain(guest,
                                      "kqemu",
                                      NULL,
                                      NULL,
                                      0,
                                      NULL) == NULL)
        goto cleanup;

    if (haskvm) {
        virCapsGuestDomainPtr dom;

        if (kvmbin &&
            virQEMUCapsGetMachineTypesCaps(kvmbinCaps, &nmachines, &machines) < 0)
            goto cleanup;

        if ((dom = virCapabilitiesAddGuestDomain(guest,
                                                 "kvm",
                                                 kvmbin ? kvmbin : binary,
                                                 NULL,
                                                 nmachines,
                                                 machines)) == NULL) {
            goto cleanup;
        }

        machines = NULL;
        nmachines = 0;

    }

    if (((guestarch == VIR_ARCH_I686) ||
         (guestarch == VIR_ARCH_X86_64)) &&
        (virCapabilitiesAddGuestFeature(guest, "acpi", true, true) == NULL ||
         virCapabilitiesAddGuestFeature(guest, "apic", true, false) == NULL))
        goto cleanup;

    if ((guestarch == VIR_ARCH_I686) &&
        (virCapabilitiesAddGuestFeature(guest, "pae", true, false) == NULL ||
         virCapabilitiesAddGuestFeature(guest, "nonpae", true, false) == NULL))
        goto cleanup;

    ret = 0;

 cleanup:

    virCapabilitiesFreeMachines(machines, nmachines);

    return ret;
}


static int
virQEMUCapsInitCPU(virCapsPtr caps,
                   virArch arch)
{
    virCPUDefPtr cpu = NULL;
    virCPUDataPtr data = NULL;
    virNodeInfo nodeinfo;
    int ret = -1;

    if (VIR_ALLOC(cpu) < 0)
        goto error;

    cpu->arch = arch;

    if (nodeGetInfo(&nodeinfo))
        goto error;

    cpu->type = VIR_CPU_TYPE_HOST;
    cpu->sockets = nodeinfo.sockets;
    cpu->cores = nodeinfo.cores;
    cpu->threads = nodeinfo.threads;
    caps->host.cpu = cpu;

    if (!(data = cpuNodeData(arch))
        || cpuDecode(cpu, data, NULL, 0, NULL) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    cpuDataFree(data);

    return ret;

 error:
    virCPUDefFree(cpu);
    goto cleanup;
}


static int
virQEMUCapsInitPages(virCapsPtr caps)
{
    int ret = -1;
    unsigned int *pages_size = NULL;
    size_t npages;

    if (virNumaGetPages(-1 /* Magic constant for overall info */,
                        &pages_size, NULL, NULL, &npages) < 0)
        goto cleanup;

    caps->host.pagesSize = pages_size;
    pages_size = NULL;
    caps->host.nPagesSize = npages;
    npages = 0;

    ret = 0;
 cleanup:
    VIR_FREE(pages_size);
    return ret;
}


virCapsPtr virQEMUCapsInit(virQEMUCapsCachePtr cache)
{
    virCapsPtr caps;
    size_t i;
    virArch hostarch = virArchFromHost();

    if ((caps = virCapabilitiesNew(hostarch,
                                   true, true)) == NULL)
        goto error;

    /* Some machines have problematic NUMA toplogy causing
     * unexpected failures. We don't want to break the QEMU
     * driver in this scenario, so log errors & carry on
     */
    if (nodeCapsInitNUMA(caps) < 0) {
        virCapabilitiesFreeNUMAInfo(caps);
        VIR_WARN("Failed to query host NUMA topology, disabling NUMA capabilities");
    }

    if (virQEMUCapsInitCPU(caps, hostarch) < 0)
        VIR_WARN("Failed to get host CPU");

    /* Add the power management features of the host */
    if (virNodeSuspendGetTargetMask(&caps->host.powerMgmt) < 0)
        VIR_WARN("Failed to get host power management capabilities");

    /* Add huge pages info */
    if (virQEMUCapsInitPages(caps) < 0)
        VIR_WARN("Failed to get pages info");

    /* Add domain migration transport URIs */
    virCapabilitiesAddHostMigrateTransport(caps, "tcp");
    virCapabilitiesAddHostMigrateTransport(caps, "rdma");

    /* QEMU can support pretty much every arch that exists,
     * so just probe for them all - we gracefully fail
     * if a qemu-system-$ARCH binary can't be found
     */
    for (i = 0; i < VIR_ARCH_LAST; i++)
        if (virQEMUCapsInitGuest(caps, cache,
                                 hostarch,
                                 i) < 0)
            goto error;

    return caps;

 error:
    virObjectUnref(caps);
    return NULL;
}


static int
virQEMUCapsComputeCmdFlags(const char *help,
                           unsigned int version,
                           bool is_kvm,
                           unsigned int kvm_version,
                           virQEMUCapsPtr qemuCaps,
                           bool check_yajl ATTRIBUTE_UNUSED)
{
    const char *p;
    const char *fsdev, *netdev;

    if (strstr(help, "-no-kqemu"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_KQEMU);
    if (strstr(help, "-enable-kqemu"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_ENABLE_KQEMU);
    if (strstr(help, "-no-kvm"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_KVM);
    if (strstr(help, "-enable-kvm"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_ENABLE_KVM);
    if (strstr(help, "-no-reboot"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_REBOOT);
    if (strstr(help, "-name")) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NAME);
        if (strstr(help, ",process="))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_NAME_PROCESS);
    }
    if (strstr(help, "-uuid"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_UUID);
    if (strstr(help, "-xen-domid"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_XEN_DOMID);
    else if (strstr(help, "-domid"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_DOMID);
    if (strstr(help, "-drive")) {
        const char *cache = strstr(help, "cache=");

        virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE);
        if (cache && (p = strchr(cache, ']'))) {
            if (memmem(cache, p - cache, "on|off", sizeof("on|off") - 1) == NULL)
                virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_CACHE_V2);
            if (memmem(cache, p - cache, "directsync", sizeof("directsync") - 1))
                virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_CACHE_DIRECTSYNC);
            if (memmem(cache, p - cache, "unsafe", sizeof("unsafe") - 1))
                virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_CACHE_UNSAFE);
        }
        if (strstr(help, "format="))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_FORMAT);
        if (strstr(help, "readonly="))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_READONLY);
        if (strstr(help, "aio=threads|native"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_AIO);
        if (strstr(help, "copy-on-read=on|off"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_COPY_ON_READ);
        if (strstr(help, "bps="))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_IOTUNE);
    }
    if ((p = strstr(help, "-vga")) && !strstr(help, "-std-vga")) {
        const char *nl = strstr(p, "\n");

        virQEMUCapsSet(qemuCaps, QEMU_CAPS_VGA);

        if (strstr(p, "|qxl"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_VGA_QXL);
        if ((p = strstr(p, "|none")) && p < nl)
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_VGA_NONE);
    }
    if (strstr(help, "-spice"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_SPICE);
    if (strstr(help, "-vnc"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_VNC);
    if (strstr(help, "seamless-migration="))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_SEAMLESS_MIGRATION);
    if (strstr(help, "boot=on"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_BOOT);
    if (strstr(help, "serial=s"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_SERIAL);
    if (strstr(help, "-pcidevice"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_PCIDEVICE);
    if (strstr(help, "host=[seg:]bus"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_HOST_PCI_MULTIDOMAIN);
    if (strstr(help, "-mem-path"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_MEM_PATH);
    if (strstr(help, "-chardev")) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_CHARDEV);
        if (strstr(help, "-chardev spicevmc"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_CHARDEV_SPICEVMC);
        if (strstr(help, "-chardev spiceport"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_CHARDEV_SPICEPORT);
    }
    if (strstr(help, "-balloon"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_BALLOON);
    if (strstr(help, "-device")) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_DEVICE);
        /*
         * When -device was introduced, qemu already supported drive's
         * readonly option but didn't advertise that.
         */
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_READONLY);
    }
    if (strstr(help, "-nodefconfig"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NODEFCONFIG);
    if (strstr(help, "-no-user-config"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_USER_CONFIG);
    /* The trailing ' ' is important to avoid a bogus match */
    if (strstr(help, "-rtc "))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_RTC);
    /* to wit */
    if (strstr(help, "-rtc-td-hack"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_RTC_TD_HACK);
    if (strstr(help, "-no-hpet"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_HPET);
    if (strstr(help, "-no-acpi"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_ACPI);
    if (strstr(help, "-no-kvm-pit-reinjection"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_KVM_PIT);
    if (strstr(help, "-tdf"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_TDF);
    if (strstr(help, "-enable-nesting"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NESTING);
    if (strstr(help, ",menu=on"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_BOOT_MENU);
    if (strstr(help, ",reboot-timeout=rb_time"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_REBOOT_TIMEOUT);
    if (strstr(help, ",splash-time=sp_time"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_SPLASH_TIMEOUT);
    if ((fsdev = strstr(help, "-fsdev"))) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_FSDEV);
        if (strstr(fsdev, "readonly"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_FSDEV_READONLY);
        if (strstr(fsdev, "writeout"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_FSDEV_WRITEOUT);
    }
    if (strstr(help, "-smbios type"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_SMBIOS_TYPE);
    if (strstr(help, "-sandbox"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_SECCOMP_SANDBOX);

    if ((netdev = strstr(help, "-netdev"))) {
        /* Disable -netdev on 0.12 since although it exists,
         * the corresponding netdev_add/remove monitor commands
         * do not, and we need them to be able to do hotplug.
         * But see below about RHEL build. */
        if (version >= 13000) {
            if (strstr(netdev, "bridge"))
                virQEMUCapsSet(qemuCaps, QEMU_CAPS_NETDEV_BRIDGE);
           virQEMUCapsSet(qemuCaps, QEMU_CAPS_NETDEV);
        }
    }

    if (strstr(help, "-sdl"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_SDL);
    if (strstr(help, "cores=") &&
        strstr(help, "threads=") &&
        strstr(help, "sockets="))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_SMP_TOPOLOGY);

    if (version >= 9000)
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_VNC_COLON);

    if (is_kvm && (version >= 10000 || kvm_version >= 74))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_VNET_HDR);

    if (strstr(help, ",vhost="))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_VHOST_NET);

    /* Do not use -no-shutdown if qemu doesn't support it or SIGTERM handling
     * is most likely buggy when used with -no-shutdown (which applies for qemu
     * 0.14.* and 0.15.0)
     */
    if (strstr(help, "-no-shutdown") && (version < 14000 || version > 15000))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_SHUTDOWN);

    if (strstr(help, "dump-guest-core=on|off"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_DUMP_GUEST_CORE);

    if (strstr(help, "-dtb"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_DTB);

    if (strstr(help, "-machine"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_MACHINE_OPT);

    /*
     * Handling of -incoming arg with varying features
     *  -incoming tcp    (kvm >= 79, qemu >= 0.10.0)
     *  -incoming exec   (kvm >= 80, qemu >= 0.10.0)
     *  -incoming unix   (qemu >= 0.12.0)
     *  -incoming fd     (qemu >= 0.12.0)
     *  -incoming stdio  (all earlier kvm)
     *
     * NB, there was a pre-kvm-79 'tcp' support, but it
     * was broken, because it blocked the monitor console
     * while waiting for data, so pretend it doesn't exist
     */
    if (version >= 10000) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_TCP);
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_EXEC);
        if (version >= 12000) {
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_UNIX);
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_FD);
        }
    } else if (kvm_version >= 79) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_TCP);
        if (kvm_version >= 80)
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_EXEC);
    } else if (kvm_version > 0) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_KVM_STDIO);
    }

    if (version >= 10000)
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_0_10);

    if (version >= 11000)
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_VIRTIO_BLK_SG_IO);

    /* While JSON mode was available in 0.12.0, it was too
     * incomplete to contemplate using. The 0.13.0 release
     * is good enough to use, even though it lacks one or
     * two features. This is also true of versions of qemu
     * built for RHEL, labeled 0.12.1, but with extra text
     * in the help output that mentions that features were
     * backported for libvirt. The benefits of JSON mode now
     * outweigh the downside.
     */
#if WITH_YAJL
    if (version >= 13000) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_MONITOR_JSON);
    } else if (version >= 12000 &&
               strstr(help, "libvirt")) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_MONITOR_JSON);
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NETDEV);
    }
#else
    /* Starting with qemu 0.15 and newer, upstream qemu no longer
     * promises to keep the human interface stable, but requests that
     * we use QMP (the JSON interface) for everything.  If the user
     * forgot to include YAJL libraries when building their own
     * libvirt but is targetting a newer qemu, we are better off
     * telling them to recompile (the spec file includes the
     * dependency, so distros won't hit this).  This check is
     * also in m4/virt-yajl.m4 (see $with_yajl).  */
    if (version >= 15000 ||
        (version >= 12000 && strstr(help, "libvirt"))) {
        if (check_yajl) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("this qemu binary requires libvirt to be "
                             "compiled with yajl"));
            return -1;
        }
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NETDEV);
    }
#endif

    if (version >= 13000)
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_PCI_MULTIFUNCTION);

    /* Although very new versions of qemu advertise the presence of
     * the rombar option in the output of "qemu -device pci-assign,?",
     * this advertisement was added to the code long after the option
     * itself. According to qemu developers, though, rombar is
     * available in all qemu binaries from release 0.12 onward.
     * Setting the capability this way makes it available in more
     * cases where it might be needed, and shouldn't cause any false
     * positives (in the case that it did, qemu would produce an error
     * log and refuse to start, so it would be immediately obvious).
     */
    if (version >= 12000)
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_PCI_ROMBAR);

    if (version >= 11000)
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_CPU_HOST);

    if (version >= 1001000) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_IPV6_MIGRATION);
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_VNC_SHARE_POLICY);
    }

    return 0;
}

/* We parse the output of 'qemu -help' to get the QEMU
 * version number. The first bit is easy, just parse
 * 'QEMU PC emulator version x.y.z'
 * or
 * 'QEMU emulator version x.y.z'.
 *
 * With qemu-kvm, however, that is followed by a string
 * in parenthesis as follows:
 *  - qemu-kvm-x.y.z in stable releases
 *  - kvm-XX for kvm versions up to kvm-85
 *  - qemu-kvm-devel-XX for kvm version kvm-86 and later
 *
 * For qemu-kvm versions before 0.10.z, we need to detect
 * the KVM version number for some features. With 0.10.z
 * and later, we just need the QEMU version number and
 * whether it is KVM QEMU or mainline QEMU.
 */
#define QEMU_VERSION_STR_1  "QEMU emulator version"
#define QEMU_VERSION_STR_2  "QEMU PC emulator version"
#define QEMU_KVM_VER_PREFIX "(qemu-kvm-"
#define KVM_VER_PREFIX      "(kvm-"

#define SKIP_BLANKS(p) do { while ((*(p) == ' ') || (*(p) == '\t')) (p)++; } while (0)

int virQEMUCapsParseHelpStr(const char *qemu,
                            const char *help,
                            virQEMUCapsPtr qemuCaps,
                            unsigned int *version,
                            bool *is_kvm,
                            unsigned int *kvm_version,
                            bool check_yajl,
                            const char *qmperr)
{
    unsigned major, minor, micro;
    const char *p = help;
    char *strflags;

    *version = *kvm_version = 0;
    *is_kvm = false;

    if (STRPREFIX(p, QEMU_VERSION_STR_1))
        p += strlen(QEMU_VERSION_STR_1);
    else if (STRPREFIX(p, QEMU_VERSION_STR_2))
        p += strlen(QEMU_VERSION_STR_2);
    else
        goto fail;

    SKIP_BLANKS(p);

    major = virParseNumber(&p);
    if (major == -1 || *p != '.')
        goto fail;

    ++p;

    minor = virParseNumber(&p);
    if (minor == -1)
        goto fail;

    if (*p != '.') {
        micro = 0;
    } else {
        ++p;
        micro = virParseNumber(&p);
        if (micro == -1)
            goto fail;
    }

    SKIP_BLANKS(p);

    if (STRPREFIX(p, QEMU_KVM_VER_PREFIX)) {
        *is_kvm = true;
        p += strlen(QEMU_KVM_VER_PREFIX);
    } else if (STRPREFIX(p, KVM_VER_PREFIX)) {
        int ret;

        *is_kvm = true;
        p += strlen(KVM_VER_PREFIX);

        ret = virParseNumber(&p);
        if (ret == -1)
            goto fail;

        *kvm_version = ret;
    }

    *version = (major * 1000 * 1000) + (minor * 1000) + micro;

    /* Refuse to parse -help output for QEMU releases >= 1.2.0 that should be
     * using QMP probing.
     */
    if (*version >= 1002000) {
        if (qmperr && *qmperr) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("QEMU / QMP failed: %s"),
                           qmperr);
        } else {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("QEMU %u.%u.%u is too new for help parsing"),
                           major, minor, micro);
        }
        goto cleanup;
    }

    if (virQEMUCapsComputeCmdFlags(help, *version, *is_kvm, *kvm_version,
                                   qemuCaps, check_yajl) < 0)
        goto cleanup;

    strflags = virBitmapString(qemuCaps->flags);
    VIR_DEBUG("Version %u.%u.%u, cooked version %u, flags %s",
              major, minor, micro, *version, NULLSTR(strflags));
    VIR_FREE(strflags);

    if (*kvm_version)
        VIR_DEBUG("KVM version %d detected", *kvm_version);
    else if (*is_kvm)
        VIR_DEBUG("qemu-kvm version %u.%u.%u detected", major, minor, micro);

    return 0;

 fail:
    p = strchr(help, '\n');
    if (!p)
        p = strchr(help, '\0');

    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("cannot parse %s version number in '%.*s'"),
                   qemu, (int) (p - help), help);

 cleanup:
    return -1;
}


struct virQEMUCapsStringFlags {
    const char *value;
    int flag;
};


struct virQEMUCapsStringFlags virQEMUCapsCommands[] = {
    { "system_wakeup", QEMU_CAPS_WAKEUP },
    { "transaction", QEMU_CAPS_TRANSACTION },
    { "block_stream", QEMU_CAPS_BLOCKJOB_SYNC },
    { "block-stream", QEMU_CAPS_BLOCKJOB_ASYNC },
    { "dump-guest-memory", QEMU_CAPS_DUMP_GUEST_MEMORY },
    { "query-spice", QEMU_CAPS_SPICE },
    { "query-kvm", QEMU_CAPS_KVM },
    { "block-commit", QEMU_CAPS_BLOCK_COMMIT },
    { "query-vnc", QEMU_CAPS_VNC },
    { "drive-mirror", QEMU_CAPS_DRIVE_MIRROR },
    { "blockdev-snapshot-sync", QEMU_CAPS_DISK_SNAPSHOT },
    { "add-fd", QEMU_CAPS_ADD_FD },
    { "nbd-server-start", QEMU_CAPS_NBD_SERVER },
    { "change-backing-file", QEMU_CAPS_CHANGE_BACKING_FILE },
    { "rtc-reset-reinjection", QEMU_CAPS_RTC_RESET_REINJECTION },
};

struct virQEMUCapsStringFlags virQEMUCapsMigration[] = {
    { "rdma-pin-all", QEMU_CAPS_MIGRATE_RDMA },
};

struct virQEMUCapsStringFlags virQEMUCapsEvents[] = {
    { "BALLOON_CHANGE", QEMU_CAPS_BALLOON_EVENT },
    { "SPICE_MIGRATE_COMPLETED", QEMU_CAPS_SEAMLESS_MIGRATION },
    { "DEVICE_DELETED", QEMU_CAPS_DEVICE_DEL_EVENT },
};

struct virQEMUCapsStringFlags virQEMUCapsObjectTypes[] = {
    { "hda-duplex", QEMU_CAPS_HDA_DUPLEX },
    { "hda-micro", QEMU_CAPS_HDA_MICRO },
    { "ccid-card-emulated", QEMU_CAPS_CCID_EMULATED },
    { "ccid-card-passthru", QEMU_CAPS_CCID_PASSTHRU },
    { "piix3-usb-uhci", QEMU_CAPS_PIIX3_USB_UHCI },
    { "piix4-usb-uhci", QEMU_CAPS_PIIX4_USB_UHCI },
    { "usb-ehci", QEMU_CAPS_USB_EHCI },
    { "ich9-usb-ehci1", QEMU_CAPS_ICH9_USB_EHCI1 },
    { "vt82c686b-usb-uhci", QEMU_CAPS_VT82C686B_USB_UHCI },
    { "pci-ohci", QEMU_CAPS_PCI_OHCI },
    { "nec-usb-xhci", QEMU_CAPS_NEC_USB_XHCI },
    { "usb-redir", QEMU_CAPS_USB_REDIR },
    { "usb-hub", QEMU_CAPS_USB_HUB },
    { "ich9-ahci", QEMU_CAPS_ICH9_AHCI },
    { "virtio-blk-s390", QEMU_CAPS_VIRTIO_S390 },
    { "virtio-blk-ccw", QEMU_CAPS_VIRTIO_CCW },
    { "sclpconsole", QEMU_CAPS_SCLP_S390 },
    { "lsi53c895a", QEMU_CAPS_SCSI_LSI },
    { "virtio-scsi-pci", QEMU_CAPS_VIRTIO_SCSI },
    { "virtio-scsi-s390", QEMU_CAPS_VIRTIO_SCSI },
    { "virtio-scsi-ccw", QEMU_CAPS_VIRTIO_SCSI },
    { "megasas", QEMU_CAPS_SCSI_MEGASAS },
    { "spicevmc", QEMU_CAPS_DEVICE_SPICEVMC },
    { "qxl-vga", QEMU_CAPS_DEVICE_QXL_VGA },
    { "qxl", QEMU_CAPS_DEVICE_QXL },
    { "sga", QEMU_CAPS_SGA },
    { "scsi-block", QEMU_CAPS_SCSI_BLOCK },
    { "scsi-cd", QEMU_CAPS_SCSI_CD },
    { "ide-cd", QEMU_CAPS_IDE_CD },
    { "VGA", QEMU_CAPS_DEVICE_VGA },
    { "cirrus-vga", QEMU_CAPS_DEVICE_CIRRUS_VGA },
    { "vmware-svga", QEMU_CAPS_DEVICE_VMWARE_SVGA },
    { "usb-serial", QEMU_CAPS_DEVICE_USB_SERIAL },
    { "usb-net", QEMU_CAPS_DEVICE_USB_NET },
    { "virtio-rng-pci", QEMU_CAPS_DEVICE_VIRTIO_RNG },
    { "virtio-rng-s390", QEMU_CAPS_DEVICE_VIRTIO_RNG },
    { "virtio-rng-ccw", QEMU_CAPS_DEVICE_VIRTIO_RNG },
    { "rng-random", QEMU_CAPS_OBJECT_RNG_RANDOM },
    { "rng-egd", QEMU_CAPS_OBJECT_RNG_EGD },
    { "spapr-nvram", QEMU_CAPS_DEVICE_NVRAM },
    { "pci-bridge", QEMU_CAPS_DEVICE_PCI_BRIDGE },
    { "vfio-pci", QEMU_CAPS_DEVICE_VFIO_PCI },
    { "scsi-generic", QEMU_CAPS_DEVICE_SCSI_GENERIC },
    { "i82801b11-bridge", QEMU_CAPS_DEVICE_DMI_TO_PCI_BRIDGE },
    { "usb-storage", QEMU_CAPS_DEVICE_USB_STORAGE },
    { "virtio-mmio", QEMU_CAPS_DEVICE_VIRTIO_MMIO },
    { "ich9-intel-hda", QEMU_CAPS_DEVICE_ICH9_INTEL_HDA },
    { "pvpanic", QEMU_CAPS_DEVICE_PANIC },
    { "usb-kbd", QEMU_CAPS_DEVICE_USB_KBD },
    { "memory-backend-ram", QEMU_CAPS_OBJECT_MEMORY_RAM },
    { "memory-backend-file", QEMU_CAPS_OBJECT_MEMORY_FILE },
    { "usb-audio", QEMU_CAPS_OBJECT_USB_AUDIO },
    { "iothread", QEMU_CAPS_OBJECT_IOTHREAD},
    { "ivshmem", QEMU_CAPS_DEVICE_IVSHMEM },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsVirtioBlk[] = {
    { "multifunction", QEMU_CAPS_PCI_MULTIFUNCTION },
    { "bootindex", QEMU_CAPS_BOOTINDEX },
    { "ioeventfd", QEMU_CAPS_VIRTIO_IOEVENTFD },
    { "event_idx", QEMU_CAPS_VIRTIO_BLK_EVENT_IDX },
    { "scsi", QEMU_CAPS_VIRTIO_BLK_SCSI },
    { "logical_block_size", QEMU_CAPS_BLOCKIO },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsVirtioNet[] = {
    { "tx", QEMU_CAPS_VIRTIO_TX_ALG },
    { "event_idx", QEMU_CAPS_VIRTIO_NET_EVENT_IDX },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsPCIAssign[] = {
    { "rombar", QEMU_CAPS_PCI_ROMBAR },
    { "configfd", QEMU_CAPS_PCI_CONFIGFD },
    { "bootindex", QEMU_CAPS_PCI_BOOTINDEX },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsVfioPCI[] = {
    { "bootindex", QEMU_CAPS_VFIO_PCI_BOOTINDEX },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsSCSIDisk[] = {
    { "channel", QEMU_CAPS_SCSI_DISK_CHANNEL },
    { "wwn", QEMU_CAPS_SCSI_DISK_WWN },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsIDEDrive[] = {
    { "wwn", QEMU_CAPS_IDE_DRIVE_WWN },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsPixx4PM[] = {
    { "disable_s3", QEMU_CAPS_DISABLE_S3 },
    { "disable_s4", QEMU_CAPS_DISABLE_S4 },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsUSBRedir[] = {
    { "filter", QEMU_CAPS_USB_REDIR_FILTER },
    { "bootindex", QEMU_CAPS_USB_REDIR_BOOTINDEX },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsUSBHost[] = {
    { "bootindex", QEMU_CAPS_USB_HOST_BOOTINDEX },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsSCSIGeneric[] = {
    { "bootindex", QEMU_CAPS_DEVICE_SCSI_GENERIC_BOOTINDEX },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsI440FXPCIHost[] = {
    { "pci-hole64-size", QEMU_CAPS_I440FX_PCI_HOLE64_SIZE },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsQ35PCIHost[] = {
    { "pci-hole64-size", QEMU_CAPS_Q35_PCI_HOLE64_SIZE },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsUSBStorage[] = {
    { "removable", QEMU_CAPS_USB_STORAGE_REMOVABLE },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsKVMPit[] = {
    { "lost_tick_policy", QEMU_CAPS_KVM_PIT_TICK_POLICY },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsVGA[] = {
    { "vgamem_mb", QEMU_CAPS_VGA_VGAMEM },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsVmwareSvga[] = {
    { "vgamem_mb", QEMU_CAPS_VMWARE_SVGA_VGAMEM },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsQxl[] = {
    { "vgamem_mb", QEMU_CAPS_QXL_VGAMEM },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsQxlVga[] = {
    { "vgamem_mb", QEMU_CAPS_QXL_VGA_VGAMEM },
};

struct virQEMUCapsObjectTypeProps {
    const char *type;
    struct virQEMUCapsStringFlags *props;
    size_t nprops;
};

static struct virQEMUCapsObjectTypeProps virQEMUCapsObjectProps[] = {
    { "virtio-blk-pci", virQEMUCapsObjectPropsVirtioBlk,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsVirtioBlk) },
    { "virtio-net-pci", virQEMUCapsObjectPropsVirtioNet,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsVirtioNet) },
    { "virtio-blk-ccw", virQEMUCapsObjectPropsVirtioBlk,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsVirtioBlk) },
    { "virtio-net-ccw", virQEMUCapsObjectPropsVirtioNet,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsVirtioNet) },
    { "virtio-blk-s390", virQEMUCapsObjectPropsVirtioBlk,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsVirtioBlk) },
    { "virtio-net-s390", virQEMUCapsObjectPropsVirtioNet,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsVirtioNet) },
    { "pci-assign", virQEMUCapsObjectPropsPCIAssign,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsPCIAssign) },
    { "kvm-pci-assign", virQEMUCapsObjectPropsPCIAssign,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsPCIAssign) },
    { "vfio-pci", virQEMUCapsObjectPropsVfioPCI,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsVfioPCI) },
    { "scsi-disk", virQEMUCapsObjectPropsSCSIDisk,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsSCSIDisk) },
    { "ide-drive", virQEMUCapsObjectPropsIDEDrive,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsIDEDrive) },
    { "PIIX4_PM", virQEMUCapsObjectPropsPixx4PM,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsPixx4PM) },
    { "usb-redir", virQEMUCapsObjectPropsUSBRedir,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsUSBRedir) },
    { "usb-host", virQEMUCapsObjectPropsUSBHost,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsUSBHost) },
    { "scsi-generic", virQEMUCapsObjectPropsSCSIGeneric,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsSCSIGeneric) },
    { "i440FX-pcihost", virQEMUCapsObjectPropsI440FXPCIHost,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsI440FXPCIHost) },
    { "q35-pcihost", virQEMUCapsObjectPropsQ35PCIHost,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsQ35PCIHost) },
    { "usb-storage", virQEMUCapsObjectPropsUSBStorage,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsUSBStorage) },
    { "kvm-pit", virQEMUCapsObjectPropsKVMPit,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsKVMPit) },
    { "VGA", virQEMUCapsObjectPropsVGA,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsVGA) },
    { "vmware-svga", virQEMUCapsObjectPropsVmwareSvga,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsVmwareSvga) },
    { "qxl", virQEMUCapsObjectPropsQxl,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsQxl) },
    { "qxl-vga", virQEMUCapsObjectPropsQxlVga,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsQxlVga) },
};


static void
virQEMUCapsProcessStringFlags(virQEMUCapsPtr qemuCaps,
                              size_t nflags,
                              struct virQEMUCapsStringFlags *flags,
                              size_t nvalues,
                              char *const*values)
{
    size_t i, j;
    for (i = 0; i < nflags; i++) {
        for (j = 0; j < nvalues; j++) {
            if (STREQ(values[j], flags[i].value)) {
                virQEMUCapsSet(qemuCaps, flags[i].flag);
                break;
            }
        }
    }
}


static void
virQEMUCapsFreeStringList(size_t len,
                          char **values)
{
    size_t i;
    for (i = 0; i < len; i++)
        VIR_FREE(values[i]);
    VIR_FREE(values);
}


#define OBJECT_TYPE_PREFIX "name \""

static int
virQEMUCapsParseDeviceStrObjectTypes(const char *str,
                                     char ***types)
{
    const char *tmp = str;
    int ret = -1;
    size_t ntypelist = 0;
    char **typelist = NULL;

    *types = NULL;

    while ((tmp = strstr(tmp, OBJECT_TYPE_PREFIX))) {
        char *end;
        tmp += strlen(OBJECT_TYPE_PREFIX);
        end = strstr(tmp, "\"");
        if (!end) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Malformed QEMU device list string, missing quote"));
            goto cleanup;
        }

        if (VIR_EXPAND_N(typelist, ntypelist, 1) < 0)
            goto cleanup;
        if (VIR_STRNDUP(typelist[ntypelist - 1], tmp, end-tmp) < 0)
            goto cleanup;
    }

    *types = typelist;
    ret = ntypelist;

 cleanup:
    if (ret < 0)
        virQEMUCapsFreeStringList(ntypelist, typelist);
    return ret;
}


static int
virQEMUCapsParseDeviceStrObjectProps(const char *str,
                                     const char *type,
                                     char ***props)
{
    const char *tmp = str;
    int ret = -1;
    size_t nproplist = 0;
    char **proplist = NULL;

    VIR_DEBUG("Extract type %s", type);
    *props = NULL;

    while ((tmp = strchr(tmp, '\n'))) {
        char *end;
        tmp += 1;

        if (*tmp == '\0')
            break;

        if (STRPREFIX(tmp, OBJECT_TYPE_PREFIX))
            continue;

        if (!STRPREFIX(tmp, type))
            continue;

        tmp += strlen(type);
        if (*tmp != '.')
            continue;
        tmp++;

        end = strstr(tmp, "=");
        if (!end) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Malformed QEMU device list string, missing '='"));
            goto cleanup;
        }
        if (VIR_EXPAND_N(proplist, nproplist, 1) < 0)
            goto cleanup;
        if (VIR_STRNDUP(proplist[nproplist - 1], tmp, end-tmp) < 0)
            goto cleanup;
    }

    *props = proplist;
    ret = nproplist;

 cleanup:
    if (ret < 0 && proplist)
        virQEMUCapsFreeStringList(nproplist, proplist);
    return ret;
}


int
virQEMUCapsParseDeviceStr(virQEMUCapsPtr qemuCaps, const char *str)
{
    int nvalues;
    char **values;
    size_t i;

    if ((nvalues = virQEMUCapsParseDeviceStrObjectTypes(str, &values)) < 0)
        return -1;
    virQEMUCapsProcessStringFlags(qemuCaps,
                                  ARRAY_CARDINALITY(virQEMUCapsObjectTypes),
                                  virQEMUCapsObjectTypes,
                                  nvalues, values);
    virQEMUCapsFreeStringList(nvalues, values);

    for (i = 0; i < ARRAY_CARDINALITY(virQEMUCapsObjectProps); i++) {
        const char *type = virQEMUCapsObjectProps[i].type;
        if ((nvalues = virQEMUCapsParseDeviceStrObjectProps(str,
                                                            type,
                                                            &values)) < 0)
            return -1;
        virQEMUCapsProcessStringFlags(qemuCaps,
                                      virQEMUCapsObjectProps[i].nprops,
                                      virQEMUCapsObjectProps[i].props,
                                      nvalues, values);
        virQEMUCapsFreeStringList(nvalues, values);
    }

    /* Prefer -chardev spicevmc (detected earlier) over -device spicevmc */
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_CHARDEV_SPICEVMC))
        virQEMUCapsClear(qemuCaps, QEMU_CAPS_DEVICE_SPICEVMC);

    return 0;
}


static int
virQEMUCapsExtractDeviceStr(const char *qemu,
                            virQEMUCapsPtr qemuCaps,
                            uid_t runUid, gid_t runGid)
{
    char *output = NULL;
    virCommandPtr cmd;
    int ret = -1;

    /* Cram together all device-related queries into one invocation;
     * the output format makes it possible to distinguish what we
     * need.  With qemu 0.13.0 and later, unrecognized '-device
     * bogus,?' cause an error in isolation, but are silently ignored
     * in combination with '-device ?'.  Upstream qemu 0.12.x doesn't
     * understand '-device name,?', and always exits with status 1 for
     * the simpler '-device ?', so this function is really only useful
     * if -help includes "device driver,?".  */
    cmd = virQEMUCapsProbeCommand(qemu, qemuCaps, runUid, runGid);
    virCommandAddArgList(cmd,
                         "-device", "?",
                         "-device", "pci-assign,?",
                         "-device", "virtio-blk-pci,?",
                         "-device", "virtio-net-pci,?",
                         "-device", "scsi-disk,?",
                         "-device", "PIIX4_PM,?",
                         "-device", "usb-redir,?",
                         "-device", "ide-drive,?",
                         "-device", "usb-host,?",
                         "-device", "scsi-generic,?",
                         "-device", "usb-storage,?",
                         "-device", "VGA,?",
                         "-device", "vmware-svga,?",
                         "-device", "qxl,?",
                         "-device", "qxl-vga,?",
                         NULL);
    /* qemu -help goes to stdout, but qemu -device ? goes to stderr.  */
    virCommandSetErrorBuffer(cmd, &output);

    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    ret = virQEMUCapsParseDeviceStr(qemuCaps, output);

 cleanup:
    VIR_FREE(output);
    virCommandFree(cmd);
    return ret;
}


int virQEMUCapsGetDefaultVersion(virCapsPtr caps,
                                 virQEMUCapsCachePtr capsCache,
                                 unsigned int *version)
{
    const char *binary;
    virQEMUCapsPtr qemucaps;
    virArch hostarch;

    if (*version > 0)
        return 0;

    hostarch = virArchFromHost();
    if ((binary = virCapabilitiesDefaultGuestEmulator(caps,
                                                      "hvm",
                                                      hostarch,
                                                      "qemu")) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Cannot find suitable emulator for %s"),
                       virArchToString(hostarch));
        return -1;
    }

    if (!(qemucaps = virQEMUCapsCacheLookup(capsCache, binary)))
        return -1;

    *version = virQEMUCapsGetVersion(qemucaps);
    virObjectUnref(qemucaps);
    return 0;
}




virQEMUCapsPtr
virQEMUCapsNew(void)
{
    virQEMUCapsPtr qemuCaps;

    if (virQEMUCapsInitialize() < 0)
        return NULL;

    if (!(qemuCaps = virObjectNew(virQEMUCapsClass)))
        return NULL;

    if (!(qemuCaps->flags = virBitmapNew(QEMU_CAPS_LAST)))
        goto error;

    return qemuCaps;

 error:
    virObjectUnref(qemuCaps);
    return NULL;
}


virQEMUCapsPtr virQEMUCapsNewCopy(virQEMUCapsPtr qemuCaps)
{
    virQEMUCapsPtr ret = virQEMUCapsNew();
    size_t i;

    if (!ret)
        return NULL;

    virBitmapCopy(ret->flags, qemuCaps->flags);

    ret->usedQMP = qemuCaps->usedQMP;
    ret->version = qemuCaps->version;
    ret->kvmVersion = qemuCaps->kvmVersion;
    ret->arch = qemuCaps->arch;

    if (VIR_ALLOC_N(ret->cpuDefinitions, qemuCaps->ncpuDefinitions) < 0)
        goto error;
    ret->ncpuDefinitions = qemuCaps->ncpuDefinitions;
    for (i = 0; i < qemuCaps->ncpuDefinitions; i++) {
        if (VIR_STRDUP(ret->cpuDefinitions[i], qemuCaps->cpuDefinitions[i]) < 0)
            goto error;
    }

    if (VIR_ALLOC_N(ret->machineTypes, qemuCaps->nmachineTypes) < 0)
        goto error;
    if (VIR_ALLOC_N(ret->machineAliases, qemuCaps->nmachineTypes) < 0)
        goto error;
    if (VIR_ALLOC_N(ret->machineMaxCpus, qemuCaps->nmachineTypes) < 0)
        goto error;
    ret->nmachineTypes = qemuCaps->nmachineTypes;
    for (i = 0; i < qemuCaps->nmachineTypes; i++) {
        if (VIR_STRDUP(ret->machineTypes[i], qemuCaps->machineTypes[i]) < 0 ||
            VIR_STRDUP(ret->machineAliases[i], qemuCaps->machineAliases[i]) < 0)
            goto error;
        ret->machineMaxCpus[i] = qemuCaps->machineMaxCpus[i];
    }

    return ret;

 error:
    virObjectUnref(ret);
    return NULL;
}


void virQEMUCapsDispose(void *obj)
{
    virQEMUCapsPtr qemuCaps = obj;
    size_t i;

    for (i = 0; i < qemuCaps->nmachineTypes; i++) {
        VIR_FREE(qemuCaps->machineTypes[i]);
        VIR_FREE(qemuCaps->machineAliases[i]);
    }
    VIR_FREE(qemuCaps->machineTypes);
    VIR_FREE(qemuCaps->machineAliases);
    VIR_FREE(qemuCaps->machineMaxCpus);

    for (i = 0; i < qemuCaps->ncpuDefinitions; i++)
        VIR_FREE(qemuCaps->cpuDefinitions[i]);
    VIR_FREE(qemuCaps->cpuDefinitions);

    virBitmapFree(qemuCaps->flags);

    VIR_FREE(qemuCaps->binary);
}

void
virQEMUCapsSet(virQEMUCapsPtr qemuCaps,
               virQEMUCapsFlags flag)
{
    ignore_value(virBitmapSetBit(qemuCaps->flags, flag));
}


void
virQEMUCapsSetList(virQEMUCapsPtr qemuCaps, ...)
{
    va_list list;
    int flag;

    va_start(list, qemuCaps);
    while ((flag = va_arg(list, int)) < QEMU_CAPS_LAST)
        ignore_value(virBitmapSetBit(qemuCaps->flags, flag));
    va_end(list);
}


void
virQEMUCapsClear(virQEMUCapsPtr qemuCaps,
                 virQEMUCapsFlags flag)
{
    ignore_value(virBitmapClearBit(qemuCaps->flags, flag));
}


char *virQEMUCapsFlagsString(virQEMUCapsPtr qemuCaps)
{
    return virBitmapString(qemuCaps->flags);
}


bool
virQEMUCapsGet(virQEMUCapsPtr qemuCaps,
               virQEMUCapsFlags flag)
{
    bool b;

    if (!qemuCaps || virBitmapGetBit(qemuCaps->flags, flag, &b) < 0)
        return false;
    else
        return b;
}


bool virQEMUCapsHasPCIMultiBus(virQEMUCapsPtr qemuCaps,
                               virDomainDefPtr def)
{
    bool hasMultiBus = virQEMUCapsGet(qemuCaps, QEMU_CAPS_PCI_MULTIBUS);

    if (hasMultiBus)
        return true;

    if (def->os.arch == VIR_ARCH_PPC ||
        ARCH_IS_PPC64(def->os.arch)) {
        /*
         * Usage of pci.0 naming:
         *
         *    ref405ep: no pci
         *       taihu: no pci
         *      bamboo: 1.1.0
         *       mac99: 2.0.0
         *     g3beige: 2.0.0
         *        prep: 1.4.0
         *     pseries: 2.0.0
         *   mpc8544ds: forever
         * virtex-m507: no pci
         *     ppce500: 1.6.0
         */

        if (qemuCaps->version >= 2000000)
            return true;

        if (qemuCaps->version >= 1006000 &&
            STREQ(def->os.machine, "ppce500"))
            return true;

        if (qemuCaps->version >= 1004000 &&
            STREQ(def->os.machine, "prep"))
            return true;

        if (qemuCaps->version >= 1001000 &&
            STREQ(def->os.machine, "bamboo"))
            return true;

        if (STREQ(def->os.machine, "mpc8544ds"))
            return true;

        return false;
    }

    return false;
}


const char *virQEMUCapsGetBinary(virQEMUCapsPtr qemuCaps)
{
    return qemuCaps->binary;
}

virArch virQEMUCapsGetArch(virQEMUCapsPtr qemuCaps)
{
    return qemuCaps->arch;
}


unsigned int virQEMUCapsGetVersion(virQEMUCapsPtr qemuCaps)
{
    return qemuCaps->version;
}


unsigned int virQEMUCapsGetKVMVersion(virQEMUCapsPtr qemuCaps)
{
    return qemuCaps->kvmVersion;
}


int virQEMUCapsAddCPUDefinition(virQEMUCapsPtr qemuCaps,
                                const char *name)
{
    char *tmp;

    if (VIR_STRDUP(tmp, name) < 0)
        return -1;
    if (VIR_EXPAND_N(qemuCaps->cpuDefinitions, qemuCaps->ncpuDefinitions, 1) < 0) {
        VIR_FREE(tmp);
        return -1;
    }
    qemuCaps->cpuDefinitions[qemuCaps->ncpuDefinitions-1] = tmp;
    return 0;
}


size_t virQEMUCapsGetCPUDefinitions(virQEMUCapsPtr qemuCaps,
                                    char ***names)
{
    if (names)
        *names = qemuCaps->cpuDefinitions;
    return qemuCaps->ncpuDefinitions;
}


size_t virQEMUCapsGetMachineTypes(virQEMUCapsPtr qemuCaps,
                                  char ***names)
{
    if (names)
        *names = qemuCaps->machineTypes;
    return qemuCaps->nmachineTypes;
}

int virQEMUCapsGetMachineTypesCaps(virQEMUCapsPtr qemuCaps,
                                   size_t *nmachines,
                                   virCapsGuestMachinePtr **machines)
{
    size_t i;

    *machines = NULL;
    *nmachines = qemuCaps->nmachineTypes;

    if (*nmachines &&
        VIR_ALLOC_N(*machines, qemuCaps->nmachineTypes) < 0)
        goto error;

    for (i = 0; i < qemuCaps->nmachineTypes; i++) {
        virCapsGuestMachinePtr mach;
        if (VIR_ALLOC(mach) < 0)
            goto error;
        (*machines)[i] = mach;
        if (qemuCaps->machineAliases[i]) {
            if (VIR_STRDUP(mach->name, qemuCaps->machineAliases[i]) < 0 ||
                VIR_STRDUP(mach->canonical, qemuCaps->machineTypes[i]) < 0)
                goto error;
        } else {
            if (VIR_STRDUP(mach->name, qemuCaps->machineTypes[i]) < 0)
                goto error;
        }
        mach->maxCpus = qemuCaps->machineMaxCpus[i];
    }

    return 0;

 error:
    virCapabilitiesFreeMachines(*machines, *nmachines);
    *nmachines = 0;
    *machines = NULL;
    return -1;
}




const char *virQEMUCapsGetCanonicalMachine(virQEMUCapsPtr qemuCaps,
                                           const char *name)
{
    size_t i;

    if (!name)
        return NULL;

    for (i = 0; i < qemuCaps->nmachineTypes; i++) {
        if (!qemuCaps->machineAliases[i])
            continue;
        if (STREQ(qemuCaps->machineAliases[i], name))
            return qemuCaps->machineTypes[i];
    }

    return name;
}


int virQEMUCapsGetMachineMaxCpus(virQEMUCapsPtr qemuCaps,
                                 const char *name)
{
    size_t i;

    if (!name)
        return 0;

    for (i = 0; i < qemuCaps->nmachineTypes; i++) {
        if (!qemuCaps->machineMaxCpus[i])
            continue;
        if (STREQ(qemuCaps->machineTypes[i], name))
            return qemuCaps->machineMaxCpus[i];
    }

    return 0;
}


static int
virQEMUCapsProbeQMPCommands(virQEMUCapsPtr qemuCaps,
                            qemuMonitorPtr mon)
{
    char **commands = NULL;
    int ncommands;

    if ((ncommands = qemuMonitorGetCommands(mon, &commands)) < 0)
        return -1;

    virQEMUCapsProcessStringFlags(qemuCaps,
                                  ARRAY_CARDINALITY(virQEMUCapsCommands),
                                  virQEMUCapsCommands,
                                  ncommands, commands);
    virQEMUCapsFreeStringList(ncommands, commands);

    /* QMP add-fd was introduced in 1.2, but did not support
     * management control of set numbering, and did not have a
     * counterpart -add-fd command line option.  We require the
     * add-fd features from 1.3 or later.  */
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_ADD_FD)) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("unable to probe for add-fd"));
            return -1;
        }
        if (qemuMonitorAddFd(mon, 0, fd, "/dev/null") < 0)
            virQEMUCapsClear(qemuCaps, QEMU_CAPS_ADD_FD);
        VIR_FORCE_CLOSE(fd);
    }

    /* Probe for active commit of qemu 2.1 (for now, we are choosing
     * to ignore the fact that qemu 2.0 can also do active commit) */
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_BLOCK_COMMIT) &&
        qemuMonitorSupportsActiveCommit(mon))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_ACTIVE_COMMIT);

    return 0;
}


static int
virQEMUCapsProbeQMPEvents(virQEMUCapsPtr qemuCaps,
                          qemuMonitorPtr mon)
{
    char **events = NULL;
    int nevents;

    if ((nevents = qemuMonitorGetEvents(mon, &events)) < 0)
        return -1;

    virQEMUCapsProcessStringFlags(qemuCaps,
                                  ARRAY_CARDINALITY(virQEMUCapsEvents),
                                  virQEMUCapsEvents,
                                  nevents, events);
    virQEMUCapsFreeStringList(nevents, events);

    return 0;
}


static int
virQEMUCapsProbeQMPObjects(virQEMUCapsPtr qemuCaps,
                           qemuMonitorPtr mon)
{
    int nvalues;
    char **values;
    size_t i;

    if ((nvalues = qemuMonitorGetObjectTypes(mon, &values)) < 0)
        return -1;
    virQEMUCapsProcessStringFlags(qemuCaps,
                                  ARRAY_CARDINALITY(virQEMUCapsObjectTypes),
                                  virQEMUCapsObjectTypes,
                                  nvalues, values);
    virQEMUCapsFreeStringList(nvalues, values);

    for (i = 0; i < ARRAY_CARDINALITY(virQEMUCapsObjectProps); i++) {
        const char *type = virQEMUCapsObjectProps[i].type;
        if ((nvalues = qemuMonitorGetObjectProps(mon,
                                                 type,
                                                 &values)) < 0)
            return -1;
        virQEMUCapsProcessStringFlags(qemuCaps,
                                      virQEMUCapsObjectProps[i].nprops,
                                      virQEMUCapsObjectProps[i].props,
                                      nvalues, values);
        virQEMUCapsFreeStringList(nvalues, values);
    }

    /* Prefer -chardev spicevmc (detected earlier) over -device spicevmc */
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_CHARDEV_SPICEVMC))
        virQEMUCapsClear(qemuCaps, QEMU_CAPS_DEVICE_SPICEVMC);
    /* If qemu supports newer -device qxl it supports -vga qxl as well */
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_QXL))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_VGA_QXL);

    return 0;
}


static int
virQEMUCapsProbeQMPMachineTypes(virQEMUCapsPtr qemuCaps,
                                qemuMonitorPtr mon)
{
    qemuMonitorMachineInfoPtr *machines = NULL;
    int nmachines = 0;
    int ret = -1;
    size_t i;
    size_t defIdx = 0;

    if ((nmachines = qemuMonitorGetMachines(mon, &machines)) < 0)
        return -1;

    if (VIR_ALLOC_N(qemuCaps->machineTypes, nmachines) < 0)
        goto cleanup;
    if (VIR_ALLOC_N(qemuCaps->machineAliases, nmachines) < 0)
        goto cleanup;
    if (VIR_ALLOC_N(qemuCaps->machineMaxCpus, nmachines) < 0)
        goto cleanup;

    for (i = 0; i < nmachines; i++) {
        if (STREQ(machines[i]->name, "none"))
            continue;
        qemuCaps->nmachineTypes++;
        if (VIR_STRDUP(qemuCaps->machineAliases[qemuCaps->nmachineTypes -1],
                       machines[i]->alias) < 0 ||
            VIR_STRDUP(qemuCaps->machineTypes[qemuCaps->nmachineTypes - 1],
                       machines[i]->name) < 0)
            goto cleanup;
        if (machines[i]->isDefault)
            defIdx = qemuCaps->nmachineTypes - 1;
        qemuCaps->machineMaxCpus[qemuCaps->nmachineTypes - 1] =
            machines[i]->maxCpus;
    }

    if (defIdx)
        virQEMUCapsSetDefaultMachine(qemuCaps, defIdx);

    ret = 0;

 cleanup:
    for (i = 0; i < nmachines; i++)
        qemuMonitorMachineInfoFree(machines[i]);
    VIR_FREE(machines);
    return ret;
}


static int
virQEMUCapsProbeQMPCPUDefinitions(virQEMUCapsPtr qemuCaps,
                                  qemuMonitorPtr mon)
{
    int ncpuDefinitions;
    char **cpuDefinitions;

    if ((ncpuDefinitions = qemuMonitorGetCPUDefinitions(mon, &cpuDefinitions)) < 0)
        return -1;

    qemuCaps->ncpuDefinitions = ncpuDefinitions;
    qemuCaps->cpuDefinitions = cpuDefinitions;

    return 0;
}

struct tpmTypeToCaps {
    int type;
    virQEMUCapsFlags caps;
};

static const struct tpmTypeToCaps virQEMUCapsTPMTypesToCaps[] = {
    {
        .type = VIR_DOMAIN_TPM_TYPE_PASSTHROUGH,
        .caps = QEMU_CAPS_DEVICE_TPM_PASSTHROUGH,
    },
};

const struct tpmTypeToCaps virQEMUCapsTPMModelsToCaps[] = {
    {
        .type = VIR_DOMAIN_TPM_MODEL_TIS,
        .caps = QEMU_CAPS_DEVICE_TPM_TIS,
    },
};

static int
virQEMUCapsProbeQMPTPM(virQEMUCapsPtr qemuCaps,
                       qemuMonitorPtr mon)
{
    int nentries;
    size_t i;
    char **entries = NULL;

    if ((nentries = qemuMonitorGetTPMModels(mon, &entries)) < 0)
        return -1;

    if (nentries > 0) {
        for (i = 0; i < ARRAY_CARDINALITY(virQEMUCapsTPMModelsToCaps); i++) {
            const char *needle = virDomainTPMModelTypeToString(
                virQEMUCapsTPMModelsToCaps[i].type);
            if (virStringArrayHasString(entries, needle))
                virQEMUCapsSet(qemuCaps,
                               virQEMUCapsTPMModelsToCaps[i].caps);
        }
    }
    virStringFreeList(entries);

    if ((nentries = qemuMonitorGetTPMTypes(mon, &entries)) < 0)
        return -1;

    if (nentries > 0) {
        for (i = 0; i < ARRAY_CARDINALITY(virQEMUCapsTPMTypesToCaps); i++) {
            const char *needle = virDomainTPMBackendTypeToString(
                virQEMUCapsTPMTypesToCaps[i].type);
            if (virStringArrayHasString(entries, needle))
                virQEMUCapsSet(qemuCaps, virQEMUCapsTPMTypesToCaps[i].caps);
        }
    }
    virStringFreeList(entries);

    return 0;
}


static int
virQEMUCapsProbeQMPKVMState(virQEMUCapsPtr qemuCaps,
                            qemuMonitorPtr mon)
{
    bool enabled = false;
    bool present = false;

    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_KVM))
        return 0;

    if (qemuMonitorGetKVMState(mon, &enabled, &present) < 0)
        return -1;

    /* The QEMU_CAPS_KVM flag was initially set according to the QEMU
     * reporting the recognition of 'query-kvm' QMP command. That merely
     * indicates existence of the command though, not whether KVM support
     * is actually available, nor whether it is enabled by default.
     *
     * If it is not present we need to clear the flag, and if it is
     * not enabled by default we need to change the flag.
     */
    if (!present) {
        virQEMUCapsClear(qemuCaps, QEMU_CAPS_KVM);
    } else if (!enabled) {
        virQEMUCapsClear(qemuCaps, QEMU_CAPS_KVM);
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_ENABLE_KVM);
    }

    return 0;
}

struct virQEMUCapsCommandLineProps {
    const char *option;
    const char *param;
    int flag;
};

static struct virQEMUCapsCommandLineProps virQEMUCapsCommandLine[] = {
    { "machine", "mem-merge", QEMU_CAPS_MEM_MERGE },
    { "drive", "discard", QEMU_CAPS_DRIVE_DISCARD },
    { "realtime", "mlock", QEMU_CAPS_MLOCK },
    { "boot-opts", "strict", QEMU_CAPS_BOOT_STRICT },
    { "boot-opts", "reboot-timeout", QEMU_CAPS_REBOOT_TIMEOUT },
    { "boot-opts", "splash-time", QEMU_CAPS_SPLASH_TIMEOUT },
    { "spice", "disable-agent-file-xfer", QEMU_CAPS_SPICE_FILE_XFER_DISABLE },
    { "msg", "timestamp", QEMU_CAPS_MSG_TIMESTAMP },
    { "numa", NULL, QEMU_CAPS_NUMA },
    { "drive", "throttling.bps-total-max", QEMU_CAPS_DRIVE_IOTUNE_MAX},
};

static int
virQEMUCapsProbeQMPCommandLine(virQEMUCapsPtr qemuCaps,
                               qemuMonitorPtr mon)
{
    bool found = false;
    int nvalues;
    char **values;
    size_t i, j;

    for (i = 0; i < ARRAY_CARDINALITY(virQEMUCapsCommandLine); i++) {
        if ((nvalues = qemuMonitorGetCommandLineOptionParameters(mon,
                                                                 virQEMUCapsCommandLine[i].option,
                                                                 &values,
                                                                 &found)) < 0)
            return -1;

        if (found && !virQEMUCapsCommandLine[i].param)
            virQEMUCapsSet(qemuCaps, virQEMUCapsCommandLine[i].flag);

        for (j = 0; j < nvalues; j++) {
            if (STREQ_NULLABLE(virQEMUCapsCommandLine[i].param, values[j])) {
                virQEMUCapsSet(qemuCaps, virQEMUCapsCommandLine[i].flag);
                break;
            }
        }
        virStringFreeList(values);
    }

    return 0;
}

static int
virQEMUCapsProbeQMPMigrationCapabilities(virQEMUCapsPtr qemuCaps,
                                         qemuMonitorPtr mon)
{
    char **caps = NULL;
    int ncaps;

    if ((ncaps = qemuMonitorGetMigrationCapabilities(mon, &caps)) < 0)
        return -1;

    virQEMUCapsProcessStringFlags(qemuCaps,
                                  ARRAY_CARDINALITY(virQEMUCapsMigration),
                                  virQEMUCapsMigration,
                                  ncaps, caps);
    virQEMUCapsFreeStringList(ncaps, caps);

    return 0;
}

int virQEMUCapsProbeQMP(virQEMUCapsPtr qemuCaps,
                        qemuMonitorPtr mon)
{
    VIR_DEBUG("qemuCaps=%p mon=%p", qemuCaps, mon);

    if (qemuCaps->usedQMP)
        return 0;

    if (virQEMUCapsProbeQMPCommands(qemuCaps, mon) < 0)
        return -1;

    if (virQEMUCapsProbeQMPEvents(qemuCaps, mon) < 0)
        return -1;

    return 0;
}


/*
 * Parsing a doc that looks like
 *
 * <qemuCaps>
 *   <qemuctime>234235253</qemuctime>
 *   <selfctime>234235253</selfctime>
 *   <usedQMP/>
 *   <flag name='foo'/>
 *   <flag name='bar'/>
 *   ...
 *   <cpu name="pentium3"/>
 *   ...
 *   <machine name="pc-1.0" alias="pc" maxCpus="4"/>
 *   ...
 * </qemuCaps>
 */
static int
virQEMUCapsLoadCache(virQEMUCapsPtr qemuCaps, const char *filename,
                     time_t *qemuctime, time_t *selfctime)
{
    xmlDocPtr doc = NULL;
    int ret = -1;
    size_t i;
    int n;
    xmlNodePtr *nodes = NULL;
    xmlXPathContextPtr ctxt = NULL;
    char *str = NULL;
    long long int l;

    if (!(doc = virXMLParseFile(filename)))
        goto cleanup;

    if (!(ctxt = xmlXPathNewContext(doc))) {
        virReportOOMError();
        goto cleanup;
    }

    ctxt->node = xmlDocGetRootElement(doc);

    if (STRNEQ((const char *)ctxt->node->name, "qemuCaps")) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("unexpected root element <%s>, "
                         "expecting <qemuCaps>"),
                       ctxt->node->name);
        goto cleanup;
    }

    if (virXPathLongLong("string(./qemuctime)", ctxt, &l) < 0) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("missing qemuctime in QEMU capabilities XML"));
        goto cleanup;
    }
    *qemuctime = (time_t)l;

    if (virXPathLongLong("string(./selfctime)", ctxt, &l) < 0) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("missing selfctime in QEMU capabilities XML"));
        goto cleanup;
    }
    *selfctime = (time_t)l;

    qemuCaps->usedQMP = virXPathBoolean("count(./usedQMP) > 0",
                                        ctxt) > 0;

    if ((n = virXPathNodeSet("./flag", ctxt, &nodes)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("failed to parse qemu capabilities flags"));
        goto cleanup;
    }
    VIR_DEBUG("Got flags %d", n);
    if (n > 0) {
        for (i = 0; i < n; i++) {
            int flag;
            if (!(str = virXMLPropString(nodes[i], "name"))) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("missing flag name in QEMU capabilities cache"));
                goto cleanup;
            }
            flag = virQEMUCapsTypeFromString(str);
            if (flag < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Unknown qemu capabilities flag %s"), str);
                goto cleanup;
            }
            VIR_FREE(str);
            virQEMUCapsSet(qemuCaps, flag);
        }
    }
    VIR_FREE(nodes);

    if (virXPathUInt("string(./version)", ctxt, &qemuCaps->version) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("missing version in QEMU capabilities cache"));
        goto cleanup;
    }

    if (virXPathUInt("string(./kvmVersion)", ctxt, &qemuCaps->kvmVersion) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("missing version in QEMU capabilities cache"));
        goto cleanup;
    }

    if (!(str = virXPathString("string(./arch)", ctxt))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("missing arch in QEMU capabilities cache"));
        goto cleanup;
    }
    if (!(qemuCaps->arch = virArchFromString(str))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown arch %s in QEMU capabilities cache"), str);
        goto cleanup;
    }
    VIR_FREE(str);

    if ((n = virXPathNodeSet("./cpu", ctxt, &nodes)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("failed to parse qemu capabilities cpus"));
        goto cleanup;
    }
    if (n > 0) {
        qemuCaps->ncpuDefinitions = n;
        if (VIR_ALLOC_N(qemuCaps->cpuDefinitions,
                        qemuCaps->ncpuDefinitions) < 0)
            goto cleanup;

        for (i = 0; i < n; i++) {
            if (!(qemuCaps->cpuDefinitions[i] = virXMLPropString(nodes[i], "name"))) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("missing cpu name in QEMU capabilities cache"));
                goto cleanup;
            }
        }
    }
    VIR_FREE(nodes);


    if ((n = virXPathNodeSet("./machine", ctxt, &nodes)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("failed to parse qemu capabilities machines"));
        goto cleanup;
    }
    if (n > 0) {
        qemuCaps->nmachineTypes = n;
        if (VIR_ALLOC_N(qemuCaps->machineTypes,
                        qemuCaps->nmachineTypes) < 0 ||
            VIR_ALLOC_N(qemuCaps->machineAliases,
                        qemuCaps->nmachineTypes) < 0 ||
            VIR_ALLOC_N(qemuCaps->machineMaxCpus,
                        qemuCaps->nmachineTypes) < 0)
            goto cleanup;

        for (i = 0; i < n; i++) {
            if (!(qemuCaps->machineTypes[i] = virXMLPropString(nodes[i], "name"))) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("missing machine name in QEMU capabilities cache"));
                goto cleanup;
            }
            qemuCaps->machineAliases[i] = virXMLPropString(nodes[i], "alias");

            str = virXMLPropString(nodes[i], "maxCpus");
            if (str &&
                virStrToLong_ui(str, NULL, 10, &(qemuCaps->machineMaxCpus[i])) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("malformed machine cpu count in QEMU capabilities cache"));
                goto cleanup;
            }
            VIR_FREE(str);
        }
    }
    VIR_FREE(nodes);

    ret = 0;
 cleanup:
    VIR_FREE(str);
    VIR_FREE(nodes);
    xmlXPathFreeContext(ctxt);
    xmlFreeDoc(doc);
    return ret;
}


static int
virQEMUCapsSaveCache(virQEMUCapsPtr qemuCaps, const char *filename)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *xml = NULL;
    int ret = -1;
    size_t i;

    virBufferAddLit(&buf, "<qemuCaps>\n");
    virBufferAdjustIndent(&buf, 2);

    virBufferAsprintf(&buf, "<qemuctime>%llu</qemuctime>\n",
                      (long long)qemuCaps->ctime);
    virBufferAsprintf(&buf, "<selfctime>%llu</selfctime>\n",
                      (long long)virGetSelfLastChanged());

    if (qemuCaps->usedQMP)
        virBufferAddLit(&buf, "<usedQMP/>\n");

    for (i = 0; i < QEMU_CAPS_LAST; i++) {
        if (virQEMUCapsGet(qemuCaps, i)) {
            virBufferAsprintf(&buf, "<flag name='%s'/>\n",
                              virQEMUCapsTypeToString(i));
        }
    }

    virBufferAsprintf(&buf, "<version>%d</version>\n",
                      qemuCaps->version);

    virBufferAsprintf(&buf, "<kvmVersion>%d</kvmVersion>\n",
                      qemuCaps->kvmVersion);

    virBufferAsprintf(&buf, "<arch>%s</arch>\n",
                      virArchToString(qemuCaps->arch));

    for (i = 0; i < qemuCaps->ncpuDefinitions; i++) {
        virBufferEscapeString(&buf, "<cpu name='%s'/>\n",
                              qemuCaps->cpuDefinitions[i]);
    }

    for (i = 0; i < qemuCaps->nmachineTypes; i++) {
        virBufferEscapeString(&buf, "<machine name='%s'",
                              qemuCaps->machineTypes[i]);
        if (qemuCaps->machineAliases[i])
            virBufferEscapeString(&buf, " alias='%s'",
                              qemuCaps->machineAliases[i]);
        virBufferAsprintf(&buf, " maxCpus='%u'/>\n",
                          qemuCaps->machineMaxCpus[i]);
    }

    virBufferAdjustIndent(&buf, -2);
    virBufferAddLit(&buf, "</qemuCaps>\n");

    if (virBufferCheckError(&buf) < 0)
        goto cleanup;

    xml = virBufferContentAndReset(&buf);

    if (virFileWriteStr(filename, xml, 0600) < 0) {
        virReportSystemError(errno,
                             _("Failed to save '%s' for '%s'"),
                             filename, qemuCaps->binary);
        goto cleanup;
    }

    VIR_DEBUG("Saved caps '%s' for '%s' with (%lld, %lld)",
              filename, qemuCaps->binary,
              (long long)qemuCaps->ctime,
              (long long)virGetSelfLastChanged());

    ret = 0;
 cleanup:
    VIR_FREE(xml);
    return ret;
}

static int
virQEMUCapsRememberCached(virQEMUCapsPtr qemuCaps, const char *cacheDir)
{
    char *capsdir = NULL;
    char *capsfile = NULL;
    int ret = -1;
    char *binaryhash = NULL;

    if (virAsprintf(&capsdir, "%s/capabilities", cacheDir) < 0)
        goto cleanup;

    if (virCryptoHashString(VIR_CRYPTO_HASH_SHA256,
                            qemuCaps->binary,
                            &binaryhash) < 0)
        goto cleanup;

    if (virAsprintf(&capsfile, "%s/%s.xml", capsdir, binaryhash) < 0)
        goto cleanup;

    if (virFileMakePath(capsdir) < 0) {
        virReportSystemError(errno,
                             _("Unable to create directory '%s'"),
                             capsdir);
        goto cleanup;
    }

    if (virQEMUCapsSaveCache(qemuCaps, capsfile) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    VIR_FREE(binaryhash);
    VIR_FREE(capsfile);
    VIR_FREE(capsdir);
    return ret;
}


static void
virQEMUCapsReset(virQEMUCapsPtr qemuCaps)
{
    size_t i;

    virBitmapClearAll(qemuCaps->flags);
    qemuCaps->version = qemuCaps->kvmVersion = 0;
    qemuCaps->arch = VIR_ARCH_NONE;
    qemuCaps->usedQMP = false;

    for (i = 0; i < qemuCaps->ncpuDefinitions; i++)
        VIR_FREE(qemuCaps->cpuDefinitions[i]);
    VIR_FREE(qemuCaps->cpuDefinitions);
    qemuCaps->ncpuDefinitions = 0;

    for (i = 0; i < qemuCaps->nmachineTypes; i++) {
        VIR_FREE(qemuCaps->machineTypes[i]);
        VIR_FREE(qemuCaps->machineAliases[i]);
    }
    VIR_FREE(qemuCaps->machineTypes);
    VIR_FREE(qemuCaps->machineAliases);
    VIR_FREE(qemuCaps->machineMaxCpus);
    qemuCaps->nmachineTypes = 0;
}


static int
virQEMUCapsInitCached(virQEMUCapsPtr qemuCaps, const char *cacheDir)
{
    char *capsdir = NULL;
    char *capsfile = NULL;
    int ret = -1;
    char *binaryhash = NULL;
    struct stat sb;
    time_t qemuctime;
    time_t selfctime;

    if (virAsprintf(&capsdir, "%s/capabilities", cacheDir) < 0)
        goto cleanup;

    if (virCryptoHashString(VIR_CRYPTO_HASH_SHA256,
                            qemuCaps->binary,
                            &binaryhash) < 0)
        goto cleanup;

    if (virAsprintf(&capsfile, "%s/%s.xml", capsdir, binaryhash) < 0)
        goto cleanup;

    if (virFileMakePath(capsdir) < 0) {
        virReportSystemError(errno,
                             _("Unable to create directory '%s'"),
                             capsdir);
        goto cleanup;
    }

    if (stat(capsfile, &sb) < 0) {
        if (errno == ENOENT) {
            VIR_DEBUG("No cached capabilities '%s' for '%s'",
                      capsfile, qemuCaps->binary);
            ret = 0;
            goto cleanup;
        }
        virReportSystemError(errno,
                             _("Unable to access cache '%s' for '%s'"),
                             capsfile, qemuCaps->binary);
        goto cleanup;
    }

    if (virQEMUCapsLoadCache(qemuCaps, capsfile, &qemuctime, &selfctime) < 0) {
        virErrorPtr err = virGetLastError();
        VIR_WARN("Failed to load cached caps from '%s' for '%s': %s",
                 capsfile, qemuCaps->binary, err ? NULLSTR(err->message) :
                 _("unknown error"));
        virResetLastError();
        ret = 0;
        virQEMUCapsReset(qemuCaps);
        goto cleanup;
    }

    /* Discard if cache is older that QEMU binary */
    if (qemuctime != qemuCaps->ctime ||
        selfctime < virGetSelfLastChanged()) {
        VIR_DEBUG("Outdated cached capabilities '%s' for '%s' "
                  "(%lld vs %lld, %lld vs %lld)",
                  capsfile, qemuCaps->binary,
                  (long long)qemuctime, (long long)qemuCaps->ctime,
                  (long long)selfctime, (long long)virGetSelfLastChanged());
        ignore_value(unlink(capsfile));
        virQEMUCapsReset(qemuCaps);
        ret = 0;
        goto cleanup;
    }

    VIR_DEBUG("Loaded '%s' for '%s' ctime %lld usedQMP=%d",
              capsfile, qemuCaps->binary,
              (long long)qemuCaps->ctime, qemuCaps->usedQMP);

    ret = 1;
 cleanup:
    VIR_FREE(binaryhash);
    VIR_FREE(capsfile);
    VIR_FREE(capsdir);
    return ret;
}


#define QEMU_SYSTEM_PREFIX "qemu-system-"

static int
virQEMUCapsInitHelp(virQEMUCapsPtr qemuCaps, uid_t runUid, gid_t runGid, const char *qmperr)
{
    virCommandPtr cmd = NULL;
    bool is_kvm;
    char *help = NULL;
    int ret = -1;
    const char *tmp;

    VIR_DEBUG("qemuCaps=%p", qemuCaps);

    tmp = strstr(qemuCaps->binary, QEMU_SYSTEM_PREFIX);
    if (tmp) {
        tmp += strlen(QEMU_SYSTEM_PREFIX);

        qemuCaps->arch = virQEMUCapsArchFromString(tmp);
    } else {
        qemuCaps->arch = virArchFromHost();
    }

    cmd = virQEMUCapsProbeCommand(qemuCaps->binary, NULL, runUid, runGid);
    virCommandAddArgList(cmd, "-help", NULL);
    virCommandSetOutputBuffer(cmd, &help);

    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    if (virQEMUCapsParseHelpStr(qemuCaps->binary,
                                help, qemuCaps,
                                &qemuCaps->version,
                                &is_kvm,
                                &qemuCaps->kvmVersion,
                                false,
                                qmperr) < 0)
        goto cleanup;

    /* x86_64 and i686 support PCI-multibus on all machine types
     * since forever. For other architectures, it has been changing
     * across releases, per machine type, so we can't simply detect
     * it here. Thus the rest of the logic is provided in a separate
     * helper virQEMUCapsHasPCIMultiBus() which keys off the machine
     * stored in virDomainDef and QEMU version number
     */
    if (qemuCaps->arch == VIR_ARCH_X86_64 ||
        qemuCaps->arch == VIR_ARCH_I686)
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_PCI_MULTIBUS);

    /* -no-acpi is not supported on non-x86
     * even if qemu reports it in -help */
    if (qemuCaps->arch != VIR_ARCH_X86_64 &&
        qemuCaps->arch != VIR_ARCH_I686)
        virQEMUCapsClear(qemuCaps, QEMU_CAPS_NO_ACPI);

    /* virQEMUCapsExtractDeviceStr will only set additional caps if qemu
     * understands the 0.13.0+ notion of "-device driver,".  */
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE) &&
        strstr(help, "-device driver,?") &&
        virQEMUCapsExtractDeviceStr(qemuCaps->binary,
                                    qemuCaps, runUid, runGid) < 0) {
        goto cleanup;
    }

    if (virQEMUCapsProbeCPUModels(qemuCaps, runUid, runGid) < 0)
        goto cleanup;

    if (virQEMUCapsProbeMachineTypes(qemuCaps, runUid, runGid) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virCommandFree(cmd);
    VIR_FREE(help);
    return ret;
}


static void virQEMUCapsMonitorNotify(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                                     virDomainObjPtr vm ATTRIBUTE_UNUSED,
                                     void *opaque ATTRIBUTE_UNUSED)
{
}

static qemuMonitorCallbacks callbacks = {
    .eofNotify = virQEMUCapsMonitorNotify,
    .errorNotify = virQEMUCapsMonitorNotify,
};


/* Capabilities that we assume are always enabled
 * for QEMU >= 1.2.0
 */
static void
virQEMUCapsInitQMPBasic(virQEMUCapsPtr qemuCaps)
{
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_VNC_COLON);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_REBOOT);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_NAME);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_UUID);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_VNET_HDR);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_TCP);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_EXEC);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_CACHE_V2);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_FORMAT);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_VGA);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_0_10);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_MEM_PATH);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_SERIAL);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_UNIX);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_CHARDEV);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_MONITOR_JSON);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_BALLOON);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DEVICE);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_SDL);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_SMP_TOPOLOGY);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_NETDEV);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_RTC);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_VHOST_NET);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_NODEFCONFIG);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_BOOT_MENU);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_FSDEV);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_NAME_PROCESS);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_READONLY);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_SMBIOS_TYPE);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_VGA_NONE);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_FD);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_AIO);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_CHARDEV_SPICEVMC);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DEVICE_QXL_VGA);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_CACHE_DIRECTSYNC);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_SHUTDOWN);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_CACHE_UNSAFE);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_FSDEV_READONLY);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_VIRTIO_BLK_SG_IO);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_COPY_ON_READ);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_CPU_HOST);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_FSDEV_WRITEOUT);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_IOTUNE);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_WAKEUP);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_USER_CONFIG);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_NETDEV_BRIDGE);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_SECCOMP_SANDBOX);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DTB);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_IPV6_MIGRATION);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_MACHINE_OPT);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DUMP_GUEST_CORE);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_VNC_SHARE_POLICY);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_HOST_PCI_MULTIDOMAIN);
}

/* Capabilities that are architecture depending
 * initialized for QEMU.
 */
static int
virQEMUCapsInitArchQMPBasic(virQEMUCapsPtr qemuCaps,
                            qemuMonitorPtr mon)
{
    char *archstr = NULL;
    int ret = -1;

    if (!(archstr = qemuMonitorGetTargetArch(mon)))
        return -1;

    if ((qemuCaps->arch = virQEMUCapsArchFromString(archstr)) == VIR_ARCH_NONE) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unknown QEMU arch %s"), archstr);
        goto cleanup;
    }

    /* x86_64 and i686 support PCI-multibus on all machine types
     * since forever. For other architectures, it has been changing
     * across releases, per machine type, so we can't simply detect
     * it here. Thus the rest of the logic is provided in a separate
     * helper virQEMUCapsHasPCIMultiBus() which keys off the machine
     * stored in virDomainDef and QEMU version number
     *
     * ACPI/HPET/KVM PIT are also x86 specific
     */
    if (qemuCaps->arch == VIR_ARCH_X86_64 ||
        qemuCaps->arch == VIR_ARCH_I686) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_PCI_MULTIBUS);
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_ACPI);
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_HPET);
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_KVM_PIT);
    }

    ret = 0;

 cleanup:
    VIR_FREE(archstr);
    return ret;
}

int
virQEMUCapsInitQMPMonitor(virQEMUCapsPtr qemuCaps,
                          qemuMonitorPtr mon)
{
    int ret = -1;
    int major, minor, micro;
    char *package = NULL;

    /* @mon is supposed to be locked by callee */

    if (qemuMonitorSetCapabilities(mon) < 0) {
        virErrorPtr err = virGetLastError();
        VIR_DEBUG("Failed to set monitor capabilities %s",
                  err ? err->message : "<unknown problem>");
        ret = 0;
        goto cleanup;
    }

    if (qemuMonitorGetVersion(mon,
                              &major, &minor, &micro,
                              &package) < 0) {
        virErrorPtr err = virGetLastError();
        VIR_DEBUG("Failed to query monitor version %s",
                  err ? err->message : "<unknown problem>");
        ret = 0;
        goto cleanup;
    }

    VIR_DEBUG("Got version %d.%d.%d (%s)",
              major, minor, micro, NULLSTR(package));

    if (major < 1 || (major == 1 && minor < 2)) {
        VIR_DEBUG("Not new enough for QMP capabilities detection");
        ret = 0;
        goto cleanup;
    }

    qemuCaps->version = major * 1000000 + minor * 1000 + micro;
    qemuCaps->usedQMP = true;

    virQEMUCapsInitQMPBasic(qemuCaps);

    if (virQEMUCapsInitArchQMPBasic(qemuCaps, mon) < 0)
        goto cleanup;

    /* USB option is supported v1.3.0 onwards */
    if (qemuCaps->version >= 1003000)
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_MACHINE_USB_OPT);

    /* WebSockets were introduced between 1.3.0 and 1.3.1 */
    if (qemuCaps->version >= 1003001)
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_VNC_WEBSOCKET);

    /* -chardev spiceport is supported from 1.4.0, but usable through
     * qapi only since 1.5.0, however, it still cannot be queried
     * for as a capability */
    if (qemuCaps->version >= 1005000)
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_CHARDEV_SPICEPORT);

    if (qemuCaps->version >= 1006000)
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_DEVICE_VIDEO_PRIMARY);

    if (virQEMUCapsProbeQMPCommands(qemuCaps, mon) < 0)
        goto cleanup;
    if (virQEMUCapsProbeQMPEvents(qemuCaps, mon) < 0)
        goto cleanup;
    if (virQEMUCapsProbeQMPObjects(qemuCaps, mon) < 0)
        goto cleanup;
    if (virQEMUCapsProbeQMPMachineTypes(qemuCaps, mon) < 0)
        goto cleanup;
    if (virQEMUCapsProbeQMPCPUDefinitions(qemuCaps, mon) < 0)
        goto cleanup;
    if (virQEMUCapsProbeQMPKVMState(qemuCaps, mon) < 0)
        goto cleanup;
    if (virQEMUCapsProbeQMPTPM(qemuCaps, mon) < 0)
        goto cleanup;
    if (virQEMUCapsProbeQMPCommandLine(qemuCaps, mon) < 0)
        goto cleanup;
    if (virQEMUCapsProbeQMPMigrationCapabilities(qemuCaps, mon) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    VIR_FREE(package);
    return ret;
}

static int
virQEMUCapsInitQMP(virQEMUCapsPtr qemuCaps,
                   const char *libDir,
                   uid_t runUid,
                   gid_t runGid,
                   char **qmperr)
{
    int ret = -1;
    virCommandPtr cmd = NULL;
    qemuMonitorPtr mon = NULL;
    int status = 0;
    virDomainChrSourceDef config;
    char *monarg = NULL;
    char *monpath = NULL;
    char *pidfile = NULL;
    pid_t pid = 0;
    virDomainObjPtr vm = NULL;
    virDomainXMLOptionPtr xmlopt = NULL;

    /* the ".sock" sufix is important to avoid a possible clash with a qemu
     * domain called "capabilities"
     */
    if (virAsprintf(&monpath, "%s/%s", libDir, "capabilities.monitor.sock") < 0)
        goto cleanup;
    if (virAsprintf(&monarg, "unix:%s,server,nowait", monpath) < 0)
        goto cleanup;

    /* ".pidfile" suffix is used rather than ".pid" to avoid a possible clash
     * with a qemu domain called "capabilities"
     * Normally we'd use runDir for pid files, but because we're using
     * -daemonize we need QEMU to be allowed to create them, rather
     * than libvirtd. So we're using libDir which QEMU can write to
     */
    if (virAsprintf(&pidfile, "%s/%s", libDir, "capabilities.pidfile") < 0)
        goto cleanup;

    memset(&config, 0, sizeof(config));
    config.type = VIR_DOMAIN_CHR_TYPE_UNIX;
    config.data.nix.path = monpath;
    config.data.nix.listen = false;

    virPidFileForceCleanupPath(pidfile);

    VIR_DEBUG("Try to get caps via QMP qemuCaps=%p", qemuCaps);

    /*
     * We explicitly need to use -daemonize here, rather than
     * virCommandDaemonize, because we need to synchronize
     * with QEMU creating its monitor socket API. Using
     * daemonize guarantees control won't return to libvirt
     * until the socket is present.
     */
    cmd = virCommandNewArgList(qemuCaps->binary,
                               "-S",
                               "-no-user-config",
                               "-nodefaults",
                               "-nographic",
                               "-M", "none",
                               "-qmp", monarg,
                               "-pidfile", pidfile,
                               "-daemonize",
                               NULL);
    virCommandAddEnvPassCommon(cmd);
    virCommandClearCaps(cmd);
    virCommandSetGID(cmd, runGid);
    virCommandSetUID(cmd, runUid);

    virCommandSetErrorBuffer(cmd, qmperr);

    /* Log, but otherwise ignore, non-zero status.  */
    if (virCommandRun(cmd, &status) < 0)
        goto cleanup;

    if (status != 0) {
        ret = 0;
        VIR_DEBUG("QEMU %s exited with status %d: %s",
                  qemuCaps->binary, status, *qmperr);
        goto cleanup;
    }

    if (virPidFileReadPath(pidfile, &pid) < 0) {
        VIR_DEBUG("Failed to read pidfile %s", pidfile);
        ret = 0;
        goto cleanup;
    }

    if (!(xmlopt = virDomainXMLOptionNew(NULL, NULL, NULL)) ||
        !(vm = virDomainObjNew(xmlopt)))
        goto cleanup;

    vm->pid = pid;

    if (!(mon = qemuMonitorOpen(vm, &config, true, &callbacks, NULL))) {
        ret = 0;
        goto cleanup;
    }

    virObjectLock(mon);

    if (virQEMUCapsInitQMPMonitor(qemuCaps, mon) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    if (mon)
        virObjectUnlock(mon);
    qemuMonitorClose(mon);
    virCommandAbort(cmd);
    virCommandFree(cmd);
    VIR_FREE(monarg);
    if (monpath)
        ignore_value(unlink(monpath));
    VIR_FREE(monpath);
    virObjectUnref(vm);
    virObjectUnref(xmlopt);

    if (pid != 0) {
        char ebuf[1024];

        VIR_DEBUG("Killing QMP caps process %lld", (long long) pid);
        if (virProcessKill(pid, SIGKILL) < 0 && errno != ESRCH)
            VIR_ERROR(_("Failed to kill process %lld: %s"),
                      (long long) pid,
                      virStrerror(errno, ebuf, sizeof(ebuf)));

        VIR_FREE(*qmperr);
    }
    if (pidfile) {
        unlink(pidfile);
        VIR_FREE(pidfile);
    }
    return ret;
}


#define MESSAGE_ID_CAPS_PROBE_FAILURE "8ae2f3fb-2dbe-498e-8fbd-012d40afa361"

static void
virQEMUCapsLogProbeFailure(const char *binary)
{
    virLogMetadata meta[] = {
        { .key = "MESSAGE_ID", .s = MESSAGE_ID_CAPS_PROBE_FAILURE, .iv = 0 },
        { .key = "LIBVIRT_QEMU_BINARY", .s = binary, .iv = 0 },
        { .key = NULL },
    };
    virErrorPtr err = virGetLastError();

    virLogMessage(&virLogSelf,
                  VIR_LOG_WARN,
                  __FILE__, __LINE__, __func__,
                  meta,
                  _("Failed to probe capabilities for %s: %s"),
                  binary, err && err->message ? err->message :
                  _("unknown failure"));
}


virQEMUCapsPtr virQEMUCapsNewForBinary(const char *binary,
                                       const char *libDir,
                                       const char *cacheDir,
                                       uid_t runUid,
                                       gid_t runGid)
{
    virQEMUCapsPtr qemuCaps;
    struct stat sb;
    int rv;
    char *qmperr = NULL;

    if (!(qemuCaps = virQEMUCapsNew()))
        goto error;

    if (VIR_STRDUP(qemuCaps->binary, binary) < 0)
        goto error;

    /* We would also want to check faccessat if we cared about ACLs,
     * but we don't.  */
    if (stat(binary, &sb) < 0) {
        virReportSystemError(errno, _("Cannot check QEMU binary %s"),
                             binary);
        goto error;
    }
    qemuCaps->ctime = sb.st_ctime;

    /* Make sure the binary we are about to try exec'ing exists.
     * Technically we could catch the exec() failure, but that's
     * in a sub-process so it's hard to feed back a useful error.
     */
    if (!virFileIsExecutable(binary)) {
        virReportSystemError(errno, _("QEMU binary %s is not executable"),
                             binary);
        goto error;
    }

    if ((rv = virQEMUCapsInitCached(qemuCaps, cacheDir)) < 0)
        goto error;

    if (rv == 0) {
        if (virQEMUCapsInitQMP(qemuCaps, libDir, runUid, runGid, &qmperr) < 0) {
            virQEMUCapsLogProbeFailure(binary);
            goto error;
        }

        if (!qemuCaps->usedQMP &&
            virQEMUCapsInitHelp(qemuCaps, runUid, runGid, qmperr) < 0) {
            virQEMUCapsLogProbeFailure(binary);
            goto error;
        }

        if (virQEMUCapsRememberCached(qemuCaps, cacheDir) < 0)
            goto error;
    }

    VIR_FREE(qmperr);
    return qemuCaps;

 error:
    VIR_FREE(qmperr);
    virObjectUnref(qemuCaps);
    qemuCaps = NULL;
    return NULL;
}


bool virQEMUCapsIsValid(virQEMUCapsPtr qemuCaps)
{
    struct stat sb;

    if (!qemuCaps->binary)
        return true;

    if (stat(qemuCaps->binary, &sb) < 0)
        return false;

    return sb.st_ctime == qemuCaps->ctime;
}


virQEMUCapsCachePtr
virQEMUCapsCacheNew(const char *libDir,
                    const char *cacheDir,
                    uid_t runUid,
                    gid_t runGid)
{
    virQEMUCapsCachePtr cache;

    if (VIR_ALLOC(cache) < 0)
        return NULL;

    if (virMutexInit(&cache->lock) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Unable to initialize mutex"));
        VIR_FREE(cache);
        return NULL;
    }

    if (!(cache->binaries = virHashCreate(10, virObjectFreeHashData)))
        goto error;
    if (VIR_STRDUP(cache->libDir, libDir) < 0)
        goto error;
    if (VIR_STRDUP(cache->cacheDir, cacheDir) < 0)
        goto error;

    cache->runUid = runUid;
    cache->runGid = runGid;

    return cache;

 error:
    virQEMUCapsCacheFree(cache);
    return NULL;
}


virQEMUCapsPtr
virQEMUCapsCacheLookup(virQEMUCapsCachePtr cache, const char *binary)
{
    virQEMUCapsPtr ret = NULL;
    virMutexLock(&cache->lock);
    ret = virHashLookup(cache->binaries, binary);
    if (ret &&
        !virQEMUCapsIsValid(ret)) {
        VIR_DEBUG("Cached capabilities %p no longer valid for %s",
                  ret, binary);
        virHashRemoveEntry(cache->binaries, binary);
        ret = NULL;
    }
    if (!ret) {
        VIR_DEBUG("Creating capabilities for %s",
                  binary);
        ret = virQEMUCapsNewForBinary(binary, cache->libDir,
                                      cache->cacheDir,
                                      cache->runUid, cache->runGid);
        if (ret) {
            VIR_DEBUG("Caching capabilities %p for %s",
                      ret, binary);
            if (virHashAddEntry(cache->binaries, binary, ret) < 0) {
                virObjectUnref(ret);
                ret = NULL;
            }
        }
    }
    VIR_DEBUG("Returning caps %p for %s", ret, binary);
    virObjectRef(ret);
    virMutexUnlock(&cache->lock);
    return ret;
}


virQEMUCapsPtr
virQEMUCapsCacheLookupCopy(virQEMUCapsCachePtr cache, const char *binary)
{
    virQEMUCapsPtr qemuCaps = virQEMUCapsCacheLookup(cache, binary);
    virQEMUCapsPtr ret;

    if (!qemuCaps)
        return NULL;

    ret = virQEMUCapsNewCopy(qemuCaps);
    virObjectUnref(qemuCaps);
    return ret;
}


static int
virQEMUCapsCompareArch(const void *payload,
                       const void *name ATTRIBUTE_UNUSED,
                       const void *opaque)
{
    struct virQEMUCapsSearchData *data = (struct virQEMUCapsSearchData *) opaque;
    const virQEMUCaps *qemuCaps = payload;

    return qemuCaps->arch == data->arch;
}


virQEMUCapsPtr
virQEMUCapsCacheLookupByArch(virQEMUCapsCachePtr cache,
                             virArch arch)
{
    virQEMUCapsPtr ret = NULL;
    struct virQEMUCapsSearchData data = { .arch = arch };

    virMutexLock(&cache->lock);
    ret = virHashSearch(cache->binaries, virQEMUCapsCompareArch, &data);
    VIR_DEBUG("Returning caps %p for arch %s", ret, virArchToString(arch));
    virObjectRef(ret);
    virMutexUnlock(&cache->lock);

    return ret;
}


void
virQEMUCapsCacheFree(virQEMUCapsCachePtr cache)
{
    if (!cache)
        return;

    VIR_FREE(cache->libDir);
    VIR_FREE(cache->cacheDir);
    virHashFree(cache->binaries);
    virMutexDestroy(&cache->lock);
    VIR_FREE(cache);
}

bool
virQEMUCapsUsedQMP(virQEMUCapsPtr qemuCaps)
{
    return qemuCaps->usedQMP;
}

bool
virQEMUCapsSupportsChardev(virDomainDefPtr def,
                           virQEMUCapsPtr qemuCaps,
                           virDomainChrDefPtr chr)
{
    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_CHARDEV) ||
        !virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE))
        return false;

    if ((def->os.arch == VIR_ARCH_PPC) || ARCH_IS_PPC64(def->os.arch)) {
        /* only pseries need -device spapr-vty with -chardev */
        return (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL &&
                chr->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO);
    }

    if ((def->os.arch != VIR_ARCH_ARMV7L) && (def->os.arch != VIR_ARCH_AARCH64))
        return true;

    /* This may not be true for all ARM machine types, but at least
     * the only supported non-virtio serial devices of vexpress and versatile
     * don't have the -chardev property wired up. */
    return (chr->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_MMIO ||
            (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE &&
             chr->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_VIRTIO));
}


bool
virQEMUCapsIsMachineSupported(virQEMUCapsPtr qemuCaps,
                              const char *canonical_machine)
{
    size_t i;

    for (i = 0; i < qemuCaps->nmachineTypes; i++) {
        if (STREQ(canonical_machine, qemuCaps->machineTypes[i]))
            return true;
    }
    return false;
}


const char *
virQEMUCapsGetDefaultMachine(virQEMUCapsPtr qemuCaps)
{
    if (!qemuCaps->nmachineTypes)
        return NULL;
    return qemuCaps->machineTypes[0];
}


static int
virQEMUCapsFillDomainLoaderCaps(virQEMUCapsPtr qemuCaps,
                                virDomainCapsLoaderPtr capsLoader,
                                char **loader,
                                size_t nloader)
{
    size_t i;

    capsLoader->device.supported = true;

    if (VIR_ALLOC_N(capsLoader->values.values, nloader) < 0)
        return -1;

    for (i = 0; i < nloader; i++) {
        const char *filename = loader[i];

        if (!virFileExists(filename)) {
            VIR_DEBUG("loader filename=%s does not exist", filename);
            continue;
        }

        if (VIR_STRDUP(capsLoader->values.values[capsLoader->values.nvalues],
                       filename) < 0)
            return -1;
        capsLoader->values.nvalues++;
    }

    VIR_DOMAIN_CAPS_ENUM_SET(capsLoader->type,
                             VIR_DOMAIN_LOADER_TYPE_ROM);

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE) &&
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE_FORMAT))
        VIR_DOMAIN_CAPS_ENUM_SET(capsLoader->type,
                                 VIR_DOMAIN_LOADER_TYPE_PFLASH);


    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE_READONLY))
        VIR_DOMAIN_CAPS_ENUM_SET(capsLoader->readonly,
                                 VIR_TRISTATE_BOOL_YES,
                                 VIR_TRISTATE_BOOL_NO);
    return 0;
}


static int
virQEMUCapsFillDomainOSCaps(virQEMUCapsPtr qemuCaps,
                            virDomainCapsOSPtr os,
                            char **loader,
                            size_t nloader)
{
    virDomainCapsLoaderPtr capsLoader = &os->loader;

    os->device.supported = true;
    if (virQEMUCapsFillDomainLoaderCaps(qemuCaps, capsLoader,
                                        loader, nloader) < 0)
        return -1;
    return 0;
}


static int
virQEMUCapsFillDomainDeviceDiskCaps(virQEMUCapsPtr qemuCaps,
                                    virDomainCapsDeviceDiskPtr disk)
{
    disk->device.supported = true;
    /* QEMU supports all of these */
    VIR_DOMAIN_CAPS_ENUM_SET(disk->diskDevice,
                             VIR_DOMAIN_DISK_DEVICE_DISK,
                             VIR_DOMAIN_DISK_DEVICE_CDROM,
                             VIR_DOMAIN_DISK_DEVICE_FLOPPY);

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_BLK_SG_IO))
        VIR_DOMAIN_CAPS_ENUM_SET(disk->diskDevice, VIR_DOMAIN_DISK_DEVICE_LUN);

    VIR_DOMAIN_CAPS_ENUM_SET(disk->bus,
                             VIR_DOMAIN_DISK_BUS_IDE,
                             VIR_DOMAIN_DISK_BUS_FDC,
                             VIR_DOMAIN_DISK_BUS_SCSI,
                             VIR_DOMAIN_DISK_BUS_VIRTIO,
                             /* VIR_DOMAIN_DISK_BUS_SD */);

    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_USB_STORAGE))
        VIR_DOMAIN_CAPS_ENUM_SET(disk->bus, VIR_DOMAIN_DISK_BUS_USB);
    return 0;
}


static int
virQEMUCapsFillDomainDeviceHostdevCaps(virQEMUCapsPtr qemuCaps,
                                       virDomainCapsDeviceHostdevPtr hostdev)
{
    bool supportsPassthroughKVM = qemuHostdevHostSupportsPassthroughLegacy();
    bool supportsPassthroughVFIO = qemuHostdevHostSupportsPassthroughVFIO();

    hostdev->device.supported = true;
    /* VIR_DOMAIN_HOSTDEV_MODE_CAPABILITIES is for containers only */
    VIR_DOMAIN_CAPS_ENUM_SET(hostdev->mode,
                             VIR_DOMAIN_HOSTDEV_MODE_SUBSYS);

    VIR_DOMAIN_CAPS_ENUM_SET(hostdev->startupPolicy,
                             VIR_DOMAIN_STARTUP_POLICY_DEFAULT,
                             VIR_DOMAIN_STARTUP_POLICY_MANDATORY,
                             VIR_DOMAIN_STARTUP_POLICY_REQUISITE,
                             VIR_DOMAIN_STARTUP_POLICY_OPTIONAL);

    VIR_DOMAIN_CAPS_ENUM_SET(hostdev->subsysType,
                             VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB,
                             VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI);
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DRIVE) &&
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE) &&
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_SCSI_GENERIC))
        VIR_DOMAIN_CAPS_ENUM_SET(hostdev->subsysType,
                                 VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI);

    /* No virDomainHostdevCapsType for QEMU */
    virDomainCapsEnumClear(&hostdev->capsType);

    virDomainCapsEnumClear(&hostdev->pciBackend);
    if (supportsPassthroughVFIO &&
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_VFIO_PCI)) {
        VIR_DOMAIN_CAPS_ENUM_SET(hostdev->pciBackend,
                                 VIR_DOMAIN_HOSTDEV_PCI_BACKEND_DEFAULT,
                                 VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO);
    }

    if (supportsPassthroughKVM &&
        (virQEMUCapsGet(qemuCaps, QEMU_CAPS_PCIDEVICE) ||
         virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE))) {
        VIR_DOMAIN_CAPS_ENUM_SET(hostdev->pciBackend,
                                 VIR_DOMAIN_HOSTDEV_PCI_BACKEND_DEFAULT,
                                 VIR_DOMAIN_HOSTDEV_PCI_BACKEND_KVM);
    }
    return 0;
}


int
virQEMUCapsFillDomainCaps(virDomainCapsPtr domCaps,
                          virQEMUCapsPtr qemuCaps,
                          char **loader,
                          size_t nloader)
{
    virDomainCapsOSPtr os = &domCaps->os;
    virDomainCapsDeviceDiskPtr disk = &domCaps->disk;
    virDomainCapsDeviceHostdevPtr hostdev = &domCaps->hostdev;
    int maxvcpus = virQEMUCapsGetMachineMaxCpus(qemuCaps, domCaps->machine);

    domCaps->maxvcpus = maxvcpus;

    if (virQEMUCapsFillDomainOSCaps(qemuCaps, os,
                                    loader, nloader) < 0 ||
        virQEMUCapsFillDomainDeviceDiskCaps(qemuCaps, disk) < 0 ||
        virQEMUCapsFillDomainDeviceHostdevCaps(qemuCaps, hostdev) < 0)
        return -1;
    return 0;
}
