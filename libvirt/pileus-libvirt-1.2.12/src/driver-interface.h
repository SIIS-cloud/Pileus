/*
 * driver-interface.h: entry points for interface drivers
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

#ifndef __VIR_DRIVER_INTERFACE_H__
# define __VIR_DRIVER_INTERFACE_H__

# ifndef __VIR_DRIVER_H_INCLUDES___
#  error "Don't include this file directly, only use driver.h"
# endif

typedef virDrvConnectOpen virDrvInterfaceOpen;
typedef virDrvConnectClose virDrvInterfaceClose;

typedef int
(*virDrvConnectNumOfInterfaces)(virConnectPtr conn);

typedef int
(*virDrvConnectListInterfaces)(virConnectPtr conn,
                               char **const names,
                               int maxnames);

typedef int
(*virDrvConnectNumOfDefinedInterfaces)(virConnectPtr conn);

typedef int
(*virDrvConnectListDefinedInterfaces)(virConnectPtr conn,
                                      char **const names,
                                      int maxnames);

typedef int
(*virDrvConnectListAllInterfaces)(virConnectPtr conn,
                                  virInterfacePtr **ifaces,
                                  unsigned int flags);

typedef virInterfacePtr
(*virDrvInterfaceLookupByName)(virConnectPtr conn,
                               const char *name);

typedef virInterfacePtr
(*virDrvInterfaceLookupByMACString)(virConnectPtr conn,
                                    const char *mac);

typedef char *
(*virDrvInterfaceGetXMLDesc)(virInterfacePtr iface,
                             unsigned int flags);

typedef virInterfacePtr
(*virDrvInterfaceDefineXML)(virConnectPtr conn,
                            const char *xmlDesc,
                            unsigned int flags);

typedef int
(*virDrvInterfaceUndefine)(virInterfacePtr iface);

typedef int
(*virDrvInterfaceCreate)(virInterfacePtr iface,
                         unsigned int flags);

typedef int
(*virDrvInterfaceDestroy)(virInterfacePtr iface,
                          unsigned int flags);

typedef int
(*virDrvInterfaceIsActive)(virInterfacePtr iface);

typedef int
(*virDrvInterfaceChangeBegin)(virConnectPtr conn,
                              unsigned int flags);

typedef int
(*virDrvInterfaceChangeCommit)(virConnectPtr conn,
                               unsigned int flags);

typedef int
(*virDrvInterfaceChangeRollback)(virConnectPtr conn,
                                 unsigned int flags);

typedef struct _virInterfaceDriver virInterfaceDriver;
typedef virInterfaceDriver *virInterfaceDriverPtr;

/**
 * _virInterfaceDriver:
 *
 * Structure associated to a network interface driver, defining the various
 * entry points for it.
 *
 * All drivers must support the following fields/methods:
 *  - open
 *  - close
 */
struct _virInterfaceDriver {
    const char *name; /* the name of the driver */
    virDrvInterfaceOpen interfaceOpen;
    virDrvInterfaceClose interfaceClose;
    virDrvConnectNumOfInterfaces connectNumOfInterfaces;
    virDrvConnectListInterfaces connectListInterfaces;
    virDrvConnectNumOfDefinedInterfaces connectNumOfDefinedInterfaces;
    virDrvConnectListDefinedInterfaces connectListDefinedInterfaces;
    virDrvConnectListAllInterfaces connectListAllInterfaces;
    virDrvInterfaceLookupByName interfaceLookupByName;
    virDrvInterfaceLookupByMACString interfaceLookupByMACString;
    virDrvInterfaceGetXMLDesc interfaceGetXMLDesc;
    virDrvInterfaceDefineXML interfaceDefineXML;
    virDrvInterfaceUndefine interfaceUndefine;
    virDrvInterfaceCreate interfaceCreate;
    virDrvInterfaceDestroy interfaceDestroy;
    virDrvInterfaceIsActive interfaceIsActive;
    virDrvInterfaceChangeBegin interfaceChangeBegin;
    virDrvInterfaceChangeCommit interfaceChangeCommit;
    virDrvInterfaceChangeRollback interfaceChangeRollback;
};


#endif /* __VIR_DRIVER_INTERFACE_H__ */
