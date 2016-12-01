/*
 * bhyve_domain.c: bhyve domain private state
 *
 * Copyright (C) 2014 Roman Bogorodskiy
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
 * Author: Roman Bogorodskiy
 */

#include <config.h>

#include "bhyve_device.h"
#include "bhyve_domain.h"
#include "viralloc.h"
#include "virlog.h"

#define VIR_FROM_THIS VIR_FROM_BHYVE

VIR_LOG_INIT("bhyve.bhyve_domain");

static void *
bhyveDomainObjPrivateAlloc(void)
{
    bhyveDomainObjPrivatePtr priv;

    if (VIR_ALLOC(priv) < 0)
        return NULL;

    return priv;
}

static void
bhyveDomainObjPrivateFree(void *data)
{
    bhyveDomainObjPrivatePtr priv = data;

    virDomainPCIAddressSetFree(priv->pciaddrs);

    VIR_FREE(priv);
}

virDomainXMLPrivateDataCallbacks virBhyveDriverPrivateDataCallbacks = {
    .alloc = bhyveDomainObjPrivateAlloc,
    .free = bhyveDomainObjPrivateFree,
};

static int
bhyveDomainDefPostParse(virDomainDefPtr def,
                        virCapsPtr caps ATTRIBUTE_UNUSED,
                        void *opaque ATTRIBUTE_UNUSED)
{
    /* Add an implicit PCI root controller */
    if (virDomainDefMaybeAddController(def, VIR_DOMAIN_CONTROLLER_TYPE_PCI, 0,
                                       VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT) < 0)
        return -1;

    return 0;
}

static int
bhyveDomainDeviceDefPostParse(virDomainDeviceDefPtr dev ATTRIBUTE_UNUSED,
                              const virDomainDef *def ATTRIBUTE_UNUSED,
                              virCapsPtr caps ATTRIBUTE_UNUSED,
                              void *opaque ATTRIBUTE_UNUSED)
{
    return 0;
}

virDomainDefParserConfig virBhyveDriverDomainDefParserConfig = {
    .devicesPostParseCallback = bhyveDomainDeviceDefPostParse,
    .domainPostParseCallback = bhyveDomainDefPostParse,
};
