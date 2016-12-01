/*
 * lock_daemon_config.h: virtlockd config file handling
 *
 * Copyright (C) 2006-2012 Red Hat, Inc.
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

#ifndef __VIR_LOCK_DAEMON_CONFIG_H__
# define __VIR_LOCK_DAEMON_CONFIG_H__

# include "internal.h"

typedef struct _virLockDaemonConfig virLockDaemonConfig;
typedef virLockDaemonConfig *virLockDaemonConfigPtr;

struct _virLockDaemonConfig {
    int log_level;
    char *log_filters;
    char *log_outputs;
    int max_clients;
};


int virLockDaemonConfigFilePath(bool privileged, char **configfile);
virLockDaemonConfigPtr virLockDaemonConfigNew(bool privileged);
void virLockDaemonConfigFree(virLockDaemonConfigPtr data);
int virLockDaemonConfigLoadFile(virLockDaemonConfigPtr data,
                                const char *filename,
                         bool allow_missing);
int virLockDaemonConfigLoadData(virLockDaemonConfigPtr data,
                                const char *filename,
                                const char *filedata);

#endif /* __LIBVIRTD_CONFIG_H__ */
