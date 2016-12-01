/*
 * driver-network.h: entry points for network drivers
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

#ifndef __VIR_DRIVER_NETWORK_H__
# define __VIR_DRIVER_NETWORK_H__

# ifndef __VIR_DRIVER_H_INCLUDES___
#  error "Don't include this file directly, only use driver.h"
# endif

typedef virDrvConnectOpen virDrvNetworkOpen;
typedef virDrvConnectClose virDrvNetworkClose;

typedef int
(*virDrvConnectNumOfNetworks)(virConnectPtr conn);

typedef int
(*virDrvConnectListNetworks)(virConnectPtr conn,
                             char **const names,
                             int maxnames);

typedef int
(*virDrvConnectNumOfDefinedNetworks)(virConnectPtr conn);

typedef int
(*virDrvConnectListDefinedNetworks)(virConnectPtr conn,
                                    char **const names,
                                    int maxnames);

typedef int
(*virDrvConnectListAllNetworks)(virConnectPtr conn,
                                virNetworkPtr **nets,
                                unsigned int flags);

typedef int
(*virDrvConnectNetworkEventRegisterAny)(virConnectPtr conn,
                                        virNetworkPtr dom,
                                        int eventID,
                                        virConnectNetworkEventGenericCallback cb,
                                        void *opaque,
                                        virFreeCallback freecb);

typedef int
(*virDrvConnectNetworkEventDeregisterAny)(virConnectPtr conn,
                                          int callbackID);

typedef virNetworkPtr
(*virDrvNetworkLookupByUUID)(virConnectPtr conn,
                             const unsigned char *uuid);

typedef virNetworkPtr
(*virDrvNetworkLookupByName)(virConnectPtr conn,
                             const char *name);

typedef virNetworkPtr
(*virDrvNetworkCreateXML)(virConnectPtr conn,
                          const char *xmlDesc);

typedef virNetworkPtr
(*virDrvNetworkDefineXML)(virConnectPtr conn,
                          const char *xml);

typedef int
(*virDrvNetworkUndefine)(virNetworkPtr network);

typedef int
(*virDrvNetworkUpdate)(virNetworkPtr network,
                       unsigned int command, /* virNetworkUpdateCommand */
                       unsigned int section, /* virNetworkUpdateSection */
                       int parentIndex,
                       const char *xml,
                       unsigned int flags);

typedef int
(*virDrvNetworkCreate)(virNetworkPtr network);

typedef int
(*virDrvNetworkDestroy)(virNetworkPtr network);

typedef char *
(*virDrvNetworkGetXMLDesc)(virNetworkPtr network,
                           unsigned int flags);

typedef char *
(*virDrvNetworkGetBridgeName)(virNetworkPtr network);

typedef int
(*virDrvNetworkGetAutostart)(virNetworkPtr network,
                             int *autostart);

typedef int
(*virDrvNetworkSetAutostart)(virNetworkPtr network,
                             int autostart);

typedef int
(*virDrvNetworkIsActive)(virNetworkPtr net);

typedef int
(*virDrvNetworkIsPersistent)(virNetworkPtr net);

typedef int
(*virDrvNetworkGetDHCPLeases)(virNetworkPtr network,
                              const char *mac,
                              virNetworkDHCPLeasePtr **leases,
                              unsigned int flags);

typedef struct _virNetworkDriver virNetworkDriver;
typedef virNetworkDriver *virNetworkDriverPtr;

/**
 * _virNetworkDriver:
 *
 * Structure associated to a network virtualization driver, defining the various
 * entry points for it.
 *
 * All drivers must support the following fields/methods:
 *  - open
 *  - close
 */
struct _virNetworkDriver {
    const char * name; /* the name of the driver */
    virDrvNetworkOpen networkOpen;
    virDrvNetworkClose networkClose;
    virDrvConnectNumOfNetworks connectNumOfNetworks;
    virDrvConnectListNetworks connectListNetworks;
    virDrvConnectNumOfDefinedNetworks connectNumOfDefinedNetworks;
    virDrvConnectListDefinedNetworks connectListDefinedNetworks;
    virDrvConnectListAllNetworks connectListAllNetworks;
    virDrvConnectNetworkEventRegisterAny connectNetworkEventRegisterAny;
    virDrvConnectNetworkEventDeregisterAny connectNetworkEventDeregisterAny;
    virDrvNetworkLookupByUUID networkLookupByUUID;
    virDrvNetworkLookupByName networkLookupByName;
    virDrvNetworkCreateXML networkCreateXML;
    virDrvNetworkDefineXML networkDefineXML;
    virDrvNetworkUndefine networkUndefine;
    virDrvNetworkUpdate networkUpdate;
    virDrvNetworkCreate networkCreate;
    virDrvNetworkDestroy networkDestroy;
    virDrvNetworkGetXMLDesc networkGetXMLDesc;
    virDrvNetworkGetBridgeName networkGetBridgeName;
    virDrvNetworkGetAutostart networkGetAutostart;
    virDrvNetworkSetAutostart networkSetAutostart;
    virDrvNetworkIsActive networkIsActive;
    virDrvNetworkIsPersistent networkIsPersistent;
    virDrvNetworkGetDHCPLeases networkGetDHCPLeases;
};


#endif /* __VIR_DRIVER_NETWORK_H__ */
