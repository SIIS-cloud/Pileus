/*
 * cpu_conf.h: CPU XML handling
 *
 * Copyright (C) 2009-2011, 2013, 2014 Red Hat, Inc.
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
 * Authors:
 *      Jiri Denemark <jdenemar@redhat.com>
 */

#ifndef __VIR_CPU_CONF_H__
# define __VIR_CPU_CONF_H__

# include "virutil.h"
# include "virbuffer.h"
# include "virxml.h"
# include "virbitmap.h"
# include "virarch.h"

# define VIR_CPU_VENDOR_ID_LENGTH 12

typedef enum {
    VIR_CPU_TYPE_HOST,
    VIR_CPU_TYPE_GUEST,
    VIR_CPU_TYPE_AUTO,

    VIR_CPU_TYPE_LAST
} virCPUType;

VIR_ENUM_DECL(virCPU)

typedef enum {
    VIR_CPU_MODE_CUSTOM,
    VIR_CPU_MODE_HOST_MODEL,
    VIR_CPU_MODE_HOST_PASSTHROUGH,

    VIR_CPU_MODE_LAST
} virCPUMode;

VIR_ENUM_DECL(virCPUMode)

typedef enum {
    VIR_CPU_MATCH_MINIMUM,
    VIR_CPU_MATCH_EXACT,
    VIR_CPU_MATCH_STRICT,

    VIR_CPU_MATCH_LAST
} virCPUMatch;

VIR_ENUM_DECL(virCPUMatch)

typedef enum {
    VIR_CPU_FALLBACK_ALLOW,
    VIR_CPU_FALLBACK_FORBID,

    VIR_CPU_FALLBACK_LAST
} virCPUFallback;

VIR_ENUM_DECL(virCPUFallback)

typedef enum {
    VIR_CPU_FEATURE_FORCE,
    VIR_CPU_FEATURE_REQUIRE,
    VIR_CPU_FEATURE_OPTIONAL,
    VIR_CPU_FEATURE_DISABLE,
    VIR_CPU_FEATURE_FORBID,

    VIR_CPU_FEATURE_LAST
} virCPUFeaturePolicy;

VIR_ENUM_DECL(virCPUFeaturePolicy)

typedef enum {
    VIR_MEM_ACCESS_DEFAULT,
    VIR_MEM_ACCESS_SHARED,
    VIR_MEM_ACCESS_PRIVATE,

    VIR_MEM_ACCESS_LAST,
} virMemAccess;

VIR_ENUM_DECL(virMemAccess)

typedef struct _virCPUFeatureDef virCPUFeatureDef;
typedef virCPUFeatureDef *virCPUFeatureDefPtr;
struct _virCPUFeatureDef {
    char *name;
    int policy;         /* enum virCPUFeaturePolicy */
};

typedef struct _virCellDef virCellDef;
typedef virCellDef *virCellDefPtr;
struct _virCellDef {
    virBitmapPtr cpumask; /* CPUs that are part of this node */
    char *cpustr;         /* CPUs stored in string form for dumpxml */
    unsigned long long mem;     /* Node memory in kB */
    virMemAccess memAccess;
};

typedef struct _virCPUDef virCPUDef;
typedef virCPUDef *virCPUDefPtr;
struct _virCPUDef {
    int type;           /* enum virCPUType */
    int mode;           /* enum virCPUMode */
    int match;          /* enum virCPUMatch */
    virArch arch;
    char *model;
    char *vendor_id;    /* vendor id returned by CPUID in the guest */
    int fallback;       /* enum virCPUFallback */
    char *vendor;
    unsigned int sockets;
    unsigned int cores;
    unsigned int threads;
    size_t nfeatures;
    size_t nfeatures_max;
    virCPUFeatureDefPtr features;
    size_t ncells;
    size_t ncells_max;
    virCellDefPtr cells;
    unsigned int cells_cpus;
};


void ATTRIBUTE_NONNULL(1)
virCPUDefFreeModel(virCPUDefPtr def);

void
virCPUDefFree(virCPUDefPtr def);

int ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2)
virCPUDefCopyModel(virCPUDefPtr dst,
                   const virCPUDef *src,
                   bool resetPolicy);

virCPUDefPtr
virCPUDefCopy(const virCPUDef *cpu);

virCPUDefPtr
virCPUDefParseXML(xmlNodePtr node,
                  xmlXPathContextPtr ctxt,
                  virCPUType mode);

bool
virCPUDefIsEqual(virCPUDefPtr src,
                 virCPUDefPtr dst);

char *
virCPUDefFormat(virCPUDefPtr def,
                bool updateCPU);

int
virCPUDefFormatBuf(virBufferPtr buf,
                   virCPUDefPtr def,
                   bool updateCPU);
int
virCPUDefFormatBufFull(virBufferPtr buf,
                       virCPUDefPtr def,
                       bool updateCPU);

int
virCPUDefAddFeature(virCPUDefPtr cpu,
                    const char *name,
                    int policy);

int
virCPUDefUpdateFeature(virCPUDefPtr cpu,
                       const char *name,
                       int policy);

#endif /* __VIR_CPU_CONF_H__ */
