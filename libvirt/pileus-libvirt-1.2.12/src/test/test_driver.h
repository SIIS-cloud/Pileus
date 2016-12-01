/*
 * test_driver.h: A "mock" hypervisor for use by application unit tests
 *
 * Copyright (C) 2006-2006 Red Hat, Inc.
 * Copyright (C) 2006  Daniel P. Berrange
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
 * Daniel Berrange <berrange@redhat.com>
 */

#ifndef __VIR_TEST_INTERNAL_H__
# define __VIR_TEST_INTERNAL_H__

# include "internal.h"

int testRegister(void);

#endif /* __VIR_TEST_INTERNAL_H__ */
