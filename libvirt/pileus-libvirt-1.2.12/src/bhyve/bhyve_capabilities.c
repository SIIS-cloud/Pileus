/*
 * bhyve_capabilities.c: bhyve capabilities module
 *
 * Copyright (C) 2014 Roman Bogorodskiy
 * Copyright (C) 2014 Semihalf
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
 */
#include <config.h>
#include <sys/utsname.h>

#include "viralloc.h"
#include "virfile.h"
#include "virlog.h"
#include "virstring.h"
#include "cpu/cpu.h"
#include "nodeinfo.h"
#include "bhyve_utils.h"
#include "domain_conf.h"
#include "vircommand.h"
#include "bhyve_capabilities.h"

#define VIR_FROM_THIS   VIR_FROM_BHYVE

VIR_LOG_INIT("bhyve.bhyve_capabilities");

static int
virBhyveCapsInitCPU(virCapsPtr caps,
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

    if (!(data = cpuNodeData(arch)) ||
        cpuDecode(cpu, data, NULL, 0, NULL) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    cpuDataFree(data);

    return ret;

 error:
    virCPUDefFree(cpu);
    goto cleanup;
}

virCapsPtr
virBhyveCapsBuild(void)
{
    virCapsPtr caps;
    virCapsGuestPtr guest;

    if ((caps = virCapabilitiesNew(virArchFromHost(),
                                   false, false)) == NULL)
        return NULL;

    if ((guest = virCapabilitiesAddGuest(caps, "hvm",
                                         VIR_ARCH_X86_64,
                                         "bhyve",
                                         NULL, 0, NULL)) == NULL)
        goto error;

    if (virCapabilitiesAddGuestDomain(guest,
                                      "bhyve", NULL, NULL, 0, NULL) == NULL)
        goto error;

    if (virBhyveCapsInitCPU(caps, virArchFromHost()) < 0)
            VIR_WARN("Failed to get host CPU");

    return caps;

 error:
    virObjectUnref(caps);
    return NULL;
}

int
virBhyveProbeGrubCaps(virBhyveGrubCapsFlags *caps)
{
    char *binary, *help;
    virCommandPtr cmd;
    int ret, exit;

    ret = 0;
    *caps = 0;
    cmd = NULL;
    help = NULL;

    binary = virFindFileInPath("grub-bhyve");
    if (binary == NULL)
        goto out;
    if (!virFileIsExecutable(binary))
        goto out;

    cmd = virCommandNew(binary);
    virCommandAddArg(cmd, "--help");
    virCommandSetOutputBuffer(cmd, &help);
    if (virCommandRun(cmd, &exit) < 0) {
        ret = -1;
        goto out;
    }

    if (strstr(help, "--cons-dev") != NULL)
        *caps |= BHYVE_GRUB_CAP_CONSDEV;

 out:
    VIR_FREE(help);
    virCommandFree(cmd);
    VIR_FREE(binary);
    return ret;
}
