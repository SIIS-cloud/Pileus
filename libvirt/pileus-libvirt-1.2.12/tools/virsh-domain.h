/*
 * virsh-domain.h: Commands to manage domain
 *
 * Copyright (C) 2005, 2007-2012 Red Hat, Inc.
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
 *  Daniel Veillard <veillard@redhat.com>
 *  Karel Zak <kzak@redhat.com>
 *  Daniel P. Berrange <berrange@redhat.com>
 *
 */

#ifndef VIRSH_DOMAIN_H
# define VIRSH_DOMAIN_H

# include "virsh.h"

virDomainPtr vshLookupDomainBy(vshControl *ctl,
                               const char *name,
                               unsigned int flags);

virDomainPtr vshCommandOptDomainBy(vshControl *ctl, const vshCmd *cmd,
                                   const char **name, unsigned int flags);

/* default is lookup by Id, Name and UUID */
# define vshCommandOptDomain(_ctl, _cmd, _name)                      \
    vshCommandOptDomainBy(_ctl, _cmd, _name, VSH_BYID|VSH_BYUUID|VSH_BYNAME)

extern const vshCmdDef domManagementCmds[];

#endif /* VIRSH_DOMAIN_H */
