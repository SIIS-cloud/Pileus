/*
 * bhyve_capabilities.h: bhyve capabilities module
 *
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

#ifndef _BHYVE_CAPABILITIES
# define _BHYVE_CAPABILITIES

# include "capabilities.h"

virCapsPtr virBhyveCapsBuild(void);

/* These are bit flags: */
typedef enum {
    BHYVE_GRUB_CAP_CONSDEV = 1,
} virBhyveGrubCapsFlags;

int virBhyveProbeGrubCaps(virBhyveGrubCapsFlags *caps);

#endif
