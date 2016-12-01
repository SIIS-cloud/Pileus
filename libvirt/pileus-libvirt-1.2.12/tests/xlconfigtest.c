/*
 * xlconfigtest.c: Test backend for xl_internal config file handling
 *
 * Copyright (C) 2007, 2010-2011, 2014 Red Hat, Inc.
 * Copyright (c) 2015 SUSE LINUX Products GmbH, Nuernberg, Germany.
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
 * Author: Kiarie Kahurani <davidkiarie4@gmail.com>
 *
 */

#include <config.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "internal.h"
#include "datatypes.h"
#include "xenconfig/xen_xl.h"
#include "viralloc.h"
#include "virstring.h"
#include "testutils.h"
#include "testutilsxen.h"
#include "libxl/libxl_conf.h"

#define VIR_FROM_THIS VIR_FROM_NONE

static virCapsPtr caps;
static virDomainXMLOptionPtr xmlopt;
/*
 * parses the xml, creates a domain def and compare with equivalent xm config
 */
static int
testCompareParseXML(const char *xmcfg, const char *xml, int xendConfigVersion)
{
    char *xmlData = NULL;
    char *xmcfgData = NULL;
    char *gotxmcfgData = NULL;
    virConfPtr conf = NULL;
    virConnectPtr conn = NULL;
    int wrote = 4096;
    int ret = -1;
    virDomainDefPtr def = NULL;

    if (VIR_ALLOC_N(gotxmcfgData, wrote) < 0)
        goto fail;

    conn = virGetConnect();
    if (!conn) goto fail;

    if (virtTestLoadFile(xml, &xmlData) < 0)
        goto fail;

    if (virtTestLoadFile(xmcfg, &xmcfgData) < 0)
        goto fail;

    if (!(def = virDomainDefParseString(xmlData, caps, xmlopt,
                                        1 << VIR_DOMAIN_VIRT_XEN,
                                        VIR_DOMAIN_XML_INACTIVE)))
        goto fail;

    if (!virDomainDefCheckABIStability(def, def)) {
        fprintf(stderr, "ABI stability check failed on %s", xml);
        goto fail;
    }

    if (!(conf = xenFormatXL(def, conn,  xendConfigVersion)))
        goto fail;

    if (virConfWriteMem(gotxmcfgData, &wrote, conf) < 0)
        goto fail;
    gotxmcfgData[wrote] = '\0';

    if (STRNEQ(xmcfgData, gotxmcfgData)) {
        virtTestDifference(stderr, xmcfgData, gotxmcfgData);
        goto fail;
    }

    ret = 0;

 fail:
    VIR_FREE(xmlData);
    VIR_FREE(xmcfgData);
    VIR_FREE(gotxmcfgData);
    if (conf)
        virConfFree(conf);
    virDomainDefFree(def);
    virObjectUnref(conn);

    return ret;
}
/*
 * parses the xl config, develops domain def and compares with equivalent xm config
 */
static int
testCompareFormatXML(const char *xmcfg, const char *xml, int xendConfigVersion)
{
    char *xmlData = NULL;
    char *xmcfgData = NULL;
    char *gotxml = NULL;
    virConfPtr conf = NULL;
    int ret = -1;
    virConnectPtr conn;
    virDomainDefPtr def = NULL;

    conn = virGetConnect();
    if (!conn) goto fail;

    if (virtTestLoadFile(xml, &xmlData) < 0)
        goto fail;

    if (virtTestLoadFile(xmcfg, &xmcfgData) < 0)
        goto fail;

    if (!(conf = virConfReadMem(xmcfgData, strlen(xmcfgData), 0)))
        goto fail;

    if (!(def = xenParseXL(conf, caps, xendConfigVersion)))
        goto fail;

    if (!(gotxml = virDomainDefFormat(def, VIR_DOMAIN_XML_INACTIVE |
                                      VIR_DOMAIN_XML_SECURE)))
        goto fail;

    if (STRNEQ(xmlData, gotxml)) {
        virtTestDifference(stderr, xmlData, gotxml);
        goto fail;
    }

    ret = 0;

 fail:
    if (conf)
        virConfFree(conf);
    VIR_FREE(xmlData);
    VIR_FREE(xmcfgData);
    VIR_FREE(gotxml);
    virDomainDefFree(def);
    virObjectUnref(conn);

    return ret;
}


struct testInfo {
    const char *name;
    int version;
    int mode;
};

static int
testCompareHelper(const void *data)
{
    int result = -1;
    const struct testInfo *info = data;
    char *xml = NULL;
    char *cfg = NULL;

    if (virAsprintf(&xml, "%s/xlconfigdata/test-%s.xml",
                    abs_srcdir, info->name) < 0 ||
        virAsprintf(&cfg, "%s/xlconfigdata/test-%s.cfg",
                    abs_srcdir, info->name) < 0)
        goto cleanup;

    if (info->mode == 0)
        result = testCompareParseXML(cfg, xml, info->version);
    else
        result = testCompareFormatXML(cfg, xml, info->version);

 cleanup:
    VIR_FREE(xml);
    VIR_FREE(cfg);

    return result;
}


static int
mymain(void)
{
    int ret = 0;

    if (!(caps = testXLInitCaps()))
        return EXIT_FAILURE;

    if (!(xmlopt = libxlCreateXMLConf()))
        return EXIT_FAILURE;

#define DO_TEST(name, version)                                          \
    do {                                                                \
        struct testInfo info0 = { name, version, 0 };                   \
        struct testInfo info1 = { name, version, 1 };                   \
        if (virtTestRun("Xen XM-2-XML Parse  " name,                    \
                        testCompareHelper, &info0) < 0)                 \
            ret = -1;                                                   \
        if (virtTestRun("Xen XM-2-XML Format " name,                    \
                        testCompareHelper, &info1) < 0)                 \
            ret = -1;                                                   \
    } while (0)

    DO_TEST("new-disk", 3);
    DO_TEST("spice", 3);

    virObjectUnref(caps);
    virObjectUnref(xmlopt);

    return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

VIRT_TEST_MAIN(mymain)
