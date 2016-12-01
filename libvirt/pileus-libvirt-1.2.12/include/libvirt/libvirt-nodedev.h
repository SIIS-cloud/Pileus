/*
 * libvirt-nodedev.h
 * Summary: APIs for management of nodedevs
 * Description: Provides APIs for the management of nodedevs
 * Author: Daniel Veillard <veillard@redhat.com>
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

#ifndef __VIR_LIBVIRT_NODEDEV_H__
# define __VIR_LIBVIRT_NODEDEV_H__

# ifndef __VIR_LIBVIRT_H_INCLUDES__
#  error "Don't include this file directly, only use libvirt/libvirt.h"
# endif


/**
 * virNodeDevice:
 *
 * A virNodeDevice contains a node (host) device details.
 */

typedef struct _virNodeDevice virNodeDevice;

/**
 * virNodeDevicePtr:
 *
 * A virNodeDevicePtr is a pointer to a virNodeDevice structure.  Get
 * one via virNodeDeviceLookupByName, or virNodeDeviceCreate.  Be sure
 * to call virNodeDeviceFree when done using a virNodeDevicePtr obtained
 * from any of the above functions to avoid leaking memory.
 */

typedef virNodeDevice *virNodeDevicePtr;


int                     virNodeNumOfDevices     (virConnectPtr conn,
                                                 const char *cap,
                                                 unsigned int flags);

int                     virNodeListDevices      (virConnectPtr conn,
                                                 const char *cap,
                                                 char **const names,
                                                 int maxnames,
                                                 unsigned int flags);
/*
 * virConnectListAllNodeDevices:
 *
 * Flags used to filter the returned node devices. Flags in each group
 * are exclusive. Currently only one group to filter the devices by cap
 * type.
 */
typedef enum {
    VIR_CONNECT_LIST_NODE_DEVICES_CAP_SYSTEM        = 1 << 0,  /* System capability */
    VIR_CONNECT_LIST_NODE_DEVICES_CAP_PCI_DEV       = 1 << 1,  /* PCI device */
    VIR_CONNECT_LIST_NODE_DEVICES_CAP_USB_DEV       = 1 << 2,  /* USB device */
    VIR_CONNECT_LIST_NODE_DEVICES_CAP_USB_INTERFACE = 1 << 3,  /* USB interface */
    VIR_CONNECT_LIST_NODE_DEVICES_CAP_NET           = 1 << 4,  /* Network device */
    VIR_CONNECT_LIST_NODE_DEVICES_CAP_SCSI_HOST     = 1 << 5,  /* SCSI Host Bus Adapter */
    VIR_CONNECT_LIST_NODE_DEVICES_CAP_SCSI_TARGET   = 1 << 6,  /* SCSI Target */
    VIR_CONNECT_LIST_NODE_DEVICES_CAP_SCSI          = 1 << 7,  /* SCSI device */
    VIR_CONNECT_LIST_NODE_DEVICES_CAP_STORAGE       = 1 << 8,  /* Storage device */
    VIR_CONNECT_LIST_NODE_DEVICES_CAP_FC_HOST       = 1 << 9,  /* FC Host Bus Adapter */
    VIR_CONNECT_LIST_NODE_DEVICES_CAP_VPORTS        = 1 << 10, /* Capable of vport */
    VIR_CONNECT_LIST_NODE_DEVICES_CAP_SCSI_GENERIC  = 1 << 11, /* Capable of scsi_generic */
} virConnectListAllNodeDeviceFlags;

int                     virConnectListAllNodeDevices (virConnectPtr conn,
                                                      virNodeDevicePtr **devices,
                                                      unsigned int flags);

virNodeDevicePtr        virNodeDeviceLookupByName (virConnectPtr conn,
                                                   const char *name);

virNodeDevicePtr        virNodeDeviceLookupSCSIHostByWWN (virConnectPtr conn,
                                                          const char *wwnn,
                                                          const char *wwpn,
                                                          unsigned int flags);

const char *            virNodeDeviceGetName     (virNodeDevicePtr dev);

const char *            virNodeDeviceGetParent   (virNodeDevicePtr dev);

int                     virNodeDeviceNumOfCaps   (virNodeDevicePtr dev);

int                     virNodeDeviceListCaps    (virNodeDevicePtr dev,
                                                  char **const names,
                                                  int maxnames);

char *                  virNodeDeviceGetXMLDesc (virNodeDevicePtr dev,
                                                 unsigned int flags);

int                     virNodeDeviceRef        (virNodeDevicePtr dev);
int                     virNodeDeviceFree       (virNodeDevicePtr dev);

int                     virNodeDeviceDettach    (virNodeDevicePtr dev);
int                     virNodeDeviceDetachFlags(virNodeDevicePtr dev,
                                                 const char *driverName,
                                                 unsigned int flags);
int                     virNodeDeviceReAttach   (virNodeDevicePtr dev);
int                     virNodeDeviceReset      (virNodeDevicePtr dev);

virNodeDevicePtr        virNodeDeviceCreateXML  (virConnectPtr conn,
                                                 const char *xmlDesc,
                                                 unsigned int flags);

int                     virNodeDeviceDestroy    (virNodeDevicePtr dev);


#endif /* __VIR_LIBVIRT_NODEDEV_H__ */
