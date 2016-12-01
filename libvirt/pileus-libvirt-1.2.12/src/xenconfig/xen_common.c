/*
 * xen_common.c: Parsing and formatting functions for config common
 * between XM and XL
 *
 * Copyright (C) 2014 SUSE LINUX Products GmbH, Nuernberg, Germany.
 * Copyright (C) 2006-2007, 2009-2014 Red Hat, Inc.
 * Copyright (C) 2011 Univention GmbH
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
 * Author: Markus Groß <gross@univention.de>
 * Author: Jim Fehlig <jfehlig@suse.com>
 */

#include <config.h>

#include "internal.h"
#include "virerror.h"
#include "virconf.h"
#include "viralloc.h"
#include "viruuid.h"
#include "count-one-bits.h"
#include "xenxs_private.h"
#include "domain_conf.h"
#include "virstring.h"
#include "xen_common.h"


/*
 * Convenience method to grab a long int from the config file object
 */
int
xenConfigGetBool(virConfPtr conf,
                 const char *name,
                 int *value,
                 int def)
{
    virConfValuePtr val;

    *value = 0;
    if (!(val = virConfGetValue(conf, name))) {
        *value = def;
        return 0;
    }

    if (val->type == VIR_CONF_ULONG) {
        *value = val->l ? 1 : 0;
    } else if (val->type == VIR_CONF_STRING) {
        *value = STREQ(val->str, "1") ? 1 : 0;
    } else {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("config value %s was malformed"), name);
        return -1;
    }
    return 0;
}


/*
 * Convenience method to grab a int from the config file object
 */
int
xenConfigGetULong(virConfPtr conf,
                  const char *name,
                  unsigned long *value,
                  unsigned long def)
{
    virConfValuePtr val;

    *value = 0;
    if (!(val = virConfGetValue(conf, name))) {
        *value = def;
        return 0;
    }

    if (val->type == VIR_CONF_ULONG) {
        *value = val->l;
    } else if (val->type == VIR_CONF_STRING) {
        if (virStrToLong_ul(val->str, NULL, 10, value) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("config value %s was malformed"), name);
            return -1;
        }
    } else {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("config value %s was malformed"), name);
        return -1;
    }
    return 0;
}


/*
 * Convenience method to grab a int from the config file object
 */
static int
xenConfigGetULongLong(virConfPtr conf,
                      const char *name,
                      unsigned long long *value,
                      unsigned long long def)
{
    virConfValuePtr val;

    *value = 0;
    if (!(val = virConfGetValue(conf, name))) {
        *value = def;
        return 0;
    }

    if (val->type == VIR_CONF_ULONG) {
        *value = val->l;
    } else if (val->type == VIR_CONF_STRING) {
        if (virStrToLong_ull(val->str, NULL, 10, value) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("config value %s was malformed"), name);
            return -1;
        }
    } else {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("config value %s was malformed"), name);
        return -1;
    }
    return 0;
}


static int
xenConfigCopyStringInternal(virConfPtr conf,
                            const char *name,
                            char **value,
                            int allowMissing)
{
    virConfValuePtr val;

    *value = NULL;
    if (!(val = virConfGetValue(conf, name))) {
        if (allowMissing)
            return 0;
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("config value %s was missing"), name);
        return -1;
    }

    if (val->type != VIR_CONF_STRING) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("config value %s was not a string"), name);
        return -1;
    }
    if (!val->str) {
        if (allowMissing)
            return 0;
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("config value %s was missing"), name);
        return -1;
    }

    return VIR_STRDUP(*value, val->str);
}


static int
xenConfigCopyString(virConfPtr conf, const char *name, char **value)
{
    return xenConfigCopyStringInternal(conf, name, value, 0);
}


int
xenConfigCopyStringOpt(virConfPtr conf, const char *name, char **value)
{
    return xenConfigCopyStringInternal(conf, name, value, 1);
}


/*
 * Convenience method to grab a string UUID from the config file object
 */
static int
xenConfigGetUUID(virConfPtr conf, const char *name, unsigned char *uuid)
{
    virConfValuePtr val;

    if (!uuid || !name || !conf) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("Arguments must be non null"));
        return -1;
    }

    if (!(val = virConfGetValue(conf, name))) {
        if (virUUIDGenerate(uuid)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("Failed to generate UUID"));
            return -1;
        } else {
            return 0;
        }
    }

    if (val->type != VIR_CONF_STRING) {
        virReportError(VIR_ERR_CONF_SYNTAX,
                       _("config value %s not a string"), name);
        return -1;
    }

    if (!val->str) {
        virReportError(VIR_ERR_CONF_SYNTAX,
                       _("%s can't be empty"), name);
        return -1;
    }

    if (virUUIDParse(val->str, uuid) < 0) {
        virReportError(VIR_ERR_CONF_SYNTAX,
                       _("%s not parseable"), val->str);
        return -1;
    }

    return 0;
}


/*
 * Convenience method to grab a string from the config file object
 */
int
xenConfigGetString(virConfPtr conf,
                   const char *name,
                   const char **value,
                   const char *def)
{
    virConfValuePtr val;

    *value = NULL;
    if (!(val = virConfGetValue(conf, name))) {
        *value = def;
        return 0;
    }

    if (val->type != VIR_CONF_STRING) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("config value %s was malformed"), name);
        return -1;
    }
    if (!val->str)
        *value = def;
    else
        *value = val->str;
    return 0;
}


int
xenConfigSetInt(virConfPtr conf, const char *setting, long long l)
{
    virConfValuePtr value = NULL;

    if ((long) l != l) {
        virReportError(VIR_ERR_OVERFLOW, _("failed to store %lld to %s"),
                       l, setting);
        return -1;
    }
    if (VIR_ALLOC(value) < 0)
        return -1;

    value->type = VIR_CONF_LONG;
    value->next = NULL;
    value->l = l;

    return virConfSetValue(conf, setting, value);
}


int
xenConfigSetString(virConfPtr conf, const char *setting, const char *str)
{
    virConfValuePtr value = NULL;

    if (VIR_ALLOC(value) < 0)
        return -1;

    value->type = VIR_CONF_STRING;
    value->next = NULL;
    if (VIR_STRDUP(value->str, str) < 0) {
        VIR_FREE(value);
        return -1;
    }

    return virConfSetValue(conf, setting, value);
}


static int
xenParseMem(virConfPtr conf, virDomainDefPtr def)
{
    if (xenConfigGetULongLong(conf, "memory", &def->mem.cur_balloon,
                                MIN_XEN_GUEST_SIZE * 2) < 0)
        return -1;

    if (xenConfigGetULongLong(conf, "maxmem", &def->mem.max_balloon,
                                def->mem.cur_balloon) < 0)
        return -1;

    def->mem.cur_balloon *= 1024;
    def->mem.max_balloon *= 1024;

    return 0;
}


static int
xenParseTimeOffset(virConfPtr conf, virDomainDefPtr def,
                     int xendConfigVersion)
{
    int vmlocaltime;

    if (xenConfigGetBool(conf, "localtime", &vmlocaltime, 0) < 0)
        return -1;

    if (STREQ(def->os.type, "hvm")) {
        /* only managed HVM domains since 3.1.0 have persistent rtc_timeoffset */
        if (xendConfigVersion < XEND_CONFIG_VERSION_3_1_0) {
            if (vmlocaltime)
                def->clock.offset = VIR_DOMAIN_CLOCK_OFFSET_LOCALTIME;
            else
                def->clock.offset = VIR_DOMAIN_CLOCK_OFFSET_UTC;
            def->clock.data.utc_reset = true;
        } else {
            unsigned long rtc_timeoffset;
            def->clock.offset = VIR_DOMAIN_CLOCK_OFFSET_VARIABLE;
            if (xenConfigGetULong(conf, "rtc_timeoffset", &rtc_timeoffset, 0) < 0)
                return -1;

            def->clock.data.variable.adjustment = (int)rtc_timeoffset;
            def->clock.data.variable.basis = vmlocaltime ?
                VIR_DOMAIN_CLOCK_BASIS_LOCALTIME :
                VIR_DOMAIN_CLOCK_BASIS_UTC;
        }
    } else {
        /* PV domains do not have an emulated RTC and the offset is fixed. */
        def->clock.offset = vmlocaltime ?
            VIR_DOMAIN_CLOCK_OFFSET_LOCALTIME :
            VIR_DOMAIN_CLOCK_OFFSET_UTC;
        def->clock.data.utc_reset = true;
    } /* !hvm */

    return 0;
}


static int
xenParseEventsActions(virConfPtr conf, virDomainDefPtr def)
{
    const char *str = NULL;

    if (xenConfigGetString(conf, "on_poweroff", &str, "destroy") < 0)
        return -1;

    if ((def->onPoweroff = virDomainLifecycleTypeFromString(str)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected value %s for on_poweroff"), str);
        return -1;
    }

    if (xenConfigGetString(conf, "on_reboot", &str, "restart") < 0)
        return -1;

    if ((def->onReboot = virDomainLifecycleTypeFromString(str)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected value %s for on_reboot"), str);
        return -1;
    }

    if (xenConfigGetString(conf, "on_crash", &str, "restart") < 0)
        return -1;

    if ((def->onCrash = virDomainLifecycleCrashTypeFromString(str)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected value %s for on_crash"), str);
        return -1;
    }

    return 0;
}


static int
xenParsePCI(virConfPtr conf, virDomainDefPtr def)
{
    virConfValuePtr list = virConfGetValue(conf, "pci");
    virDomainHostdevDefPtr hostdev = NULL;

    if (list && list->type == VIR_CONF_LIST) {
        list = list->list;
        while (list) {
            char domain[5];
            char bus[3];
            char slot[3];
            char func[2];
            char *key, *nextkey;
            int domainID;
            int busID;
            int slotID;
            int funcID;

            domain[0] = bus[0] = slot[0] = func[0] = '\0';

            if ((list->type != VIR_CONF_STRING) || (list->str == NULL))
                goto skippci;
            /* pci=['0000:00:1b.0','0000:00:13.0'] */
            if (!(key = list->str))
                goto skippci;
            if (!(nextkey = strchr(key, ':')))
                goto skippci;
            if (virStrncpy(domain, key, (nextkey - key), sizeof(domain)) == NULL) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Domain %s too big for destination"), key);
                goto skippci;
            }

            key = nextkey + 1;
            if (!(nextkey = strchr(key, ':')))
                goto skippci;
            if (virStrncpy(bus, key, (nextkey - key), sizeof(bus)) == NULL) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Bus %s too big for destination"), key);
                goto skippci;
            }

            key = nextkey + 1;
            if (!(nextkey = strchr(key, '.')))
                goto skippci;
            if (virStrncpy(slot, key, (nextkey - key), sizeof(slot)) == NULL) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Slot %s too big for destination"), key);
                goto skippci;
            }

            key = nextkey + 1;
            if (strlen(key) != 1)
                goto skippci;
            if (virStrncpy(func, key, 1, sizeof(func)) == NULL) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Function %s too big for destination"), key);
                goto skippci;
            }

            if (virStrToLong_i(domain, NULL, 16, &domainID) < 0)
                goto skippci;
            if (virStrToLong_i(bus, NULL, 16, &busID) < 0)
                goto skippci;
            if (virStrToLong_i(slot, NULL, 16, &slotID) < 0)
                goto skippci;
            if (virStrToLong_i(func, NULL, 16, &funcID) < 0)
                goto skippci;
            if (!(hostdev = virDomainHostdevDefAlloc()))
               return -1;

            hostdev->managed = false;
            hostdev->source.subsys.type = VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI;
            hostdev->source.subsys.u.pci.addr.domain = domainID;
            hostdev->source.subsys.u.pci.addr.bus = busID;
            hostdev->source.subsys.u.pci.addr.slot = slotID;
            hostdev->source.subsys.u.pci.addr.function = funcID;

            if (VIR_APPEND_ELEMENT(def->hostdevs, def->nhostdevs, hostdev) < 0) {
                virDomainHostdevDefFree(hostdev);
                return -1;
            }

        skippci:
            list = list->next;
        }
    }

    return 0;
}


static int
xenParseCPUFeatures(virConfPtr conf, virDomainDefPtr def)
{
    unsigned long count = 0;
    const char *str = NULL;
    int val = 0;

    if (xenConfigGetULong(conf, "vcpus", &count, 1) < 0 ||
        MAX_VIRT_CPUS < count)
        return -1;

    def->maxvcpus = count;
    if (xenConfigGetULong(conf, "vcpu_avail", &count, -1) < 0)
        return -1;

    def->vcpus = MIN(count_one_bits_l(count), def->maxvcpus);
    if (xenConfigGetString(conf, "cpus", &str, NULL) < 0)
        return -1;

    if (str && (virBitmapParse(str, 0, &def->cpumask, 4096) < 0))
        return -1;

    if (STREQ(def->os.type, "hvm")) {
        if (xenConfigGetBool(conf, "pae", &val, 1) < 0)
            return -1;

        else if (val)
            def->features[VIR_DOMAIN_FEATURE_PAE] = VIR_TRISTATE_SWITCH_ON;
        if (xenConfigGetBool(conf, "acpi", &val, 1) < 0)
            return -1;

        else if (val)
            def->features[VIR_DOMAIN_FEATURE_ACPI] = VIR_TRISTATE_SWITCH_ON;
        if (xenConfigGetBool(conf, "apic", &val, 1) < 0)
            return -1;

        else if (val)
            def->features[VIR_DOMAIN_FEATURE_APIC] = VIR_TRISTATE_SWITCH_ON;
        if (xenConfigGetBool(conf, "hap", &val, 0) < 0)
            return -1;

        else if (val)
            def->features[VIR_DOMAIN_FEATURE_HAP] = VIR_TRISTATE_SWITCH_ON;
        if (xenConfigGetBool(conf, "viridian", &val, 0) < 0)
            return -1;

        else if (val)
            def->features[VIR_DOMAIN_FEATURE_VIRIDIAN] = VIR_TRISTATE_SWITCH_ON;

        if (xenConfigGetBool(conf, "hpet", &val, -1) < 0)
            return -1;

        if (val != -1) {
            virDomainTimerDefPtr timer;

            if (VIR_ALLOC_N(def->clock.timers, 1) < 0 ||
                VIR_ALLOC(timer) < 0)
                return -1;

            timer->name = VIR_DOMAIN_TIMER_NAME_HPET;
            timer->present = val;
            timer->tickpolicy = -1;

            def->clock.ntimers = 1;
            def->clock.timers[0] = timer;
        }
    }

    return 0;
}


#define MAX_VFB 1024

static int
xenParseVfb(virConfPtr conf, virDomainDefPtr def, int xendConfigVersion)
{
    int val;
    char *listenAddr = NULL;
    int hvm = STREQ(def->os.type, "hvm");
    virConfValuePtr list;
    virDomainGraphicsDefPtr graphics = NULL;

    if (hvm || xendConfigVersion < XEND_CONFIG_VERSION_3_0_4) {
        if (xenConfigGetBool(conf, "vnc", &val, 0) < 0)
            goto cleanup;
        if (val) {
            if (VIR_ALLOC(graphics) < 0)
                goto cleanup;
            graphics->type = VIR_DOMAIN_GRAPHICS_TYPE_VNC;
            if (xenConfigGetBool(conf, "vncunused", &val, 1) < 0)
                goto cleanup;
            graphics->data.vnc.autoport = val ? 1 : 0;
            if (!graphics->data.vnc.autoport) {
                unsigned long vncdisplay;
                if (xenConfigGetULong(conf, "vncdisplay", &vncdisplay, 0) < 0)
                    goto cleanup;
                graphics->data.vnc.port = (int)vncdisplay + 5900;
            }

            if (xenConfigCopyStringOpt(conf, "vnclisten", &listenAddr) < 0)
                goto cleanup;
            if (listenAddr &&
                virDomainGraphicsListenSetAddress(graphics, 0, listenAddr,
                                                  -1, true) < 0) {
               goto cleanup;
            }

            VIR_FREE(listenAddr);
            if (xenConfigCopyStringOpt(conf, "vncpasswd", &graphics->data.vnc.auth.passwd) < 0)
                goto cleanup;
            if (xenConfigCopyStringOpt(conf, "keymap", &graphics->data.vnc.keymap) < 0)
                goto cleanup;
            if (VIR_ALLOC_N(def->graphics, 1) < 0)
                goto cleanup;
            def->graphics[0] = graphics;
            def->ngraphics = 1;
            graphics = NULL;
        } else {
            if (xenConfigGetBool(conf, "sdl", &val, 0) < 0)
                goto cleanup;
            if (val) {
                if (VIR_ALLOC(graphics) < 0)
                    goto cleanup;
                graphics->type = VIR_DOMAIN_GRAPHICS_TYPE_SDL;
                if (xenConfigCopyStringOpt(conf, "display", &graphics->data.sdl.display) < 0)
                    goto cleanup;
                if (xenConfigCopyStringOpt(conf, "xauthority", &graphics->data.sdl.xauth) < 0)
                    goto cleanup;
                if (VIR_ALLOC_N(def->graphics, 1) < 0)
                    goto cleanup;
                def->graphics[0] = graphics;
                def->ngraphics = 1;
                graphics = NULL;
            }
        }
    }

    if (!hvm && def->graphics == NULL) { /* New PV guests use this format */
        list = virConfGetValue(conf, "vfb");
        if (list && list->type == VIR_CONF_LIST &&
            list->list && list->list->type == VIR_CONF_STRING &&
            list->list->str) {
            char vfb[MAX_VFB];
            char *key = vfb;

            if (virStrcpyStatic(vfb, list->list->str) == NULL) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("VFB %s too big for destination"),
                               list->list->str);
                goto cleanup;
            }

            if (VIR_ALLOC(graphics) < 0)
                goto cleanup;
            if (strstr(key, "type=sdl"))
                graphics->type = VIR_DOMAIN_GRAPHICS_TYPE_SDL;
            else
                graphics->type = VIR_DOMAIN_GRAPHICS_TYPE_VNC;
            while (key) {
                char *nextkey = strchr(key, ',');
                char *end = nextkey;
                if (nextkey) {
                    *end = '\0';
                    nextkey++;
                }

                if (!strchr(key, '='))
                    break;
                if (graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC) {
                    if (STRPREFIX(key, "vncunused=")) {
                        if (STREQ(key + 10, "1"))
                            graphics->data.vnc.autoport = true;
                    } else if (STRPREFIX(key, "vnclisten=")) {
                        if (virDomainGraphicsListenSetAddress(graphics, 0, key+10,
                                                              -1, true) < 0)
                            goto cleanup;
                    } else if (STRPREFIX(key, "vncpasswd=")) {
                        if (VIR_STRDUP(graphics->data.vnc.auth.passwd, key + 10) < 0)
                            goto cleanup;
                    } else if (STRPREFIX(key, "keymap=")) {
                        if (VIR_STRDUP(graphics->data.vnc.keymap, key + 7) < 0)
                            goto cleanup;
                    } else if (STRPREFIX(key, "vncdisplay=")) {
                        if (virStrToLong_i(key + 11, NULL, 10,
                                           &graphics->data.vnc.port) < 0) {
                            virReportError(VIR_ERR_INTERNAL_ERROR,
                                           _("invalid vncdisplay value '%s'"),
                                           key + 11);
                            goto cleanup;
                        }
                        graphics->data.vnc.port += 5900;
                    }
                } else {
                    if (STRPREFIX(key, "display=")) {
                        if (VIR_STRDUP(graphics->data.sdl.display, key + 8) < 0)
                            goto cleanup;
                    } else if (STRPREFIX(key, "xauthority=")) {
                        if (VIR_STRDUP(graphics->data.sdl.xauth, key + 11) < 0)
                            goto cleanup;
                    }
                }

                while (nextkey && (nextkey[0] == ',' ||
                                   nextkey[0] == ' ' ||
                                   nextkey[0] == '\t'))
                    nextkey++;
                key = nextkey;
            }
            if (VIR_ALLOC_N(def->graphics, 1) < 0)
                goto cleanup;
            def->graphics[0] = graphics;
            def->ngraphics = 1;
            graphics = NULL;
        }
    }

    return 0;

 cleanup:
    virDomainGraphicsDefFree(graphics);
    VIR_FREE(listenAddr);
    return -1;
}


static int
xenParseCharDev(virConfPtr conf, virDomainDefPtr def)
{
    const char *str;
    virConfValuePtr value = NULL;
    virDomainChrDefPtr chr = NULL;

    if (STREQ(def->os.type, "hvm")) {
        if (xenConfigGetString(conf, "parallel", &str, NULL) < 0)
            goto cleanup;
        if (str && STRNEQ(str, "none") &&
            !(chr = xenParseSxprChar(str, NULL)))
            goto cleanup;
        if (chr) {
            if (VIR_ALLOC_N(def->parallels, 1) < 0)
                goto cleanup;

            chr->deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_PARALLEL;
            chr->target.port = 0;
            def->parallels[0] = chr;
            def->nparallels++;
            chr = NULL;
        }

        /* Try to get the list of values to support multiple serial ports */
        value = virConfGetValue(conf, "serial");
        if (value && value->type == VIR_CONF_LIST) {
            int portnum = -1;

            value = value->list;
            while (value) {
                char *port = NULL;

                if ((value->type != VIR_CONF_STRING) || (value->str == NULL))
                    goto cleanup;
                port = value->str;
                portnum++;
                if (STREQ(port, "none")) {
                    value = value->next;
                    continue;
                }

                if (!(chr = xenParseSxprChar(port, NULL)))
                    goto cleanup;
                chr->deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL;
                chr->target.port = portnum;
                if (VIR_APPEND_ELEMENT(def->serials, def->nserials, chr) < 0)
                    goto cleanup;

                value = value->next;
            }
        } else {
            /* If domain is not using multiple serial ports we parse data old way */
            if (xenConfigGetString(conf, "serial", &str, NULL) < 0)
                goto cleanup;
            if (str && STRNEQ(str, "none") &&
                !(chr = xenParseSxprChar(str, NULL)))
                goto cleanup;
            if (chr) {
                if (VIR_ALLOC_N(def->serials, 1) < 0)
                    goto cleanup;
                chr->deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL;
                chr->target.port = 0;
                def->serials[0] = chr;
                def->nserials++;
            }
        }
    } else {
        if (VIR_ALLOC_N(def->consoles, 1) < 0)
            goto cleanup;
        def->nconsoles = 1;
        if (!(def->consoles[0] = xenParseSxprChar("pty", NULL)))
            goto cleanup;
        def->consoles[0]->deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE;
        def->consoles[0]->target.port = 0;
        def->consoles[0]->targetType = VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_XEN;
    }

    return 0;

 cleanup:
    virDomainChrDefFree(chr);
    return -1;
}


static int
xenParseVif(virConfPtr conf, virDomainDefPtr def)
{
    char *script = NULL;
    virDomainNetDefPtr net = NULL;
    virConfValuePtr list = virConfGetValue(conf, "vif");

    if (list && list->type == VIR_CONF_LIST) {
        list = list->list;
        while (list) {
            char model[10];
            char type[10];
            char ip[16];
            char mac[18];
            char bridge[50];
            char vifname[50];
            char *key;

            bridge[0] = '\0';
            mac[0] = '\0';
            ip[0] = '\0';
            model[0] = '\0';
            type[0] = '\0';
            vifname[0] = '\0';

            if ((list->type != VIR_CONF_STRING) || (list->str == NULL))
                goto skipnic;

            key = list->str;
            while (key) {
                char *data;
                char *nextkey = strchr(key, ',');

                if (!(data = strchr(key, '=')))
                    goto skipnic;
                data++;

                if (STRPREFIX(key, "mac=")) {
                    int len = nextkey ? (nextkey - data) : sizeof(mac) - 1;
                    if (virStrncpy(mac, data, len, sizeof(mac)) == NULL) {
                        virReportError(VIR_ERR_INTERNAL_ERROR,
                                       _("MAC address %s too big for destination"),
                                       data);
                        goto skipnic;
                    }
                } else if (STRPREFIX(key, "bridge=")) {
                    int len = nextkey ? (nextkey - data) : sizeof(bridge) - 1;
                    if (virStrncpy(bridge, data, len, sizeof(bridge)) == NULL) {
                        virReportError(VIR_ERR_INTERNAL_ERROR,
                                       _("Bridge %s too big for destination"),
                                       data);
                        goto skipnic;
                    }
                } else if (STRPREFIX(key, "script=")) {
                    int len = nextkey ? (nextkey - data) : strlen(data);
                    VIR_FREE(script);
                    if (VIR_STRNDUP(script, data, len) < 0)
                        goto cleanup;
                } else if (STRPREFIX(key, "model=")) {
                    int len = nextkey ? (nextkey - data) : sizeof(model) - 1;
                    if (virStrncpy(model, data, len, sizeof(model)) == NULL) {
                        virReportError(VIR_ERR_INTERNAL_ERROR,
                                       _("Model %s too big for destination"),
                                       data);
                        goto skipnic;
                    }
                } else if (STRPREFIX(key, "type=")) {
                    int len = nextkey ? (nextkey - data) : sizeof(type) - 1;
                    if (virStrncpy(type, data, len, sizeof(type)) == NULL) {
                        virReportError(VIR_ERR_INTERNAL_ERROR,
                                       _("Type %s too big for destination"),
                                       data);
                        goto skipnic;
                    }
                } else if (STRPREFIX(key, "vifname=")) {
                    int len = nextkey ? (nextkey - data) : sizeof(vifname) - 1;
                    if (virStrncpy(vifname, data, len, sizeof(vifname)) == NULL) {
                        virReportError(VIR_ERR_INTERNAL_ERROR,
                                       _("Vifname %s too big for destination"),
                                       data);
                        goto skipnic;
                    }
                } else if (STRPREFIX(key, "ip=")) {
                    int len = nextkey ? (nextkey - data) : sizeof(ip) - 1;
                    if (virStrncpy(ip, data, len, sizeof(ip)) == NULL) {
                        virReportError(VIR_ERR_INTERNAL_ERROR,
                                       _("IP %s too big for destination"), data);
                        goto skipnic;
                    }
                }

                while (nextkey && (nextkey[0] == ',' ||
                                   nextkey[0] == ' ' ||
                                   nextkey[0] == '\t'))
                    nextkey++;
                key = nextkey;
            }

            if (VIR_ALLOC(net) < 0)
                goto cleanup;

            if (mac[0]) {
                if (virMacAddrParse(mac, &net->mac) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("malformed mac address '%s'"), mac);
                    goto cleanup;
                }
            }

            if (bridge[0] || STREQ_NULLABLE(script, "vif-bridge") ||
                STREQ_NULLABLE(script, "vif-vnic")) {
                net->type = VIR_DOMAIN_NET_TYPE_BRIDGE;
            } else {
                net->type = VIR_DOMAIN_NET_TYPE_ETHERNET;
            }

            if (net->type == VIR_DOMAIN_NET_TYPE_BRIDGE) {
                if (bridge[0] && VIR_STRDUP(net->data.bridge.brname, bridge) < 0)
                    goto cleanup;
            }
            if (ip[0] && virDomainNetAppendIpAddress(net, ip, AF_INET, 0) < 0)
                goto cleanup;

            if (script && script[0] &&
                VIR_STRDUP(net->script, script) < 0)
                goto cleanup;

            if (model[0] &&
                VIR_STRDUP(net->model, model) < 0)
                goto cleanup;

            if (!model[0] && type[0] && STREQ(type, "netfront") &&
                VIR_STRDUP(net->model, "netfront") < 0)
                goto cleanup;

            if (vifname[0] &&
                VIR_STRDUP(net->ifname, vifname) < 0)
                goto cleanup;

            if (VIR_APPEND_ELEMENT(def->nets, def->nnets, net) < 0)
                goto cleanup;

        skipnic:
            list = list->next;
            virDomainNetDefFree(net);
            net = NULL;
            VIR_FREE(script);
        }
    }

    return 0;

 cleanup:
    virDomainNetDefFree(net);
    VIR_FREE(script);
    return -1;
}


static int
xenParseEmulatedDevices(virConfPtr conf, virDomainDefPtr def)
{
    const char *str;

    if (STREQ(def->os.type, "hvm")) {
        if (xenConfigGetString(conf, "soundhw", &str, NULL) < 0)
            return -1;

        if (str &&
            xenParseSxprSound(def, str) < 0)
            return -1;

        if (xenConfigGetString(conf, "usbdevice", &str, NULL) < 0)
            return -1;

        if (str &&
            (STREQ(str, "tablet") ||
             STREQ(str, "mouse") ||
             STREQ(str, "keyboard"))) {
            virDomainInputDefPtr input;
            if (VIR_ALLOC(input) < 0)
                return -1;

            input->bus = VIR_DOMAIN_INPUT_BUS_USB;
            if (STREQ(str, "mouse"))
                input->type = VIR_DOMAIN_INPUT_TYPE_MOUSE;
            else if (STREQ(str, "tablet"))
                input->type = VIR_DOMAIN_INPUT_TYPE_TABLET;
            else if (STREQ(str, "keyboard"))
                input->type = VIR_DOMAIN_INPUT_TYPE_KBD;
            if (VIR_ALLOC_N(def->inputs, 1) < 0) {
                virDomainInputDefFree(input);
                return -1;

            }
            def->inputs[0] = input;
            def->ninputs = 1;
        }
    }

    return 0;
}


static int
xenParseGeneralMeta(virConfPtr conf, virDomainDefPtr def, virCapsPtr caps)
{
    const char *defaultMachine;
    const char *str;
    int hvm = 0;

    if (xenConfigCopyString(conf, "name", &def->name) < 0)
        return -1;

    if (xenConfigGetUUID(conf, "uuid", def->uuid) < 0)
        return -1;

    if ((xenConfigGetString(conf, "builder", &str, "linux") == 0) &&
        STREQ(str, "hvm"))
        hvm = 1;

    if (VIR_STRDUP(def->os.type, hvm ? "hvm" : "xen") < 0)
        return -1;

    def->os.arch =
        virCapabilitiesDefaultGuestArch(caps,
                                        def->os.type,
                                        virDomainVirtTypeToString(def->virtType));
    if (!def->os.arch) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("no supported architecture for os type '%s'"),
                       def->os.type);
        return -1;
    }

    defaultMachine = virCapabilitiesDefaultGuestMachine(caps,
                                                        def->os.type,
                                                        def->os.arch,
                                                        virDomainVirtTypeToString(def->virtType));
    if (defaultMachine != NULL) {
        if (VIR_STRDUP(def->os.machine, defaultMachine) < 0)
            return -1;
    }

    return 0;
}


static int
xenParseOS(virConfPtr conf, virDomainDefPtr def)
{
    size_t i;

    if (xenConfigCopyStringOpt(conf, "device_model", &def->emulator) < 0)
        return -1;

    if (STREQ(def->os.type, "hvm")) {
        const char *boot;

        if (VIR_ALLOC(def->os.loader) < 0 ||
            xenConfigCopyString(conf, "kernel", &def->os.loader->path) < 0)
            return -1;

        if (xenConfigGetString(conf, "boot", &boot, "c") < 0)
            return -1;

        for (i = 0; i < VIR_DOMAIN_BOOT_LAST && boot[i]; i++) {
            switch (boot[i]) {
            case 'a':
                def->os.bootDevs[i] = VIR_DOMAIN_BOOT_FLOPPY;
                break;
            case 'd':
                def->os.bootDevs[i] = VIR_DOMAIN_BOOT_CDROM;
                break;
            case 'n':
                def->os.bootDevs[i] = VIR_DOMAIN_BOOT_NET;
                break;
            case 'c':
            default:
                def->os.bootDevs[i] = VIR_DOMAIN_BOOT_DISK;
                break;
            }
            def->os.nBootDevs++;
        }
    } else {
        const char *extra, *root;

        if (xenConfigCopyStringOpt(conf, "bootloader", &def->os.bootloader) < 0)
            return -1;
        if (xenConfigCopyStringOpt(conf, "bootargs", &def->os.bootloaderArgs) < 0)
            return -1;

        if (xenConfigCopyStringOpt(conf, "kernel", &def->os.kernel) < 0)
            return -1;

        if (xenConfigCopyStringOpt(conf, "ramdisk", &def->os.initrd) < 0)
            return -1;

        if (xenConfigGetString(conf, "extra", &extra, NULL) < 0)
            return -1;

        if (xenConfigGetString(conf, "root", &root, NULL) < 0)
            return -1;

        if (root) {
            if (virAsprintf(&def->os.cmdline, "root=%s %s", root, extra) < 0)
                return -1;
        } else {
            if (VIR_STRDUP(def->os.cmdline, extra) < 0)
                return -1;
        }
    }

    return 0;
}


/*
 * A convenience function for parsing all config common to both XM and XL
 */
int
xenParseConfigCommon(virConfPtr conf,
                     virDomainDefPtr def,
                     virCapsPtr caps,
                     int xendConfigVersion)
{
    if (xenParseGeneralMeta(conf, def, caps) < 0)
        return -1;

    if (xenParseOS(conf, def) < 0)
        return -1;

    if (xenParseMem(conf, def) < 0)
        return -1;

    if (xenParseEventsActions(conf, def) < 0)
        return -1;

    if (xenParseCPUFeatures(conf, def) < 0)
        return -1;

    if (xenParseTimeOffset(conf, def, xendConfigVersion) < 0)
        return -1;

    if (xenConfigCopyStringOpt(conf, "device_model", &def->emulator) < 0)
        return -1;

    if (xenParseVif(conf, def) < 0)
        return -1;

    if (xenParsePCI(conf, def) < 0)
        return -1;

    if (xenParseEmulatedDevices(conf, def) < 0)
        return -1;

    if (xenParseVfb(conf, def, xendConfigVersion) < 0)
        return -1;

    if (xenParseCharDev(conf, def) < 0)
        return -1;

    return 0;
}


static int
xenFormatSerial(virConfValuePtr list, virDomainChrDefPtr serial)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    virConfValuePtr val, tmp;
    int ret;

    if (serial) {
        ret = xenFormatSxprChr(serial, &buf);
        if (ret < 0)
            goto cleanup;
    } else {
        virBufferAddLit(&buf, "none");
    }
    if (virBufferCheckError(&buf) < 0)
        goto cleanup;

    if (VIR_ALLOC(val) < 0)
        goto cleanup;

    val->type = VIR_CONF_STRING;
    val->str = virBufferContentAndReset(&buf);
    tmp = list->list;
    while (tmp && tmp->next)
        tmp = tmp->next;
    if (tmp)
        tmp->next = val;
    else
        list->list = val;

    return 0;

 cleanup:
    virBufferFreeAndReset(&buf);
    return -1;
}


static int
xenFormatNet(virConnectPtr conn,
             virConfValuePtr list,
             virDomainNetDefPtr net,
             int hvm, int xendConfigVersion)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    virConfValuePtr val, tmp;
    char macaddr[VIR_MAC_STRING_BUFLEN];

    virBufferAsprintf(&buf, "mac=%s", virMacAddrFormat(&net->mac, macaddr));

    switch (net->type) {
    case VIR_DOMAIN_NET_TYPE_BRIDGE:
        virBufferAsprintf(&buf, ",bridge=%s", net->data.bridge.brname);
        if (net->nips == 1) {
            char *ipStr = virSocketAddrFormat(&net->ips[0]->address);
            virBufferAsprintf(&buf, ",ip=%s", ipStr);
            VIR_FREE(ipStr);
        } else if (net->nips > 1) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Driver does not support setting multiple IP addresses"));
            goto cleanup;
        }
        virBufferAsprintf(&buf, ",script=%s", DEFAULT_VIF_SCRIPT);
        break;

    case VIR_DOMAIN_NET_TYPE_ETHERNET:
        if (net->script)
            virBufferAsprintf(&buf, ",script=%s", net->script);
        if (net->nips == 1) {
            char *ipStr = virSocketAddrFormat(&net->ips[0]->address);
            virBufferAsprintf(&buf, ",ip=%s", ipStr);
            VIR_FREE(ipStr);
        } else if (net->nips > 1) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Driver does not support setting multiple IP addresses"));
            goto cleanup;
        }
        break;

    case VIR_DOMAIN_NET_TYPE_NETWORK:
    {
        virNetworkPtr network = virNetworkLookupByName(conn, net->data.network.name);
        char *bridge;
        if (!network) {
            virReportError(VIR_ERR_NO_NETWORK, "%s",
                           net->data.network.name);
            return -1;
        }
        bridge = virNetworkGetBridgeName(network);
        virObjectUnref(network);
        if (!bridge) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("network %s is not active"),
                           net->data.network.name);
            return -1;
        }

        virBufferAsprintf(&buf, ",bridge=%s", bridge);
        virBufferAsprintf(&buf, ",script=%s", DEFAULT_VIF_SCRIPT);
    }
    break;

    default:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unsupported network type %d"),
                       net->type);
        goto cleanup;
    }

    if (!hvm) {
        if (net->model != NULL)
            virBufferAsprintf(&buf, ",model=%s", net->model);
    } else {
        if (net->model != NULL && STREQ(net->model, "netfront")) {
            virBufferAddLit(&buf, ",type=netfront");
        } else {
            if (net->model != NULL)
                virBufferAsprintf(&buf, ",model=%s", net->model);

            /*
             * apparently type ioemu breaks paravirt drivers on HVM so skip this
             * from XEND_CONFIG_MAX_VERS_NET_TYPE_IOEMU
             */
            if (xendConfigVersion <= XEND_CONFIG_MAX_VERS_NET_TYPE_IOEMU)
                virBufferAddLit(&buf, ",type=ioemu");
        }
    }

    if (net->ifname)
        virBufferAsprintf(&buf, ",vifname=%s",
                          net->ifname);

    if (virBufferCheckError(&buf) < 0)
        goto cleanup;

    if (VIR_ALLOC(val) < 0)
        goto cleanup;

    val->type = VIR_CONF_STRING;
    val->str = virBufferContentAndReset(&buf);
    tmp = list->list;
    while (tmp && tmp->next)
        tmp = tmp->next;
    if (tmp)
        tmp->next = val;
    else
        list->list = val;

    return 0;

 cleanup:
    virBufferFreeAndReset(&buf);
    return -1;
}


static int
xenFormatPCI(virConfPtr conf, virDomainDefPtr def)
{

    virConfValuePtr pciVal = NULL;
    int hasPCI = 0;
    size_t i;

    for (i = 0; i < def->nhostdevs; i++)
        if (def->hostdevs[i]->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
            def->hostdevs[i]->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI)
            hasPCI = 1;

    if (!hasPCI)
        return 0;

    if (VIR_ALLOC(pciVal) < 0)
        return -1;

    pciVal->type = VIR_CONF_LIST;
    pciVal->list = NULL;

    for (i = 0; i < def->nhostdevs; i++) {
        if (def->hostdevs[i]->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
            def->hostdevs[i]->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI) {
            virConfValuePtr val, tmp;
            char *buf;

            if (virAsprintf(&buf, "%04x:%02x:%02x.%x",
                            def->hostdevs[i]->source.subsys.u.pci.addr.domain,
                            def->hostdevs[i]->source.subsys.u.pci.addr.bus,
                            def->hostdevs[i]->source.subsys.u.pci.addr.slot,
                            def->hostdevs[i]->source.subsys.u.pci.addr.function) < 0)
                goto error;

            if (VIR_ALLOC(val) < 0) {
                VIR_FREE(buf);
                goto error;
            }
            val->type = VIR_CONF_STRING;
            val->str = buf;
            tmp = pciVal->list;
            while (tmp && tmp->next)
                tmp = tmp->next;
            if (tmp)
                tmp->next = val;
            else
                pciVal->list = val;
        }
    }

    if (pciVal->list != NULL) {
        int ret = virConfSetValue(conf, "pci", pciVal);
        pciVal = NULL;
        if (ret < 0)
            return -1;
    }
    VIR_FREE(pciVal);

    return 0;

 error:
    virConfFreeValue(pciVal);
    return -1;
}


static int
xenFormatGeneralMeta(virConfPtr conf, virDomainDefPtr def)
{
    char uuid[VIR_UUID_STRING_BUFLEN];

    if (xenConfigSetString(conf, "name", def->name) < 0)
        return -1;

    virUUIDFormat(def->uuid, uuid);
    if (xenConfigSetString(conf, "uuid", uuid) < 0)
        return -1;

    return 0;
}


static int
xenFormatMem(virConfPtr conf, virDomainDefPtr def)
{
    if (xenConfigSetInt(conf, "maxmem",
                        VIR_DIV_UP(def->mem.max_balloon, 1024)) < 0)
        return -1;

    if (xenConfigSetInt(conf, "memory",
                        VIR_DIV_UP(def->mem.cur_balloon, 1024)) < 0)
        return -1;

    return 0;
}


static int
xenFormatTimeOffset(virConfPtr conf, virDomainDefPtr def, int xendConfigVersion)
{
    int vmlocaltime;

    if (xendConfigVersion < XEND_CONFIG_VERSION_3_1_0) {
        /* <3.1: UTC and LOCALTIME */
        switch (def->clock.offset) {
        case VIR_DOMAIN_CLOCK_OFFSET_UTC:
            vmlocaltime = 0;
            break;
        case VIR_DOMAIN_CLOCK_OFFSET_LOCALTIME:
            vmlocaltime = 1;
            break;
        default:
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("unsupported clock offset='%s'"),
                           virDomainClockOffsetTypeToString(def->clock.offset));
            return -1;
        }

    } else {
        if (STREQ(def->os.type, "hvm")) {
            /* >=3.1 HV: VARIABLE */
            int rtc_timeoffset;

            switch (def->clock.offset) {
            case VIR_DOMAIN_CLOCK_OFFSET_VARIABLE:
                vmlocaltime = (int)def->clock.data.variable.basis;
                rtc_timeoffset = def->clock.data.variable.adjustment;
                break;
            case VIR_DOMAIN_CLOCK_OFFSET_UTC:
                if (def->clock.data.utc_reset) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("unsupported clock adjustment='reset'"));
                    return -1;
                }
                vmlocaltime = 0;
                rtc_timeoffset = 0;
                break;
            case VIR_DOMAIN_CLOCK_OFFSET_LOCALTIME:
                if (def->clock.data.utc_reset) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("unsupported clock adjustment='reset'"));
                    return -1;
                }
                vmlocaltime = 1;
                rtc_timeoffset = 0;
                break;
            default:
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("unsupported clock offset='%s'"),
                               virDomainClockOffsetTypeToString(def->clock.offset));
                return -1;
            }
            if (xenConfigSetInt(conf, "rtc_timeoffset", rtc_timeoffset) < 0)
                return -1;

        } else {
            /* >=3.1 PV: UTC and LOCALTIME */
            switch (def->clock.offset) {
            case VIR_DOMAIN_CLOCK_OFFSET_UTC:
                vmlocaltime = 0;
                break;
            case VIR_DOMAIN_CLOCK_OFFSET_LOCALTIME:
                vmlocaltime = 1;
                break;
            default:
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("unsupported clock offset='%s'"),
                               virDomainClockOffsetTypeToString(def->clock.offset));
                return -1;
            }
        } /* !hvm */
    }

    if (xenConfigSetInt(conf, "localtime", vmlocaltime) < 0)
        return -1;

    return 0;
}


static int
xenFormatEventActions(virConfPtr conf, virDomainDefPtr def)
{
    const char *lifecycle = NULL;

    if (!(lifecycle = virDomainLifecycleTypeToString(def->onPoweroff))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected lifecycle action %d"), def->onPoweroff);
        return -1;
    }
    if (xenConfigSetString(conf, "on_poweroff", lifecycle) < 0)
        return -1;


    if (!(lifecycle = virDomainLifecycleTypeToString(def->onReboot))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected lifecycle action %d"), def->onReboot);
        return -1;
    }
    if (xenConfigSetString(conf, "on_reboot", lifecycle) < 0)
        return -1;


    if (!(lifecycle = virDomainLifecycleCrashTypeToString(def->onCrash))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected lifecycle action %d"), def->onCrash);
        return -1;
    }
    if (xenConfigSetString(conf, "on_crash", lifecycle) < 0)
        return -1;

    return 0;
}


static int
xenFormatCharDev(virConfPtr conf, virDomainDefPtr def)
{
    size_t i;

    if (STREQ(def->os.type, "hvm")) {
        if (def->nparallels) {
            virBuffer buf = VIR_BUFFER_INITIALIZER;
            char *str;
            int ret;

            ret = xenFormatSxprChr(def->parallels[0], &buf);
            str = virBufferContentAndReset(&buf);
            if (ret == 0)
                ret = xenConfigSetString(conf, "parallel", str);
            VIR_FREE(str);
            if (ret < 0)
                return -1;
        } else {
            if (xenConfigSetString(conf, "parallel", "none") < 0)
                return -1;
        }

        if (def->nserials) {
            if ((def->nserials == 1) && (def->serials[0]->target.port == 0)) {
                virBuffer buf = VIR_BUFFER_INITIALIZER;
                char *str;
                int ret;

                ret = xenFormatSxprChr(def->serials[0], &buf);
                str = virBufferContentAndReset(&buf);
                if (ret == 0)
                    ret = xenConfigSetString(conf, "serial", str);
                VIR_FREE(str);
                if (ret < 0)
                    return -1;
            } else {
                size_t j = 0;
                int maxport = -1, port;
                virConfValuePtr serialVal = NULL;

                if (VIR_ALLOC(serialVal) < 0)
                    return -1;

                serialVal->type = VIR_CONF_LIST;
                serialVal->list = NULL;

                for (i = 0; i < def->nserials; i++)
                    if (def->serials[i]->target.port > maxport)
                        maxport = def->serials[i]->target.port;

                for (port = 0; port <= maxport; port++) {
                    virDomainChrDefPtr chr = NULL;

                    for (j = 0; j < def->nserials; j++) {
                        if (def->serials[j]->target.port == port) {
                            chr = def->serials[j];
                            break;
                        }
                    }

                    if (xenFormatSerial(serialVal, chr) < 0) {
                        VIR_FREE(serialVal);
                        return -1;
                    }
                }

                if (serialVal->list != NULL) {
                    int ret = virConfSetValue(conf, "serial", serialVal);

                    serialVal = NULL;
                    if (ret < 0)
                        return -1;
                }
                VIR_FREE(serialVal);
            }
        } else {
            if (xenConfigSetString(conf, "serial", "none") < 0)
                return -1;
        }
    }

    return 0;
}


static int
xenFormatCPUAllocation(virConfPtr conf, virDomainDefPtr def)
{
    int ret = -1;
    char *cpus = NULL;

    if (xenConfigSetInt(conf, "vcpus", def->maxvcpus) < 0)
        goto cleanup;

    /* Computing the vcpu_avail bitmask works because MAX_VIRT_CPUS is
       either 32, or 64 on a platform where long is big enough.  */
    if (def->vcpus < def->maxvcpus &&
        xenConfigSetInt(conf, "vcpu_avail", (1UL << def->vcpus) - 1) < 0)
        goto cleanup;

    if ((def->cpumask != NULL) &&
        ((cpus = virBitmapFormat(def->cpumask)) == NULL)) {
        goto cleanup;
    }

    if (cpus &&
        xenConfigSetString(conf, "cpus", cpus) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_FREE(cpus);
    return ret;
}


static int
xenFormatCPUFeatures(virConfPtr conf, virDomainDefPtr def, int xendConfigVersion)
{
    size_t i;

    if (STREQ(def->os.type, "hvm")) {
        if (xenConfigSetInt(conf, "pae",
                            (def->features[VIR_DOMAIN_FEATURE_PAE] ==
                            VIR_TRISTATE_SWITCH_ON) ? 1 : 0) < 0)
            return -1;

        if (xenConfigSetInt(conf, "acpi",
                            (def->features[VIR_DOMAIN_FEATURE_ACPI] ==
                            VIR_TRISTATE_SWITCH_ON) ? 1 : 0) < 0)
            return -1;

        if (xenConfigSetInt(conf, "apic",
                            (def->features[VIR_DOMAIN_FEATURE_APIC] ==
                            VIR_TRISTATE_SWITCH_ON) ? 1 : 0) < 0)
            return -1;

        if (xendConfigVersion >= XEND_CONFIG_VERSION_3_0_4) {
            if (xenConfigSetInt(conf, "hap",
                                (def->features[VIR_DOMAIN_FEATURE_HAP] ==
                                VIR_TRISTATE_SWITCH_ON) ? 1 : 0) < 0)
                return -1;

            if (xenConfigSetInt(conf, "viridian",
                                (def->features[VIR_DOMAIN_FEATURE_VIRIDIAN] ==
                                VIR_TRISTATE_SWITCH_ON) ? 1 : 0) < 0)
                return -1;
        }

        for (i = 0; i < def->clock.ntimers; i++) {
            if (def->clock.timers[i]->name == VIR_DOMAIN_TIMER_NAME_HPET &&
                def->clock.timers[i]->present != -1 &&
                xenConfigSetInt(conf, "hpet", def->clock.timers[i]->present) < 0)
                return -1;
        }
    }

    return 0;
}


static int
xenFormatEmulator(virConfPtr conf, virDomainDefPtr def)
{
    if (def->emulator &&
        xenConfigSetString(conf, "device_model", def->emulator) < 0)
        return -1;

    return 0;
}


static int
xenFormatCDROM(virConfPtr conf, virDomainDefPtr def, int xendConfigVersion)
{
    size_t i;

    if (STREQ(def->os.type, "hvm")) {
        if (xendConfigVersion == XEND_CONFIG_VERSION_3_0_2) {
            for (i = 0; i < def->ndisks; i++) {
                if (def->disks[i]->device == VIR_DOMAIN_DISK_DEVICE_CDROM &&
                    def->disks[i]->dst &&
                    STREQ(def->disks[i]->dst, "hdc") &&
                    virDomainDiskGetSource(def->disks[i])) {
                    if (xenConfigSetString(conf, "cdrom",
                                           virDomainDiskGetSource(def->disks[i])) < 0)
                        return -1;
                    break;
                }
            }
        }
    }

    return 0;
}


static int
xenFormatOS(virConfPtr conf, virDomainDefPtr def)
{
    size_t i;

    if (STREQ(def->os.type, "hvm")) {
        char boot[VIR_DOMAIN_BOOT_LAST+1];
        if (xenConfigSetString(conf, "builder", "hvm") < 0)
            return -1;

        if (def->os.loader && def->os.loader->path &&
            xenConfigSetString(conf, "kernel", def->os.loader->path) < 0)
            return -1;

        for (i = 0; i < def->os.nBootDevs; i++) {
            switch (def->os.bootDevs[i]) {
            case VIR_DOMAIN_BOOT_FLOPPY:
                boot[i] = 'a';
                break;
            case VIR_DOMAIN_BOOT_CDROM:
                boot[i] = 'd';
                break;
            case VIR_DOMAIN_BOOT_NET:
                boot[i] = 'n';
                break;
            case VIR_DOMAIN_BOOT_DISK:
            default:
                boot[i] = 'c';
                break;
            }
        }

        if (!def->os.nBootDevs) {
            boot[0] = 'c';
            boot[1] = '\0';
        } else {
            boot[def->os.nBootDevs] = '\0';
        }

        if (xenConfigSetString(conf, "boot", boot) < 0)
            return -1;

        /* XXX floppy disks */
    } else {
        if (def->os.bootloader &&
             xenConfigSetString(conf, "bootloader", def->os.bootloader) < 0)
            return -1;

         if (def->os.bootloaderArgs &&
             xenConfigSetString(conf, "bootargs", def->os.bootloaderArgs) < 0)
            return -1;

         if (def->os.kernel &&
             xenConfigSetString(conf, "kernel", def->os.kernel) < 0)
            return -1;

         if (def->os.initrd &&
             xenConfigSetString(conf, "ramdisk", def->os.initrd) < 0)
            return -1;

         if (def->os.cmdline &&
             xenConfigSetString(conf, "extra", def->os.cmdline) < 0)
            return -1;
     } /* !hvm */

    return 0;
}


static int
xenFormatVfb(virConfPtr conf, virDomainDefPtr def, int xendConfigVersion)
{
    int hvm = STREQ(def->os.type, "hvm") ? 1 : 0;

    if (def->ngraphics == 1 &&
        def->graphics[0]->type != VIR_DOMAIN_GRAPHICS_TYPE_SPICE) {
        if (hvm || (xendConfigVersion < XEND_CONFIG_MIN_VERS_PVFB_NEWCONF)) {
            if (def->graphics[0]->type == VIR_DOMAIN_GRAPHICS_TYPE_SDL) {
                if (xenConfigSetInt(conf, "sdl", 1) < 0)
                    return -1;

                if (xenConfigSetInt(conf, "vnc", 0) < 0)
                    return -1;

                if (def->graphics[0]->data.sdl.display &&
                    xenConfigSetString(conf, "display",
                                       def->graphics[0]->data.sdl.display) < 0)
                    return -1;

                if (def->graphics[0]->data.sdl.xauth &&
                    xenConfigSetString(conf, "xauthority",
                                       def->graphics[0]->data.sdl.xauth) < 0)
                    return -1;
            } else {
                const char *listenAddr;

                if (xenConfigSetInt(conf, "sdl", 0) < 0)
                    return -1;

                if (xenConfigSetInt(conf, "vnc", 1) < 0)
                    return -1;

                if (xenConfigSetInt(conf, "vncunused",
                              def->graphics[0]->data.vnc.autoport ? 1 : 0) < 0)
                    return -1;

                if (!def->graphics[0]->data.vnc.autoport &&
                    xenConfigSetInt(conf, "vncdisplay",
                                    def->graphics[0]->data.vnc.port - 5900) < 0)
                    return -1;

                listenAddr = virDomainGraphicsListenGetAddress(def->graphics[0], 0);
                if (listenAddr &&
                    xenConfigSetString(conf, "vnclisten", listenAddr) < 0)
                    return -1;

                if (def->graphics[0]->data.vnc.auth.passwd &&
                    xenConfigSetString(conf, "vncpasswd",
                                       def->graphics[0]->data.vnc.auth.passwd) < 0)
                    return -1;

                if (def->graphics[0]->data.vnc.keymap &&
                    xenConfigSetString(conf, "keymap",
                                       def->graphics[0]->data.vnc.keymap) < 0)
                    return -1;
            }
        } else {
            virConfValuePtr vfb, disp;
            char *vfbstr = NULL;
            virBuffer buf = VIR_BUFFER_INITIALIZER;

            if (def->graphics[0]->type == VIR_DOMAIN_GRAPHICS_TYPE_SDL) {
                virBufferAddLit(&buf, "type=sdl");
                if (def->graphics[0]->data.sdl.display)
                    virBufferAsprintf(&buf, ",display=%s",
                                      def->graphics[0]->data.sdl.display);
                if (def->graphics[0]->data.sdl.xauth)
                    virBufferAsprintf(&buf, ",xauthority=%s",
                                      def->graphics[0]->data.sdl.xauth);
            } else {
                const char *listenAddr
                    = virDomainGraphicsListenGetAddress(def->graphics[0], 0);

                virBufferAddLit(&buf, "type=vnc");
                virBufferAsprintf(&buf, ",vncunused=%d",
                                  def->graphics[0]->data.vnc.autoport ? 1 : 0);
                if (!def->graphics[0]->data.vnc.autoport)
                    virBufferAsprintf(&buf, ",vncdisplay=%d",
                                      def->graphics[0]->data.vnc.port - 5900);
                if (listenAddr)
                    virBufferAsprintf(&buf, ",vnclisten=%s", listenAddr);
                if (def->graphics[0]->data.vnc.auth.passwd)
                    virBufferAsprintf(&buf, ",vncpasswd=%s",
                                      def->graphics[0]->data.vnc.auth.passwd);
                if (def->graphics[0]->data.vnc.keymap)
                    virBufferAsprintf(&buf, ",keymap=%s",
                                      def->graphics[0]->data.vnc.keymap);
            }
            if (virBufferCheckError(&buf) < 0)
                return -1;

            vfbstr = virBufferContentAndReset(&buf);

            if (VIR_ALLOC(vfb) < 0) {
                VIR_FREE(vfbstr);
                return -1;
            }

            if (VIR_ALLOC(disp) < 0) {
                VIR_FREE(vfb);
                VIR_FREE(vfbstr);
                return -1;
            }

            vfb->type = VIR_CONF_LIST;
            vfb->list = disp;
            disp->type = VIR_CONF_STRING;
            disp->str = vfbstr;

            if (virConfSetValue(conf, "vfb", vfb) < 0)
                return -1;
        }
    }

    return 0;
}


static int
xenFormatSound(virConfPtr conf, virDomainDefPtr def)
{
    if (STREQ(def->os.type, "hvm")) {
        if (def->sounds) {
            virBuffer buf = VIR_BUFFER_INITIALIZER;
            char *str = NULL;
            int ret = xenFormatSxprSound(def, &buf);

            str = virBufferContentAndReset(&buf);
            if (ret == 0)
                ret = xenConfigSetString(conf, "soundhw", str);

            VIR_FREE(str);
            if (ret < 0)
                return -1;
        }
    }

    return 0;
}


static int
xenFormatInputDevs(virConfPtr conf, virDomainDefPtr def)
{
    size_t i;

    if (STREQ(def->os.type, "hvm")) {
        for (i = 0; i < def->ninputs; i++) {
            if (def->inputs[i]->bus == VIR_DOMAIN_INPUT_BUS_USB) {
                if (xenConfigSetInt(conf, "usb", 1) < 0)
                    return -1;

                switch (def->inputs[i]->type) {
                    case VIR_DOMAIN_INPUT_TYPE_MOUSE:
                        if (xenConfigSetString(conf, "usbdevice", "mouse") < 0)
                            return -1;

                        break;
                    case VIR_DOMAIN_INPUT_TYPE_TABLET:
                        if (xenConfigSetString(conf, "usbdevice", "tablet") < 0)
                            return -1;

                        break;
                    case VIR_DOMAIN_INPUT_TYPE_KBD:
                        if (xenConfigSetString(conf, "usbdevice", "keyboard") < 0)
                            return -1;

                        break;
                }
                break;
            }
        }
    }

    return 0;
}


static int
xenFormatVif(virConfPtr conf,
             virConnectPtr conn,
             virDomainDefPtr def,
             int xendConfigVersion)
{
   virConfValuePtr netVal = NULL;
   size_t i;
   int hvm = STREQ(def->os.type, "hvm");

   if (VIR_ALLOC(netVal) < 0)
        goto cleanup;
    netVal->type = VIR_CONF_LIST;
    netVal->list = NULL;

    for (i = 0; i < def->nnets; i++) {
        if (xenFormatNet(conn, netVal, def->nets[i],
                         hvm, xendConfigVersion) < 0)
           goto cleanup;
    }

    if (netVal->list != NULL) {
        int ret = virConfSetValue(conf, "vif", netVal);
        netVal = NULL;
        if (ret < 0)
            goto cleanup;
    }

    VIR_FREE(netVal);
    return 0;

 cleanup:
    virConfFreeValue(netVal);
    return -1;
}


/*
 * A convenience function for formatting all config common to both XM and XL
 */
int
xenFormatConfigCommon(virConfPtr conf,
                      virDomainDefPtr def,
                      virConnectPtr conn,
                      int xendConfigVersion)
{
    if (xenFormatGeneralMeta(conf, def) < 0)
        return -1;

    if (xenFormatMem(conf, def) < 0)
        return -1;

    if (xenFormatCPUAllocation(conf, def) < 0)
        return -1;

    if (xenFormatOS(conf, def) < 0)
        return -1;

    if (xenFormatCPUFeatures(conf, def, xendConfigVersion) < 0)
        return -1;

    if (xenFormatCDROM(conf, def, xendConfigVersion) < 0)
        return -1;

    if (xenFormatTimeOffset(conf, def, xendConfigVersion) < 0)
        return -1;

    if (xenFormatEventActions(conf, def) < 0)
        return -1;

    if (xenFormatEmulator(conf, def) < 0)
        return -1;

    if (xenFormatInputDevs(conf, def) < 0)
        return -1;

    if (xenFormatVfb(conf, def, xendConfigVersion) < 0)
        return -1;

    if (xenFormatVif(conf, conn, def, xendConfigVersion) < 0)
        return -1;

    if (xenFormatPCI(conf, def) < 0)
        return -1;

    if (xenFormatCharDev(conf, def) < 0)
        return -1;

    if (xenFormatSound(conf, def) < 0)
        return -1;

    return 0;
}
