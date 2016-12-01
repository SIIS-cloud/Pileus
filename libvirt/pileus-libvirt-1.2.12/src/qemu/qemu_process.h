/*
 * qemu_process.h: QEMU process management
 *
 * Copyright (C) 2006-2012 Red Hat, Inc.
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

#ifndef __QEMU_PROCESS_H__
# define __QEMU_PROCESS_H__

# include "qemu_conf.h"
# include "qemu_domain.h"

int qemuProcessPrepareMonitorChr(virQEMUDriverConfigPtr cfg,
                                 virDomainChrSourceDefPtr monConfig,
                                 const char *vm);

int qemuProcessStartCPUs(virQEMUDriverPtr driver,
                         virDomainObjPtr vm,
                         virConnectPtr conn,
                         virDomainRunningReason reason,
                         qemuDomainAsyncJob asyncJob);
int qemuProcessStopCPUs(virQEMUDriverPtr driver,
                        virDomainObjPtr vm,
                        virDomainPausedReason reason,
                        qemuDomainAsyncJob asyncJob);

void qemuProcessAutostartAll(virQEMUDriverPtr driver);
void qemuProcessReconnectAll(virConnectPtr conn, virQEMUDriverPtr driver);

int qemuProcessAssignPCIAddresses(virDomainDefPtr def);

typedef enum {
    VIR_QEMU_PROCESS_START_COLD         = 1 << 0,
    VIR_QEMU_PROCESS_START_PAUSED       = 1 << 1,
    VIR_QEMU_PROCESS_START_AUTODESTROY  = 1 << 2,
} qemuProcessStartFlags;

int qemuProcessStart(virConnectPtr conn,
                     virQEMUDriverPtr driver,
                     virDomainObjPtr vm,
                     int asyncJob,
                     const char *migrateFrom,
                     int stdin_fd,
                     const char *stdin_path,
                     virDomainSnapshotObjPtr snapshot,
                     virNetDevVPortProfileOp vmop,
                     unsigned int flags);

typedef enum {
    VIR_QEMU_PROCESS_STOP_MIGRATED      = 1 << 0,
    VIR_QEMU_PROCESS_STOP_NO_RELABEL    = 1 << 1,
} qemuProcessStopFlags;

void qemuProcessStop(virQEMUDriverPtr driver,
                     virDomainObjPtr vm,
                     virDomainShutoffReason reason,
                     unsigned int flags);

int qemuProcessAttach(virConnectPtr conn,
                      virQEMUDriverPtr driver,
                      virDomainObjPtr vm,
                      pid_t pid,
                      const char *pidfile,
                      virDomainChrSourceDefPtr monConfig,
                      bool monJSON);

typedef enum {
   VIR_QEMU_PROCESS_KILL_FORCE  = 1 << 0,
   VIR_QEMU_PROCESS_KILL_NOWAIT = 1 << 1,
   VIR_QEMU_PROCESS_KILL_NOCHECK = 1 << 2, /* bypass the running vm check */
} virQemuProcessKillMode;

int qemuProcessKill(virDomainObjPtr vm, unsigned int flags);

void qemuProcessShutdownOrReboot(virQEMUDriverPtr driver,
                                 virDomainObjPtr vm);

int qemuProcessAutoDestroyInit(virQEMUDriverPtr driver);
void qemuProcessAutoDestroyShutdown(virQEMUDriverPtr driver);
int qemuProcessAutoDestroyAdd(virQEMUDriverPtr driver,
                              virDomainObjPtr vm,
                              virConnectPtr conn);
int qemuProcessAutoDestroyRemove(virQEMUDriverPtr driver,
                                 virDomainObjPtr vm);
bool qemuProcessAutoDestroyActive(virQEMUDriverPtr driver,
                                  virDomainObjPtr vm);
virBitmapPtr qemuPrepareCpumap(virQEMUDriverPtr driver,
                               virBitmapPtr nodemask);

int qemuProcessReadLog(int fd, char *buf, int buflen, int off, bool skipchar);

#endif /* __QEMU_PROCESS_H__ */
