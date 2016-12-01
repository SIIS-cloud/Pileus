/*
 * Copyright (C) 2010-2011 Red Hat, Inc.
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
 * POSIX DAC security driver
 */

#include "security_driver.h"

#ifndef __VIR_SECURITY_DAC
# define __VIR_SECURITY_DAC

extern virSecurityDriver virSecurityDriverDAC;

int virSecurityDACSetUserAndGroup(virSecurityManagerPtr mgr,
                                  uid_t user,
                                  gid_t group);

void virSecurityDACSetDynamicOwnership(virSecurityManagerPtr mgr,
                                       bool dynamic);

void virSecurityDACSetChownCallback(virSecurityManagerPtr mgr,
                                    virSecurityManagerDACChownCallback chownCallback);

#endif /* __VIR_SECURITY_DAC */
