/*
 * domain_addr.c: helper APIs for managing domain device addresses
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

#include "viralloc.h"
#include "virlog.h"
#include "virstring.h"
#include "domain_addr.h"

#define VIR_FROM_THIS VIR_FROM_DOMAIN

VIR_LOG_INIT("conf.domain_addr");

bool
virDomainPCIAddressFlagsCompatible(virDevicePCIAddressPtr addr,
                                   const char *addrStr,
                                   virDomainPCIConnectFlags busFlags,
                                   virDomainPCIConnectFlags devFlags,
                                   bool reportError,
                                   bool fromConfig)
{
    virErrorNumber errType = (fromConfig
                              ? VIR_ERR_XML_ERROR : VIR_ERR_INTERNAL_ERROR);
    virDomainPCIConnectFlags flagsMatchMask = VIR_PCI_CONNECT_TYPES_MASK;

    if (fromConfig)
       flagsMatchMask |= VIR_PCI_CONNECT_TYPE_EITHER_IF_CONFIG;

    /* If this bus doesn't allow the type of connection (PCI
     * vs. PCIe) required by the device, or if the device requires
     * hot-plug and this bus doesn't have it, return false.
     */
    if (!(devFlags & busFlags & flagsMatchMask)) {
        if (reportError) {
            if (devFlags & VIR_PCI_CONNECT_TYPE_PCI) {
                virReportError(errType,
                               _("PCI bus is not compatible with the device "
                                 "at %s. Device requires a standard PCI slot, "
                                 "which is not provided by bus %.4x:%.2x"),
                               addrStr, addr->domain, addr->bus);
            } else if (devFlags & VIR_PCI_CONNECT_TYPE_PCIE) {
                virReportError(errType,
                               _("PCI bus is not compatible with the device "
                                 "at %s. Device requires a PCI Express slot, "
                                 "which is not provided by bus %.4x:%.2x"),
                               addrStr, addr->domain, addr->bus);
            } else {
                /* this should never happen. If it does, there is a
                 * bug in the code that sets the flag bits for devices.
                 */
                virReportError(errType,
                           _("The device information for %s has no PCI "
                             "connection types listed"), addrStr);
            }
        }
        return false;
    }
    if ((devFlags & VIR_PCI_CONNECT_HOTPLUGGABLE) &&
        !(busFlags & VIR_PCI_CONNECT_HOTPLUGGABLE)) {
        if (reportError) {
            virReportError(errType,
                           _("PCI bus is not compatible with the device "
                             "at %s. Device requires hot-plug capability, "
                             "which is not provided by bus %.4x:%.2x"),
                           addrStr, addr->domain, addr->bus);
        }
        return false;
    }
    return true;
}


/* Verify that the address is in bounds for the chosen bus, and
 * that the bus is of the correct type for the device (via
 * comparing the flags).
 */
bool
virDomainPCIAddressValidate(virDomainPCIAddressSetPtr addrs,
                            virDevicePCIAddressPtr addr,
                            const char *addrStr,
                            virDomainPCIConnectFlags flags,
                            bool fromConfig)
{
    virDomainPCIAddressBusPtr bus;
    virErrorNumber errType = (fromConfig
                              ? VIR_ERR_XML_ERROR : VIR_ERR_INTERNAL_ERROR);

    if (addrs->nbuses == 0) {
        virReportError(errType, "%s", _("No PCI buses available"));
        return false;
    }
    if (addr->domain != 0) {
        virReportError(errType,
                       _("Invalid PCI address %s. "
                         "Only PCI domain 0 is available"),
                       addrStr);
        return false;
    }
    if (addr->bus >= addrs->nbuses) {
        virReportError(errType,
                       _("Invalid PCI address %s. "
                         "Only PCI buses up to %zu are available"),
                       addrStr, addrs->nbuses - 1);
        return false;
    }

    bus = &addrs->buses[addr->bus];

    /* assure that at least one of the requested connection types is
     * provided by this bus
     */
    if (!virDomainPCIAddressFlagsCompatible(addr, addrStr, bus->flags,
                                            flags, true, fromConfig))
        return false;

    /* some "buses" are really just a single port */
    if (bus->minSlot && addr->slot < bus->minSlot) {
        virReportError(errType,
                       _("Invalid PCI address %s. slot must be >= %zu"),
                       addrStr, bus->minSlot);
        return false;
    }
    if (addr->slot > bus->maxSlot) {
        virReportError(errType,
                       _("Invalid PCI address %s. slot must be <= %zu"),
                       addrStr, bus->maxSlot);
        return false;
    }
    if (addr->function > VIR_PCI_ADDRESS_FUNCTION_LAST) {
        virReportError(errType,
                       _("Invalid PCI address %s. function must be <= %u"),
                       addrStr, VIR_PCI_ADDRESS_FUNCTION_LAST);
        return false;
    }
    return true;
}


int
virDomainPCIAddressBusSetModel(virDomainPCIAddressBusPtr bus,
                               virDomainControllerModelPCI model)
{
    switch (model) {
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_BRIDGE:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT:
        bus->flags = (VIR_PCI_CONNECT_HOTPLUGGABLE |
                      VIR_PCI_CONNECT_TYPE_PCI);
        bus->minSlot = 1;
        bus->maxSlot = VIR_PCI_ADDRESS_SLOT_LAST;
        break;
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT:
        /* slots 1 - 31, no hotplug, PCIe only unless the address was
         * specified in user config *and* the particular device being
         * attached also allows it
         */
        bus->flags = (VIR_PCI_CONNECT_TYPE_PCIE |
                      VIR_PCI_CONNECT_TYPE_EITHER_IF_CONFIG);
        bus->minSlot = 1;
        bus->maxSlot = VIR_PCI_ADDRESS_SLOT_LAST;
        break;
    case VIR_DOMAIN_CONTROLLER_MODEL_DMI_TO_PCI_BRIDGE:
        /* slots 1 - 31, standard PCI slots,
         * but *not* hot-pluggable */
        bus->flags = VIR_PCI_CONNECT_TYPE_PCI;
        bus->minSlot = 1;
        bus->maxSlot = VIR_PCI_ADDRESS_SLOT_LAST;
        break;
    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Invalid PCI controller model %d"), model);
        return -1;
    }

    bus->model = model;
    return 0;
}


/* Ensure addr fits in the address set, by expanding it if needed.
 * This will only grow if the flags say that we need a normal
 * hot-pluggable PCI slot. If we need a different type of slot, it
 * will fail.
 *
 * Return value:
 * -1 = OOM
 *  0 = no action performed
 * >0 = number of buses added
 */
int
virDomainPCIAddressSetGrow(virDomainPCIAddressSetPtr addrs,
                           virDevicePCIAddressPtr addr,
                           virDomainPCIConnectFlags flags)
{
    int add;
    size_t i;

    add = addr->bus - addrs->nbuses + 1;
    i = addrs->nbuses;
    if (add <= 0)
        return 0;

    /* auto-grow only works when we're adding plain PCI devices */
    if (!(flags & VIR_PCI_CONNECT_TYPE_PCI)) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot automatically add a new PCI bus for a "
                         "device requiring a slot other than standard PCI."));
        return -1;
    }

    if (VIR_EXPAND_N(addrs->buses, addrs->nbuses, add) < 0)
        return -1;

    for (; i < addrs->nbuses; i++) {
        /* Any time we auto-add a bus, we will want a multi-slot
         * bus. Currently the only type of bus we will auto-add is a
         * pci-bridge, which is hot-pluggable and provides standard
         * PCI slots.
         */
        virDomainPCIAddressBusSetModel(&addrs->buses[i],
                                       VIR_DOMAIN_CONTROLLER_MODEL_PCI_BRIDGE);
    }
    return add;
}


char *
virDomainPCIAddressAsString(virDevicePCIAddressPtr addr)
{
    char *str;

    ignore_value(virAsprintf(&str, "%.4x:%.2x:%.2x.%.1x",
                             addr->domain,
                             addr->bus,
                             addr->slot,
                             addr->function));
    return str;
}


/*
 * Check if the PCI slot is used by another device.
 */
bool
virDomainPCIAddressSlotInUse(virDomainPCIAddressSetPtr addrs,
                             virDevicePCIAddressPtr addr)
{
    return !!addrs->buses[addr->bus].slots[addr->slot];
}


/*
 * Reserve a slot (or just one function) for a device. If
 * reserveEntireSlot is true, all functions for the slot are reserved,
 * otherwise only one. If fromConfig is true, the address being
 * requested came directly from the config and errors should be worded
 * appropriately. If fromConfig is false, the address was
 * automatically created by libvirt, so it is an internal error (not
 * XML).
 */
int
virDomainPCIAddressReserveAddr(virDomainPCIAddressSetPtr addrs,
                               virDevicePCIAddressPtr addr,
                               virDomainPCIConnectFlags flags,
                               bool reserveEntireSlot,
                               bool fromConfig)
{
    int ret = -1;
    char *addrStr = NULL;
    virDomainPCIAddressBusPtr bus;
    virErrorNumber errType = (fromConfig
                              ? VIR_ERR_XML_ERROR : VIR_ERR_INTERNAL_ERROR);

    if (!(addrStr = virDomainPCIAddressAsString(addr)))
        goto cleanup;

    /* Add an extra bus if necessary */
    if (addrs->dryRun && virDomainPCIAddressSetGrow(addrs, addr, flags) < 0)
        goto cleanup;
    /* Check that the requested bus exists, is the correct type, and we
     * are asking for a valid slot
     */
    if (!virDomainPCIAddressValidate(addrs, addr, addrStr, flags, fromConfig))
        goto cleanup;

    bus = &addrs->buses[addr->bus];

    if (reserveEntireSlot) {
        if (bus->slots[addr->slot]) {
            virReportError(errType,
                           _("Attempted double use of PCI slot %s "
                             "(may need \"multifunction='on'\" for "
                             "device on function 0)"), addrStr);
            goto cleanup;
        }
        bus->slots[addr->slot] = 0xFF; /* reserve all functions of slot */
        VIR_DEBUG("Reserving PCI slot %s (multifunction='off')", addrStr);
    } else {
        if (bus->slots[addr->slot] & (1 << addr->function)) {
            if (addr->function == 0) {
                virReportError(errType,
                               _("Attempted double use of PCI Address %s"),
                               addrStr);
            } else {
                virReportError(errType,
                               _("Attempted double use of PCI Address %s "
                                 "(may need \"multifunction='on'\" "
                                 "for device on function 0)"), addrStr);
            }
            goto cleanup;
        }
        bus->slots[addr->slot] |= (1 << addr->function);
        VIR_DEBUG("Reserving PCI address %s", addrStr);
    }

    ret = 0;
 cleanup:
    VIR_FREE(addrStr);
    return ret;
}


int
virDomainPCIAddressReserveSlot(virDomainPCIAddressSetPtr addrs,
                               virDevicePCIAddressPtr addr,
                               virDomainPCIConnectFlags flags)
{
    return virDomainPCIAddressReserveAddr(addrs, addr, flags, true, false);
}

int
virDomainPCIAddressEnsureAddr(virDomainPCIAddressSetPtr addrs,
                              virDomainDeviceInfoPtr dev)
{
    int ret = -1;
    char *addrStr = NULL;
    /* Flags should be set according to the particular device,
     * but only the caller knows the type of device. Currently this
     * function is only used for hot-plug, though, and hot-plug is
     * only supported for standard PCI devices, so we can safely use
     * the setting below */
    virDomainPCIConnectFlags flags = (VIR_PCI_CONNECT_HOTPLUGGABLE |
                                      VIR_PCI_CONNECT_TYPE_PCI);

    if (!(addrStr = virDomainPCIAddressAsString(&dev->addr.pci)))
        goto cleanup;

    if (dev->type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
        /* We do not support hotplug multi-function PCI device now, so we should
         * reserve the whole slot. The function of the PCI device must be 0.
         */
        if (dev->addr.pci.function != 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Only PCI device addresses with function=0"
                             " are supported"));
            goto cleanup;
        }

        if (!virDomainPCIAddressValidate(addrs, &dev->addr.pci,
                                         addrStr, flags, true))
            goto cleanup;

        ret = virDomainPCIAddressReserveSlot(addrs, &dev->addr.pci, flags);
    } else {
        ret = virDomainPCIAddressReserveNextSlot(addrs, dev, flags);
    }

 cleanup:
    VIR_FREE(addrStr);
    return ret;
}


int
virDomainPCIAddressReleaseAddr(virDomainPCIAddressSetPtr addrs,
                               virDevicePCIAddressPtr addr)
{
    addrs->buses[addr->bus].slots[addr->slot] &= ~(1 << addr->function);
    return 0;
}

int
virDomainPCIAddressReleaseSlot(virDomainPCIAddressSetPtr addrs,
                               virDevicePCIAddressPtr addr)
{
    /* permit any kind of connection type in validation, since we
     * already had it, and are giving it back.
     */
    virDomainPCIConnectFlags flags = VIR_PCI_CONNECT_TYPES_MASK;
    int ret = -1;
    char *addrStr = NULL;

    if (!(addrStr = virDomainPCIAddressAsString(addr)))
        goto cleanup;

    if (!virDomainPCIAddressValidate(addrs, addr, addrStr, flags, false))
        goto cleanup;

    addrs->buses[addr->bus].slots[addr->slot] = 0;
    ret = 0;
 cleanup:
    VIR_FREE(addrStr);
    return ret;
}


virDomainPCIAddressSetPtr
virDomainPCIAddressSetAlloc(unsigned int nbuses)
{
    virDomainPCIAddressSetPtr addrs;

    if (VIR_ALLOC(addrs) < 0)
        goto error;

    if (VIR_ALLOC_N(addrs->buses, nbuses) < 0)
        goto error;

    addrs->nbuses = nbuses;
    return addrs;

 error:
    virDomainPCIAddressSetFree(addrs);
    return NULL;
}


void
virDomainPCIAddressSetFree(virDomainPCIAddressSetPtr addrs)
{
    if (!addrs)
        return;

    VIR_FREE(addrs->buses);
    VIR_FREE(addrs);
}


int
virDomainPCIAddressGetNextSlot(virDomainPCIAddressSetPtr addrs,
                               virDevicePCIAddressPtr next_addr,
                               virDomainPCIConnectFlags flags)
{
    /* default to starting the search for a free slot from
     * 0000:00:00.0
     */
    virDevicePCIAddress a = { 0, 0, 0, 0, false };
    char *addrStr = NULL;

    /* except if this search is for the exact same type of device as
     * last time, continue the search from the previous match
     */
    if (flags == addrs->lastFlags)
        a = addrs->lastaddr;

    if (addrs->nbuses == 0) {
        virReportError(VIR_ERR_XML_ERROR, "%s", _("No PCI buses available"));
        goto error;
    }

    /* Start the search at the last used bus and slot */
    for (a.slot++; a.bus < addrs->nbuses; a.bus++) {
        if (!(addrStr = virDomainPCIAddressAsString(&a)))
            goto error;
        if (!virDomainPCIAddressFlagsCompatible(&a, addrStr,
                                                addrs->buses[a.bus].flags,
                                                flags, false, false)) {
            VIR_FREE(addrStr);
            VIR_DEBUG("PCI bus %.4x:%.2x is not compatible with the device",
                      a.domain, a.bus);
            continue;
        }
        for (; a.slot <= VIR_PCI_ADDRESS_SLOT_LAST; a.slot++) {
            if (!virDomainPCIAddressSlotInUse(addrs, &a))
                goto success;

            VIR_DEBUG("PCI slot %.4x:%.2x:%.2x already in use",
                      a.domain, a.bus, a.slot);
        }
        a.slot = 1;
        VIR_FREE(addrStr);
    }

    /* There were no free slots after the last used one */
    if (addrs->dryRun) {
        /* a is already set to the first new bus and slot 1 */
        if (virDomainPCIAddressSetGrow(addrs, &a, flags) < 0)
            goto error;
        goto success;
    } else if (flags == addrs->lastFlags) {
        /* Check the buses from 0 up to the last used one */
        for (a.bus = 0; a.bus <= addrs->lastaddr.bus; a.bus++) {
            addrStr = NULL;
            if (!(addrStr = virDomainPCIAddressAsString(&a)))
                goto error;
            if (!virDomainPCIAddressFlagsCompatible(&a, addrStr,
                                                    addrs->buses[a.bus].flags,
                                                    flags, false, false)) {
                VIR_DEBUG("PCI bus %.4x:%.2x is not compatible with the device",
                          a.domain, a.bus);
                continue;
            }
            for (a.slot = 1; a.slot <= VIR_PCI_ADDRESS_SLOT_LAST; a.slot++) {
                if (!virDomainPCIAddressSlotInUse(addrs, &a))
                    goto success;

                VIR_DEBUG("PCI slot %.4x:%.2x:%.2x already in use",
                          a.domain, a.bus, a.slot);
            }
        }
    }

    virReportError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("No more available PCI slots"));
 error:
    VIR_FREE(addrStr);
    return -1;

 success:
    VIR_DEBUG("Found free PCI slot %.4x:%.2x:%.2x",
              a.domain, a.bus, a.slot);
    *next_addr = a;
    VIR_FREE(addrStr);
    return 0;
}

int
virDomainPCIAddressReserveNextSlot(virDomainPCIAddressSetPtr addrs,
                                   virDomainDeviceInfoPtr dev,
                                   virDomainPCIConnectFlags flags)
{
    virDevicePCIAddress addr;
    if (virDomainPCIAddressGetNextSlot(addrs, &addr, flags) < 0)
        return -1;

    if (virDomainPCIAddressReserveSlot(addrs, &addr, flags) < 0)
        return -1;

    if (!addrs->dryRun) {
        dev->type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;
        dev->addr.pci = addr;
    }

    addrs->lastaddr = addr;
    addrs->lastFlags = flags;
    return 0;
}


static char*
virDomainCCWAddressAsString(virDomainDeviceCCWAddressPtr addr)
{
    char *addrstr = NULL;

    ignore_value(virAsprintf(&addrstr, "%x.%x.%04x",
                             addr->cssid,
                             addr->ssid,
                             addr->devno));
    return addrstr;
}

static int
virDomainCCWAddressIncrement(virDomainDeviceCCWAddressPtr addr)
{
    virDomainDeviceCCWAddress ccwaddr = *addr;

    /* We are not touching subchannel sets and channel subsystems */
    if (++ccwaddr.devno > VIR_DOMAIN_DEVICE_CCW_MAX_DEVNO)
        return -1;

    *addr = ccwaddr;
    return 0;
}


int
virDomainCCWAddressAssign(virDomainDeviceInfoPtr dev,
                          virDomainCCWAddressSetPtr addrs,
                          bool autoassign)
{
    int ret = -1;
    char *addr = NULL;

    if (dev->type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW)
        return 0;

    if (!autoassign && dev->addr.ccw.assigned) {
        if (!(addr = virDomainCCWAddressAsString(&dev->addr.ccw)))
            goto cleanup;

        if (virHashLookup(addrs->defined, addr)) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("The CCW devno '%s' is in use already "),
                           addr);
            goto cleanup;
        }
    } else if (autoassign && !dev->addr.ccw.assigned) {
        if (!(addr = virDomainCCWAddressAsString(&addrs->next)))
            goto cleanup;

        while (virHashLookup(addrs->defined, addr)) {
            if (virDomainCCWAddressIncrement(&addrs->next) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("There are no more free CCW devnos."));
                goto cleanup;
            }
            VIR_FREE(addr);
            if (!(addr = virDomainCCWAddressAsString(&addrs->next)))
                goto cleanup;
        }
        dev->addr.ccw = addrs->next;
        dev->addr.ccw.assigned = true;
    } else {
        return 0;
    }

    if (virHashAddEntry(addrs->defined, addr, addr) < 0)
        goto cleanup;
    else
        addr = NULL; /* memory will be freed by hash table */

    ret = 0;

 cleanup:
    VIR_FREE(addr);
    return ret;
}

int
virDomainCCWAddressAllocate(virDomainDefPtr def ATTRIBUTE_UNUSED,
                            virDomainDeviceDefPtr dev ATTRIBUTE_UNUSED,
                            virDomainDeviceInfoPtr info,
                            void *data)
{
    return virDomainCCWAddressAssign(info, data, true);
}

int
virDomainCCWAddressValidate(virDomainDefPtr def ATTRIBUTE_UNUSED,
                            virDomainDeviceDefPtr dev ATTRIBUTE_UNUSED,
                            virDomainDeviceInfoPtr info,
                            void *data)
{
    return virDomainCCWAddressAssign(info, data, false);
}

int
virDomainCCWAddressReleaseAddr(virDomainCCWAddressSetPtr addrs,
                               virDomainDeviceInfoPtr dev)
{
    char *addr;
    int ret;

    addr = virDomainCCWAddressAsString(&(dev->addr.ccw));
    if (!addr)
        return -1;

    if ((ret = virHashRemoveEntry(addrs->defined, addr)) == 0 &&
        dev->addr.ccw.cssid == addrs->next.cssid &&
        dev->addr.ccw.ssid == addrs->next.ssid &&
        dev->addr.ccw.devno < addrs->next.devno) {
        addrs->next.devno = dev->addr.ccw.devno;
        addrs->next.assigned = false;
    }

    VIR_FREE(addr);

    return ret;
}

void virDomainCCWAddressSetFree(virDomainCCWAddressSetPtr addrs)
{
    if (!addrs)
        return;

    virHashFree(addrs->defined);
    VIR_FREE(addrs);
}

virDomainCCWAddressSetPtr
virDomainCCWAddressSetCreate(void)
{
    virDomainCCWAddressSetPtr addrs = NULL;

    if (VIR_ALLOC(addrs) < 0)
        goto error;

    if (!(addrs->defined = virHashCreate(10, virHashValueFree)))
        goto error;

    /* must use cssid = 0xfe (254) for virtio-ccw devices */
    addrs->next.cssid = 254;
    addrs->next.ssid = 0;
    addrs->next.devno = 0;
    addrs->next.assigned = 0;
    return addrs;

 error:
    virDomainCCWAddressSetFree(addrs);
    return NULL;
}
