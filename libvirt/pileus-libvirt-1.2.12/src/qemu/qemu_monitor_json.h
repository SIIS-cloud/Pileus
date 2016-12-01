/*
 * qemu_monitor_json.h: interaction with QEMU monitor console
 *
 * Copyright (C) 2006-2009, 2011-2014 Red Hat, Inc.
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


#ifndef QEMU_MONITOR_JSON_H
# define QEMU_MONITOR_JSON_H

# include "internal.h"

# include "qemu_monitor.h"
# include "virbitmap.h"
# include "cpu/cpu.h"

int qemuMonitorJSONIOProcess(qemuMonitorPtr mon,
                             const char *data,
                             size_t len,
                             qemuMonitorMessagePtr msg);

int qemuMonitorJSONHumanCommandWithFd(qemuMonitorPtr mon,
                                      const char *cmd,
                                      int scm_fd,
                                      char **reply);

int qemuMonitorJSONSetCapabilities(qemuMonitorPtr mon);

int qemuMonitorJSONStartCPUs(qemuMonitorPtr mon,
                             virConnectPtr conn);
int qemuMonitorJSONStopCPUs(qemuMonitorPtr mon);
int qemuMonitorJSONGetStatus(qemuMonitorPtr mon,
                             bool *running,
                             virDomainPausedReason *reason);

int qemuMonitorJSONSystemPowerdown(qemuMonitorPtr mon);
int qemuMonitorJSONSystemReset(qemuMonitorPtr mon);

int qemuMonitorJSONGetCPUInfo(qemuMonitorPtr mon,
                              int **pids);
int qemuMonitorJSONGetVirtType(qemuMonitorPtr mon,
                               int *virtType);
int qemuMonitorJSONUpdateVideoMemorySize(qemuMonitorPtr mon,
                                         virDomainVideoDefPtr video,
                                         char *path);
int qemuMonitorJSONGetBalloonInfo(qemuMonitorPtr mon,
                                  unsigned long long *currmem);
int qemuMonitorJSONGetMemoryStats(qemuMonitorPtr mon,
                                  char *balloonpath,
                                  virDomainMemoryStatPtr stats,
                                  unsigned int nr_stats);
int qemuMonitorJSONSetMemoryStatsPeriod(qemuMonitorPtr mon,
                                        char *balloonpath,
                                        int period);
int qemuMonitorJSONGetBlockInfo(qemuMonitorPtr mon,
                                virHashTablePtr table);
int qemuMonitorJSONGetBlockStatsInfo(qemuMonitorPtr mon,
                                     const char *dev_name,
                                     long long *rd_req,
                                     long long *rd_bytes,
                                     long long *rd_total_times,
                                     long long *wr_req,
                                     long long *wr_bytes,
                                     long long *wr_total_times,
                                     long long *flush_req,
                                     long long *flush_total_times,
                                     long long *errs);
int qemuMonitorJSONGetAllBlockStatsInfo(qemuMonitorPtr mon,
                                        virHashTablePtr *ret_stats,
                                        bool backingChain);
int qemuMonitorJSONBlockStatsUpdateCapacity(qemuMonitorPtr mon,
                                            virHashTablePtr stats,
                                            bool backingChain);
int qemuMonitorJSONGetBlockStatsParamsNumber(qemuMonitorPtr mon,
                                             int *nparams);
int qemuMonitorJSONGetBlockExtent(qemuMonitorPtr mon,
                                  const char *dev_name,
                                  unsigned long long *extent);
int qemuMonitorJSONBlockResize(qemuMonitorPtr mon,
                               const char *devce,
                               unsigned long long size);

int qemuMonitorJSONSetVNCPassword(qemuMonitorPtr mon,
                                  const char *password);
int qemuMonitorJSONSetPassword(qemuMonitorPtr mon,
                               const char *protocol,
                               const char *password,
                               const char *action_if_connected);
int qemuMonitorJSONExpirePassword(qemuMonitorPtr mon,
                                  const char *protocol,
                                  const char *expire_time);
int qemuMonitorJSONSetBalloon(qemuMonitorPtr mon,
                              unsigned long newmem);
int qemuMonitorJSONSetCPU(qemuMonitorPtr mon, int cpu, bool online);

int qemuMonitorJSONEjectMedia(qemuMonitorPtr mon,
                              const char *dev_name,
                              bool force);
int qemuMonitorJSONChangeMedia(qemuMonitorPtr mon,
                               const char *dev_name,
                               const char *newmedia,
                               const char *format);


int qemuMonitorJSONSaveVirtualMemory(qemuMonitorPtr mon,
                                     unsigned long long offset,
                                     size_t length,
                                     const char *path);
int qemuMonitorJSONSavePhysicalMemory(qemuMonitorPtr mon,
                                      unsigned long long offset,
                                      size_t length,
                                      const char *path);

int qemuMonitorJSONSetMigrationSpeed(qemuMonitorPtr mon,
                                     unsigned long bandwidth);

int qemuMonitorJSONSetMigrationDowntime(qemuMonitorPtr mon,
                                        unsigned long long downtime);

int qemuMonitorJSONGetMigrationCacheSize(qemuMonitorPtr mon,
                                         unsigned long long *cacheSize);
int qemuMonitorJSONSetMigrationCacheSize(qemuMonitorPtr mon,
                                         unsigned long long cacheSize);

int qemuMonitorJSONGetMigrationStatus(qemuMonitorPtr mon,
                                      qemuMonitorMigrationStatusPtr status);

int qemuMonitorJSONGetMigrationCapabilities(qemuMonitorPtr mon,
                                            char ***capabilities);
int qemuMonitorJSONGetMigrationCapability(qemuMonitorPtr mon,
                                          qemuMonitorMigrationCaps capability);
int qemuMonitorJSONSetMigrationCapability(qemuMonitorPtr mon,
                                          qemuMonitorMigrationCaps capability,
                                          bool state);

int qemuMonitorJSONMigrate(qemuMonitorPtr mon,
                           unsigned int flags,
                           const char *uri);
int qemuMonitorJSONGetSpiceMigrationStatus(qemuMonitorPtr mon,
                                           bool *spice_migrated);


int qemuMonitorJSONMigrateCancel(qemuMonitorPtr mon);

int qemuMonitorJSONGetDumpGuestMemoryCapability(qemuMonitorPtr mon,
                                                const char *capability);

int qemuMonitorJSONDump(qemuMonitorPtr mon,
                        const char *protocol,
                        const char *dumpformat);

int qemuMonitorJSONGraphicsRelocate(qemuMonitorPtr mon,
                                    int type,
                                    const char *hostname,
                                    int port,
                                    int tlsPort,
                                    const char *tlsSubject);

int qemuMonitorJSONAddUSBDisk(qemuMonitorPtr mon,
                              const char *path);

int qemuMonitorJSONAddUSBDeviceExact(qemuMonitorPtr mon,
                                     int bus,
                                     int dev);
int qemuMonitorJSONAddUSBDeviceMatch(qemuMonitorPtr mon,
                                     int vendor,
                                     int product);


int qemuMonitorJSONAddPCIHostDevice(qemuMonitorPtr mon,
                                    virDevicePCIAddress *hostAddr,
                                    virDevicePCIAddress *guestAddr);

int qemuMonitorJSONAddPCIDisk(qemuMonitorPtr mon,
                              const char *path,
                              const char *bus,
                              virDevicePCIAddress *guestAddr);

int qemuMonitorJSONAddPCINetwork(qemuMonitorPtr mon,
                                 const char *nicstr,
                                 virDevicePCIAddress *guestAddr);

int qemuMonitorJSONRemovePCIDevice(qemuMonitorPtr mon,
                                   virDevicePCIAddress *guestAddr);

int qemuMonitorJSONSendFileHandle(qemuMonitorPtr mon,
                                  const char *fdname,
                                  int fd);
int qemuMonitorJSONAddFd(qemuMonitorPtr mon, int fdset, int fd,
                         const char *name);

int qemuMonitorJSONCloseFileHandle(qemuMonitorPtr mon,
                                   const char *fdname);
int qemuMonitorJSONRemoveFd(qemuMonitorPtr mon, int fdset, int fd);

int qemuMonitorJSONAddNetdev(qemuMonitorPtr mon,
                             const char *netdevstr);

int qemuMonitorJSONRemoveNetdev(qemuMonitorPtr mon,
                                const char *alias);

int qemuMonitorJSONQueryRxFilter(qemuMonitorPtr mon, const char *alias,
                                 virNetDevRxFilterPtr *filter);

int qemuMonitorJSONGetChardevInfo(qemuMonitorPtr mon,
                                  virHashTablePtr info);

int qemuMonitorJSONAttachPCIDiskController(qemuMonitorPtr mon,
                                           const char *bus,
                                           virDevicePCIAddress *guestAddr);

int qemuMonitorJSONGetAllPCIAddresses(qemuMonitorPtr mon,
                                      qemuMonitorPCIAddress **addrs);

int qemuMonitorJSONAddDevice(qemuMonitorPtr mon,
                             const char *devicestr);

int qemuMonitorJSONDelDevice(qemuMonitorPtr mon,
                             const char *devalias);

int qemuMonitorJSONAddObject(qemuMonitorPtr mon,
                             const char *type,
                             const char *objalias,
                             virJSONValuePtr props);

int qemuMonitorJSONDelObject(qemuMonitorPtr mon,
                             const char *objalias);

int qemuMonitorJSONAddDrive(qemuMonitorPtr mon,
                            const char *drivestr);

int qemuMonitorJSONDriveDel(qemuMonitorPtr mon,
                            const char *drivestr);

int qemuMonitorJSONSetDrivePassphrase(qemuMonitorPtr mon,
                                      const char *alias,
                                      const char *passphrase);

int qemuMonitorJSONCreateSnapshot(qemuMonitorPtr mon, const char *name);
int qemuMonitorJSONLoadSnapshot(qemuMonitorPtr mon, const char *name);
int qemuMonitorJSONDeleteSnapshot(qemuMonitorPtr mon, const char *name);

int qemuMonitorJSONDiskSnapshot(qemuMonitorPtr mon,
                                virJSONValuePtr actions,
                                const char *device,
                                const char *file,
                                const char *format,
                                bool reuse)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(3)
    ATTRIBUTE_NONNULL(4) ATTRIBUTE_NONNULL(5);
int qemuMonitorJSONTransaction(qemuMonitorPtr mon, virJSONValuePtr actions)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2);
int qemuMonitorJSONDriveMirror(qemuMonitorPtr mon,
                               const char *device,
                               const char *file,
                               const char *format,
                               unsigned long long speed,
                               unsigned int granularity,
                               unsigned long long buf_size,
                               unsigned int flags)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2) ATTRIBUTE_NONNULL(3);
int qemuMonitorJSONDrivePivot(qemuMonitorPtr mon,
                              const char *device,
                              const char *file,
                              const char *format)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2);

int qemuMonitorJSONBlockCommit(qemuMonitorPtr mon,
                               const char *device,
                               const char *top,
                               const char *base,
                               const char *backingName,
                               unsigned long long bandwidth)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2);

int qemuMonitorJSONArbitraryCommand(qemuMonitorPtr mon,
                                    const char *cmd_str,
                                    char **reply_str,
                                    bool hmp);

int qemuMonitorJSONInjectNMI(qemuMonitorPtr mon);

int qemuMonitorJSONSendKey(qemuMonitorPtr mon,
                           unsigned int holdtime,
                           unsigned int *keycodes,
                           unsigned int nkeycodes);

int qemuMonitorJSONScreendump(qemuMonitorPtr mon,
                              const char *file);

int qemuMonitorJSONBlockJob(qemuMonitorPtr mon,
                            const char *device,
                            const char *base,
                            const char *backingName,
                            unsigned long long speed,
                            qemuMonitorBlockJobCmd mode,
                            bool modern)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2);

int qemuMonitorJSONBlockJobInfo(qemuMonitorPtr mon,
                                const char *device,
                                virDomainBlockJobInfoPtr info,
                                unsigned long long *bandwidth)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2) ATTRIBUTE_NONNULL(3);

int qemuMonitorJSONSetLink(qemuMonitorPtr mon,
                           const char *name,
                           virDomainNetInterfaceLinkState state);

int qemuMonitorJSONOpenGraphics(qemuMonitorPtr mon,
                                const char *protocol,
                                const char *fdname,
                                bool skipauth);

int qemuMonitorJSONSetBlockIoThrottle(qemuMonitorPtr mon,
                                      const char *device,
                                      virDomainBlockIoTuneInfoPtr info,
                                      bool supportMaxOptions);

int qemuMonitorJSONGetBlockIoThrottle(qemuMonitorPtr mon,
                                      const char *device,
                                      virDomainBlockIoTuneInfoPtr reply,
                                      bool supportMaxOptions);

int qemuMonitorJSONSystemWakeup(qemuMonitorPtr mon);

int qemuMonitorJSONGetVersion(qemuMonitorPtr mon,
                              int *major,
                              int *minor,
                              int *micro,
                              char **package)
    ATTRIBUTE_NONNULL(2) ATTRIBUTE_NONNULL(3) ATTRIBUTE_NONNULL(4);

int qemuMonitorJSONGetMachines(qemuMonitorPtr mon,
                               qemuMonitorMachineInfoPtr **machines)
    ATTRIBUTE_NONNULL(2);

int qemuMonitorJSONGetCPUDefinitions(qemuMonitorPtr mon,
                                     char ***cpus)
    ATTRIBUTE_NONNULL(2);

int qemuMonitorJSONGetCommands(qemuMonitorPtr mon,
                               char ***commands)
    ATTRIBUTE_NONNULL(2);
int qemuMonitorJSONGetEvents(qemuMonitorPtr mon,
                             char ***events)
    ATTRIBUTE_NONNULL(2);
int qemuMonitorJSONGetCommandLineOptionParameters(qemuMonitorPtr mon,
                                                  const char *option,
                                                  char ***params,
                                                  bool *found)
    ATTRIBUTE_NONNULL(2) ATTRIBUTE_NONNULL(3);

int qemuMonitorJSONGetKVMState(qemuMonitorPtr mon,
                               bool *enabled,
                               bool *present)
    ATTRIBUTE_NONNULL(2) ATTRIBUTE_NONNULL(3);

int qemuMonitorJSONGetObjectTypes(qemuMonitorPtr mon,
                                  char ***types)
    ATTRIBUTE_NONNULL(2);

/* ListPath structures and API's are public only for qemumonitorjsontest */
typedef struct _qemuMonitorJSONListPath qemuMonitorJSONListPath;
typedef qemuMonitorJSONListPath *qemuMonitorJSONListPathPtr;

struct _qemuMonitorJSONListPath {
    char *name;
    char *type;
};

int qemuMonitorJSONGetObjectListPaths(qemuMonitorPtr mon,
                                      const char *path,
                                      qemuMonitorJSONListPathPtr **paths)
    ATTRIBUTE_NONNULL(2) ATTRIBUTE_NONNULL(3);

void qemuMonitorJSONListPathFree(qemuMonitorJSONListPathPtr paths);

/* ObjectProperty structures and Get/Set API's are public only
 * for qemumonitorjsontest
 */
/* Flags for the 'type' field in _qemuMonitorJSONObjectProperty */
typedef enum {
    QEMU_MONITOR_OBJECT_PROPERTY_BOOLEAN=1,
    QEMU_MONITOR_OBJECT_PROPERTY_INT,
    QEMU_MONITOR_OBJECT_PROPERTY_LONG,
    QEMU_MONITOR_OBJECT_PROPERTY_UINT,
    QEMU_MONITOR_OBJECT_PROPERTY_ULONG,
    QEMU_MONITOR_OBJECT_PROPERTY_DOUBLE,
    QEMU_MONITOR_OBJECT_PROPERTY_STRING,

    QEMU_MONITOR_OBJECT_PROPERTY_LAST
} qemuMonitorJSONObjectPropertyType;

typedef struct _qemuMonitorJSONObjectProperty qemuMonitorJSONObjectProperty;
typedef qemuMonitorJSONObjectProperty *qemuMonitorJSONObjectPropertyPtr;
struct _qemuMonitorJSONObjectProperty {
    int type;    /* qemuMonitorJSONObjectPropertyType */
    union {
        bool b;
        int iv;
        long long l;
        unsigned int ui;
        unsigned long long ul;
        double d;
        char *str;
    } val;
};

int qemuMonitorJSONGetObjectProperty(qemuMonitorPtr mon,
                                     const char *path,
                                     const char *property,
                                     qemuMonitorJSONObjectPropertyPtr prop)
    ATTRIBUTE_NONNULL(2) ATTRIBUTE_NONNULL(3) ATTRIBUTE_NONNULL(4);

int qemuMonitorJSONSetObjectProperty(qemuMonitorPtr mon,
                                     const char *path,
                                     const char *property,
                                     qemuMonitorJSONObjectPropertyPtr prop)
    ATTRIBUTE_NONNULL(2) ATTRIBUTE_NONNULL(3) ATTRIBUTE_NONNULL(4);

int qemuMonitorJSONGetObjectProps(qemuMonitorPtr mon,
                                  const char *type,
                                  char ***props)
    ATTRIBUTE_NONNULL(2) ATTRIBUTE_NONNULL(3);
char *qemuMonitorJSONGetTargetArch(qemuMonitorPtr mon);

int qemuMonitorJSONNBDServerStart(qemuMonitorPtr mon,
                                  const char *host,
                                  unsigned int port);
int qemuMonitorJSONNBDServerAdd(qemuMonitorPtr mon,
                                const char *deviceID,
                                bool writable);
int qemuMonitorJSONNBDServerStop(qemuMonitorPtr mon);
int qemuMonitorJSONGetTPMModels(qemuMonitorPtr mon,
                                char ***tpmmodels)
    ATTRIBUTE_NONNULL(2);

int qemuMonitorJSONGetTPMTypes(qemuMonitorPtr mon,
                               char ***tpmtypes)
    ATTRIBUTE_NONNULL(2);

int qemuMonitorJSONAttachCharDev(qemuMonitorPtr mon,
                                 const char *chrID,
                                 virDomainChrSourceDefPtr chr);
int qemuMonitorJSONDetachCharDev(qemuMonitorPtr mon,
                                 const char *chrID);

int qemuMonitorJSONGetDeviceAliases(qemuMonitorPtr mon,
                                    char ***aliases);

int qemuMonitorJSONGetGuestCPU(qemuMonitorPtr mon,
                               virArch arch,
                               virCPUDataPtr *data);

int qemuMonitorJSONRTCResetReinjection(qemuMonitorPtr mon);

int qemuMonitorJSONGetIOThreads(qemuMonitorPtr mon,
                                qemuMonitorIOThreadsInfoPtr **iothreads)
    ATTRIBUTE_NONNULL(2);
#endif /* QEMU_MONITOR_JSON_H */
