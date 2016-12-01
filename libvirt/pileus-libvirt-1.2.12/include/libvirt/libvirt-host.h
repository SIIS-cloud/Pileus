/*
 * libvirt-host.h
 * Summary: APIs for management of hosts
 * Description: Provides APIs for the management of hosts
 * Author: Daniel Veillard <veillard@redhat.com>
 *
 * Copyright (C) 2006-2014 Red Hat, Inc.
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
 */

#ifndef __VIR_LIBVIRT_HOST_H__
# define __VIR_LIBVIRT_HOST_H__

# ifndef __VIR_LIBVIRT_H_INCLUDES__
#  error "Don't include this file directly, only use libvirt/libvirt.h"
# endif


/*
 * virFreeCallback:
 * @opaque: opaque user data provided at registration
 *
 * Type for a callback cleanup function to be paired with a callback.  This
 * function will be called as a final chance to clean up the @opaque
 * registered with the primary callback, at the time when the primary
 * callback is deregistered.
 *
 * It is forbidden to call any other libvirt APIs from an
 * implementation of this callback, since it can be invoked
 * from a context which is not re-entrant safe. Failure to
 * abide by this requirement may lead to application deadlocks
 * or crashes.
 */
typedef void (*virFreeCallback)(void *opaque);


/**
 * virConnect:
 *
 * a virConnect is a private structure representing a connection to
 * the Hypervisor.
 */
typedef struct _virConnect virConnect;

/**
 * virConnectPtr:
 *
 * a virConnectPtr is pointer to a virConnect private structure, this is the
 * type used to reference a connection to the Hypervisor in the API.
 */
typedef virConnect *virConnectPtr;

/**
 * virNodeSuspendTarget:
 *
 * Flags to indicate which system-wide sleep state the host must be
 * transitioned to.
 */
typedef enum {
    VIR_NODE_SUSPEND_TARGET_MEM     = 0,
    VIR_NODE_SUSPEND_TARGET_DISK    = 1,
    VIR_NODE_SUSPEND_TARGET_HYBRID  = 2,

# ifdef VIR_ENUM_SENTINELS
    VIR_NODE_SUSPEND_TARGET_LAST /* This constant is subject to change */
# endif
} virNodeSuspendTarget;

/**
 * virStream:
 *
 * a virStream is a private structure representing a data stream.
 */
typedef struct _virStream virStream;

/**
 * virStreamPtr:
 *
 * a virStreamPtr is pointer to a virStream private structure, this is the
 * type used to reference a data stream in the API.
 */
typedef virStream *virStreamPtr;

/**
 * VIR_SECURITY_LABEL_BUFLEN:
 *
 * Macro providing the maximum length of the virSecurityLabel label string.
 * Note that this value is based on that used by Labeled NFS.
 */
# define VIR_SECURITY_LABEL_BUFLEN (4096 + 1)

/**
 * virSecurityLabel:
 *
 * a virSecurityLabel is a structure filled by virDomainGetSecurityLabel(),
 * providing the security label and associated attributes for the specified
 * domain.
 */
typedef struct _virSecurityLabel virSecurityLabel;

struct _virSecurityLabel {
    char label[VIR_SECURITY_LABEL_BUFLEN];    /* security label string */
    int enforcing;                            /* 1 if security policy is being enforced for domain */
};

/**
 * virSecurityLabelPtr:
 *
 * a virSecurityLabelPtr is a pointer to a virSecurityLabel.
 */
typedef virSecurityLabel *virSecurityLabelPtr;

/**
 * VIR_SECURITY_MODEL_BUFLEN:
 *
 * Macro providing the maximum length of the virSecurityModel model string.
 */
# define VIR_SECURITY_MODEL_BUFLEN (256 + 1)

/**
 * VIR_SECURITY_DOI_BUFLEN:
 *
 * Macro providing the maximum length of the virSecurityModel doi string.
 */
# define VIR_SECURITY_DOI_BUFLEN (256 + 1)

/**
 * virSecurityModel:
 *
 * a virSecurityModel is a structure filled by virNodeGetSecurityModel(),
 * providing the per-hypervisor security model and DOI attributes for the
 * specified domain.
 */
typedef struct _virSecurityModel virSecurityModel;

struct _virSecurityModel {
    char model[VIR_SECURITY_MODEL_BUFLEN];      /* security model string */
    char doi[VIR_SECURITY_DOI_BUFLEN];          /* domain of interpretation */
};

/**
 * virSecurityModelPtr:
 *
 * a virSecurityModelPtr is a pointer to a virSecurityModel.
 */
typedef virSecurityModel *virSecurityModelPtr;

/* Common data types shared among interfaces with name/type/value lists.  */

/**
 * virTypedParameterType:
 *
 * Express the type of a virTypedParameter
 */
typedef enum {
    VIR_TYPED_PARAM_INT     = 1, /* integer case */
    VIR_TYPED_PARAM_UINT    = 2, /* unsigned integer case */
    VIR_TYPED_PARAM_LLONG   = 3, /* long long case */
    VIR_TYPED_PARAM_ULLONG  = 4, /* unsigned long long case */
    VIR_TYPED_PARAM_DOUBLE  = 5, /* double case */
    VIR_TYPED_PARAM_BOOLEAN = 6, /* boolean(character) case */
    VIR_TYPED_PARAM_STRING  = 7, /* string case */

# ifdef VIR_ENUM_SENTINELS
    VIR_TYPED_PARAM_LAST
# endif
} virTypedParameterType;

/**
 * virTypedParameterFlags:
 *
 * Flags related to libvirt APIs that use virTypedParameter.
 *
 * These enums should not conflict with those of virDomainModificationImpact.
 */
typedef enum {
    /* 1 << 0 is reserved for virDomainModificationImpact */
    /* 1 << 1 is reserved for virDomainModificationImpact */

    /* Older servers lacked the ability to handle string typed
     * parameters.  Attempts to set a string parameter with an older
     * server will fail at the client, but attempts to retrieve
     * parameters must not return strings from a new server to an
     * older client, so this flag exists to identify newer clients to
     * newer servers.  This flag is automatically set when needed, so
     * the user does not have to worry about it; however, manually
     * setting the flag can be used to reject servers that cannot
     * return typed strings, even if no strings would be returned.
     */
    VIR_TYPED_PARAM_STRING_OKAY = 1 << 2,

} virTypedParameterFlags;

/**
 * VIR_TYPED_PARAM_FIELD_LENGTH:
 *
 * Macro providing the field length of virTypedParameter name
 */
# define VIR_TYPED_PARAM_FIELD_LENGTH 80

/**
 * virTypedParameter:
 *
 * A named parameter, including a type and value.
 *
 * The types virSchedParameter, virBlkioParameter, and
 * virMemoryParameter are aliases of this type, for use when
 * targetting libvirt earlier than 0.9.2.
 */
typedef struct _virTypedParameter virTypedParameter;

struct _virTypedParameter {
    char field[VIR_TYPED_PARAM_FIELD_LENGTH];  /* parameter name */
    int type;   /* parameter type, virTypedParameterType */
    union {
        int i;                      /* type is INT */
        unsigned int ui;            /* type is UINT */
        long long int l;            /* type is LLONG */
        unsigned long long int ul;  /* type is ULLONG */
        double d;                   /* type is DOUBLE */
        char b;                     /* type is BOOLEAN */
        char *s;                    /* type is STRING, may not be NULL */
    } value; /* parameter value */
};

/**
 * virTypedParameterPtr:
 *
 * a pointer to a virTypedParameter structure.
 */
typedef virTypedParameter *virTypedParameterPtr;


virTypedParameterPtr
virTypedParamsGet       (virTypedParameterPtr params,
                         int nparams,
                         const char *name);
int
virTypedParamsGetInt    (virTypedParameterPtr params,
                         int nparams,
                         const char *name,
                         int *value);
int
virTypedParamsGetUInt   (virTypedParameterPtr params,
                         int nparams,
                         const char *name,
                         unsigned int *value);
int
virTypedParamsGetLLong  (virTypedParameterPtr params,
                         int nparams,
                         const char *name,
                         long long *value);
int
virTypedParamsGetULLong (virTypedParameterPtr params,
                         int nparams,
                         const char *name,
                         unsigned long long *value);
int
virTypedParamsGetDouble (virTypedParameterPtr params,
                         int nparams,
                         const char *name,
                         double *value);
int
virTypedParamsGetBoolean(virTypedParameterPtr params,
                         int nparams,
                         const char *name,
                         int *value);
int
virTypedParamsGetString (virTypedParameterPtr params,
                         int nparams,
                         const char *name,
                         const char **value);
int
virTypedParamsAddInt    (virTypedParameterPtr *params,
                         int *nparams,
                         int *maxparams,
                         const char *name,
                         int value);
int
virTypedParamsAddUInt   (virTypedParameterPtr *params,
                         int *nparams,
                         int *maxparams,
                         const char *name,
                         unsigned int value);
int
virTypedParamsAddLLong  (virTypedParameterPtr *params,
                         int *nparams,
                         int *maxparams,
                         const char *name,
                         long long value);
int
virTypedParamsAddULLong (virTypedParameterPtr *params,
                         int *nparams,
                         int *maxparams,
                         const char *name,
                         unsigned long long value);
int
virTypedParamsAddDouble (virTypedParameterPtr *params,
                         int *nparams,
                         int *maxparams,
                         const char *name,
                         double value);
int
virTypedParamsAddBoolean(virTypedParameterPtr *params,
                         int *nparams,
                         int *maxparams,
                         const char *name,
                         int value);
int
virTypedParamsAddString (virTypedParameterPtr *params,
                         int *nparams,
                         int *maxparams,
                         const char *name,
                         const char *value);
int
virTypedParamsAddFromString(virTypedParameterPtr *params,
                         int *nparams,
                         int *maxparams,
                         const char *name,
                         int type,
                         const char *value);
void
virTypedParamsClear     (virTypedParameterPtr params,
                         int nparams);
void
virTypedParamsFree      (virTypedParameterPtr params,
                         int nparams);

/* data types related to virNodePtr */

/**
 * virNodeInfoPtr:
 *
 * a virNodeInfo is a structure filled by virNodeGetInfo() and providing
 * the information for the Node.
 */

typedef struct _virNodeInfo virNodeInfo;

struct _virNodeInfo {
    char model[32];       /* string indicating the CPU model */
    unsigned long memory; /* memory size in kilobytes */
    unsigned int cpus;    /* the number of active CPUs */
    unsigned int mhz;     /* expected CPU frequency */
    unsigned int nodes;   /* the number of NUMA cell, 1 for unusual NUMA
                             topologies or uniform memory access; check
                             capabilities XML for the actual NUMA topology */
    unsigned int sockets; /* number of CPU sockets per node if nodes > 1,
                             1 in case of unusual NUMA topology */
    unsigned int cores;   /* number of cores per socket, total number of
                             processors in case of unusual NUMA topology*/
    unsigned int threads; /* number of threads per core, 1 in case of
                             unusual numa topology */
};

/**
 * VIR_NODE_CPU_STATS_FIELD_LENGTH:
 *
 * Macro providing the field length of virNodeCPUStats
 */
# define VIR_NODE_CPU_STATS_FIELD_LENGTH 80

/**
 * VIR_NODE_CPU_STATS_ALL_CPUS:
 *
 * Value for specifying request for the total CPU time/utilization
 */
typedef enum {
    VIR_NODE_CPU_STATS_ALL_CPUS = -1,
} virNodeGetCPUStatsAllCPUs;

/**
 * VIR_NODE_CPU_STATS_KERNEL:
 *
 * Macro for the cumulative CPU time which was spent by the kernel,
 * since the node booting up (in nanoseconds).
 */
# define VIR_NODE_CPU_STATS_KERNEL "kernel"

/**
 * VIR_NODE_CPU_STATS_USER:
 *
 * The cumulative CPU time which was spent by user processes,
 * since the node booting up (in nanoseconds).
 */
# define VIR_NODE_CPU_STATS_USER "user"

/**
 * VIR_NODE_CPU_STATS_IDLE:
 *
 * The cumulative idle CPU time,
 * since the node booting up (in nanoseconds).
 */
# define VIR_NODE_CPU_STATS_IDLE "idle"

/**
 * VIR_NODE_CPU_STATS_IOWAIT:
 *
 * The cumulative I/O wait CPU time,
 * since the node booting up (in nanoseconds).
 */
# define VIR_NODE_CPU_STATS_IOWAIT "iowait"

/**
 * VIR_NODE_CPU_STATS_INTR:
 *
 * The cumulative interrupt CPU time,
 * since the node booting up (in nanoseconds).
 */
# define VIR_NODE_CPU_STATS_INTR "intr"

/**
 * VIR_NODE_CPU_STATS_UTILIZATION:
 *
 * The CPU utilization of a node.
 * The usage value is in percent and 100% represents all CPUs of
 * the node.
 */
# define VIR_NODE_CPU_STATS_UTILIZATION "utilization"

/**
 * virNodeCPUStats:
 *
 * a virNodeCPUStats is a structure filled by virNodeGetCPUStats()
 * providing information about the CPU stats of the node.
 */
typedef struct _virNodeCPUStats virNodeCPUStats;

struct _virNodeCPUStats {
    char field[VIR_NODE_CPU_STATS_FIELD_LENGTH];
    unsigned long long value;
};

/**
 * VIR_NODE_MEMORY_STATS_FIELD_LENGTH:
 *
 * Macro providing the field length of virNodeMemoryStats
 */
# define VIR_NODE_MEMORY_STATS_FIELD_LENGTH 80

/**
 * VIR_NODE_MEMORY_STATS_ALL_CELLS:
 *
 * Value for specifying request for the total memory of all cells.
 */
typedef enum {
    VIR_NODE_MEMORY_STATS_ALL_CELLS = -1,
} virNodeGetMemoryStatsAllCells;

/**
 * VIR_NODE_MEMORY_STATS_TOTAL:
 *
 * Macro for the total memory of specified cell:
 * it represents the maximum memory.
 */
# define VIR_NODE_MEMORY_STATS_TOTAL "total"

/**
 * VIR_NODE_MEMORY_STATS_FREE:
 *
 * Macro for the free memory of specified cell:
 * On Linux, it includes buffer and cached memory, in case of
 * VIR_NODE_MEMORY_STATS_ALL_CELLS.
 */
# define VIR_NODE_MEMORY_STATS_FREE "free"

/**
 * VIR_NODE_MEMORY_STATS_BUFFERS:
 *
 * Macro for the buffer memory: On Linux, it is only returned in case of
 * VIR_NODE_MEMORY_STATS_ALL_CELLS.
 */
# define VIR_NODE_MEMORY_STATS_BUFFERS "buffers"

/**
 * VIR_NODE_MEMORY_STATS_CACHED:
 *
 * Macro for the cached memory: On Linux, it is only returned in case of
 * VIR_NODE_MEMORY_STATS_ALL_CELLS.
 */
# define VIR_NODE_MEMORY_STATS_CACHED "cached"

/**
 * virNodeMemoryStats:
 *
 * a virNodeMemoryStats is a structure filled by virNodeGetMemoryStats()
 * providing information about the memory of the node.
 */
typedef struct _virNodeMemoryStats virNodeMemoryStats;

struct _virNodeMemoryStats {
    char field[VIR_NODE_MEMORY_STATS_FIELD_LENGTH];
    unsigned long long value;
};

/*
 * VIR_NODE_MEMORY_SHARED_PAGES_TO_SCAN:
 *
 * Macro for typed parameter that represents how many present pages
 * to scan before the shared memory service goes to sleep.
 */
# define VIR_NODE_MEMORY_SHARED_PAGES_TO_SCAN      "shm_pages_to_scan"

/*
 * VIR_NODE_MEMORY_SHARED_SLEEP_MILLISECS:
 *
 * Macro for typed parameter that represents how many milliseconds
 * the shared memory service should sleep before next scan.
 */
# define VIR_NODE_MEMORY_SHARED_SLEEP_MILLISECS    "shm_sleep_millisecs"

/*
 * VIR_NODE_MEMORY_SHARED_PAGES_SHARED:
 *
 * Macro for typed parameter that represents how many the shared
 * memory pages are being used.
 */
# define VIR_NODE_MEMORY_SHARED_PAGES_SHARED       "shm_pages_shared"

/*
 * VIR_NODE_MEMORY_SHARED_PAGES_SHARING:
 *
 * Macro for typed parameter that represents how many sites are
 * sharing the pages i.e. how much saved.
 */
# define VIR_NODE_MEMORY_SHARED_PAGES_SHARING      "shm_pages_sharing"

/*
 * VIR_NODE_MEMORY_SHARED_PAGES_UNSHARED:
 *
 * Macro for typed parameter that represents how many pages unique
 * but repeatedly checked for merging.
 */
# define VIR_NODE_MEMORY_SHARED_PAGES_UNSHARED     "shm_pages_unshared"

/*
 * VIR_NODE_MEMORY_SHARED_PAGES_VOLATILE:
 *
 * Macro for typed parameter that represents how many pages changing
 * too fast to be placed in a tree.
 */
# define VIR_NODE_MEMORY_SHARED_PAGES_VOLATILE     "shm_pages_volatile"

/*
 * VIR_NODE_MEMORY_SHARED_FULL_SCANS:
 *
 * Macro for typed parameter that represents how many times all
 * mergeable areas have been scanned.
 */
# define VIR_NODE_MEMORY_SHARED_FULL_SCANS         "shm_full_scans"

/*
 * VIR_NODE_MEMORY_SHARED_MERGE_ACROSS_NODES:
 *
 * Macro for typed parameter that represents whether pages from
 * different NUMA nodes can be merged. The parameter has type int,
 * when its value is 0, only pages which physically reside in the
 * memory area of same NUMA node are merged; When its value is 1,
 * pages from all nodes can be merged. Other values are reserved
 * for future use.
 */
# define VIR_NODE_MEMORY_SHARED_MERGE_ACROSS_NODES "shm_merge_across_nodes"


int virNodeGetMemoryParameters(virConnectPtr conn,
                               virTypedParameterPtr params,
                               int *nparams,
                               unsigned int flags);

int virNodeSetMemoryParameters(virConnectPtr conn,
                               virTypedParameterPtr params,
                               int nparams,
                               unsigned int flags);

/*
 *  node CPU map
 */
int virNodeGetCPUMap(virConnectPtr conn,
                     unsigned char **cpumap,
                     unsigned int *online,
                     unsigned int flags);


/**
 * VIR_NODEINFO_MAXCPUS:
 * @nodeinfo: virNodeInfo instance
 *
 * This macro is to calculate the total number of CPUs supported
 * but not necessary active in the host.
 */


# define VIR_NODEINFO_MAXCPUS(nodeinfo) ((nodeinfo).nodes*(nodeinfo).sockets*(nodeinfo).cores*(nodeinfo).threads)

/**
 * virNodeInfoPtr:
 *
 * a virNodeInfoPtr is a pointer to a virNodeInfo structure.
 */

typedef virNodeInfo *virNodeInfoPtr;

/**
 * virNodeCPUStatsPtr:
 *
 * a virNodeCPUStatsPtr is a pointer to a virNodeCPUStats structure.
 */

typedef virNodeCPUStats *virNodeCPUStatsPtr;

/**
 * virNodeMemoryStatsPtr:
 *
 * a virNodeMemoryStatsPtr is a pointer to a virNodeMemoryStats structure.
 */

typedef virNodeMemoryStats *virNodeMemoryStatsPtr;

/**
 * virConnectFlags
 *
 * Flags when opening a connection to a hypervisor
 */
typedef enum {
    VIR_CONNECT_RO         = (1 << 0),  /* A readonly connection */
    VIR_CONNECT_NO_ALIASES = (1 << 1),  /* Don't try to resolve URI aliases */
} virConnectFlags;


typedef enum {
    VIR_CRED_USERNAME = 1,     /* Identity to act as */
    VIR_CRED_AUTHNAME = 2,     /* Identify to authorize as */
    VIR_CRED_LANGUAGE = 3,     /* RFC 1766 languages, comma separated */
    VIR_CRED_CNONCE = 4,       /* client supplies a nonce */
    VIR_CRED_PASSPHRASE = 5,   /* Passphrase secret */
    VIR_CRED_ECHOPROMPT = 6,   /* Challenge response */
    VIR_CRED_NOECHOPROMPT = 7, /* Challenge response */
    VIR_CRED_REALM = 8,        /* Authentication realm */
    VIR_CRED_EXTERNAL = 9,     /* Externally managed credential */

# ifdef VIR_ENUM_SENTINELS
    VIR_CRED_LAST              /* More may be added - expect the unexpected */
# endif
} virConnectCredentialType;

struct _virConnectCredential {
    int type; /* One of virConnectCredentialType constants */
    const char *prompt; /* Prompt to show to user */
    const char *challenge; /* Additional challenge to show */
    const char *defresult; /* Optional default result */
    char *result; /* Result to be filled with user response (or defresult) */
    unsigned int resultlen; /* Length of the result */
};

typedef struct _virConnectCredential virConnectCredential;
typedef virConnectCredential *virConnectCredentialPtr;


/**
 * virConnectAuthCallbackPtr:
 * @cred: list of virConnectCredential object to fetch from user
 * @ncred: size of cred list
 * @cbdata: opaque data passed to virConnectOpenAuth
 *
 * When authentication requires one or more interactions, this callback
 * is invoked. For each interaction supplied, data must be gathered
 * from the user and filled in to the 'result' and 'resultlen' fields.
 * If an interaction cannot be filled, fill in NULL and 0.
 *
 * Returns 0 if all interactions were filled, or -1 upon error
 */
typedef int (*virConnectAuthCallbackPtr)(virConnectCredentialPtr cred,
                                         unsigned int ncred,
                                         void *cbdata);

struct _virConnectAuth {
    int *credtype; /* List of supported virConnectCredentialType values */
    unsigned int ncredtype;

    virConnectAuthCallbackPtr cb; /* Callback used to collect credentials */
    void *cbdata;
};


typedef struct _virConnectAuth virConnectAuth;
typedef virConnectAuth *virConnectAuthPtr;

VIR_EXPORT_VAR virConnectAuthPtr virConnectAuthPtrDefault;

/**
 * VIR_UUID_BUFLEN:
 *
 * This macro provides the length of the buffer required
 * for virDomainGetUUID()
 */

# define VIR_UUID_BUFLEN (16)

/**
 * VIR_UUID_STRING_BUFLEN:
 *
 * This macro provides the length of the buffer required
 * for virDomainGetUUIDString()
 */

# define VIR_UUID_STRING_BUFLEN (36+1)


int                     virGetVersion           (unsigned long *libVer,
                                                 const char *type,
                                                 unsigned long *typeVer);

/*
 * Connection and disconnections to the Hypervisor
 */
int                     virInitialize           (void);

virConnectPtr           virConnectOpen          (const char *name);
// SYQ
virConnectPtr		virConnectOpenLabel	(const char *name, const char *label, int len);
virConnectPtr           virConnectOpenReadOnly  (const char *name);
virConnectPtr           virConnectOpenAuth      (const char *name,
                                                 virConnectAuthPtr auth,
                                                 unsigned int flags);
int                     virConnectRef           (virConnectPtr conn);
int                     virConnectClose         (virConnectPtr conn);
const char *            virConnectGetType       (virConnectPtr conn);
int                     virConnectGetVersion    (virConnectPtr conn,
                                                 unsigned long *hvVer);
int                     virConnectGetLibVersion (virConnectPtr conn,
                                                 unsigned long *libVer);
char *                  virConnectGetHostname   (virConnectPtr conn);
char *                  virConnectGetURI        (virConnectPtr conn);
char *                  virConnectGetSysinfo    (virConnectPtr conn,
                                                 unsigned int flags);

int virConnectSetKeepAlive(virConnectPtr conn,
                           int interval,
                           unsigned int count);

typedef enum {
    VIR_CONNECT_CLOSE_REASON_ERROR     = 0, /* Misc I/O error */
    VIR_CONNECT_CLOSE_REASON_EOF       = 1, /* End-of-file from server */
    VIR_CONNECT_CLOSE_REASON_KEEPALIVE = 2, /* Keepalive timer triggered */
    VIR_CONNECT_CLOSE_REASON_CLIENT    = 3, /* Client requested it */

# ifdef VIR_ENUM_SENTINELS
    VIR_CONNECT_CLOSE_REASON_LAST
# endif
} virConnectCloseReason;

/**
 * virConnectCloseFunc:
 * @conn: virConnect connection
 * @reason: reason why the connection was closed (one of virConnectCloseReason)
 * @opaque: opaque user data
 *
 * A callback function to be registered, and called when the connection
 * is closed.
 */
typedef void (*virConnectCloseFunc)(virConnectPtr conn,
                                    int reason,
                                    void *opaque);

int virConnectRegisterCloseCallback(virConnectPtr conn,
                                    virConnectCloseFunc cb,
                                    void *opaque,
                                    virFreeCallback freecb);
int virConnectUnregisterCloseCallback(virConnectPtr conn,
                                      virConnectCloseFunc cb);

/*
 * Capabilities of the connection / driver.
 */

int                     virConnectGetMaxVcpus   (virConnectPtr conn,
                                                 const char *type);
int                     virNodeGetInfo          (virConnectPtr conn,
                                                 virNodeInfoPtr info);
char *                  virConnectGetCapabilities (virConnectPtr conn);

int                     virNodeGetCPUStats (virConnectPtr conn,
                                            int cpuNum,
                                            virNodeCPUStatsPtr params,
                                            int *nparams,
                                            unsigned int flags);

int                     virNodeGetMemoryStats (virConnectPtr conn,
                                               int cellNum,
                                               virNodeMemoryStatsPtr params,
                                               int *nparams,
                                               unsigned int flags);

unsigned long long      virNodeGetFreeMemory    (virConnectPtr conn);

int                     virNodeGetSecurityModel (virConnectPtr conn,
                                                 virSecurityModelPtr secmodel);

int                     virNodeSuspendForDuration (virConnectPtr conn,
                                                   unsigned int target,
                                                   unsigned long long duration,
                                                   unsigned int flags);

/*
 * NUMA support
 */

int                      virNodeGetCellsFreeMemory(virConnectPtr conn,
                                                   unsigned long long *freeMems,
                                                   int startCell,
                                                   int maxCells);


int virConnectIsEncrypted(virConnectPtr conn);
int virConnectIsSecure(virConnectPtr conn);
int virConnectIsAlive(virConnectPtr conn);

/*
 * CPU specification API
 */

typedef enum {
    VIR_CPU_COMPARE_ERROR           = -1,
    VIR_CPU_COMPARE_INCOMPATIBLE    = 0,
    VIR_CPU_COMPARE_IDENTICAL       = 1,
    VIR_CPU_COMPARE_SUPERSET        = 2,

# ifdef VIR_ENUM_SENTINELS
    VIR_CPU_COMPARE_LAST
# endif
} virCPUCompareResult;

typedef enum {
    VIR_CONNECT_COMPARE_CPU_FAIL_INCOMPATIBLE = (1 << 0), /* treat incompatible
                                                             CPUs as failure */
} virConnectCompareCPUFlags;

int virConnectCompareCPU(virConnectPtr conn,
                         const char *xmlDesc,
                         unsigned int flags);

int virConnectGetCPUModelNames(virConnectPtr conn,
                               const char *arch,
                               char ***models,
                               unsigned int flags);

/**
 * virConnectBaselineCPUFlags
 *
 * Flags when getting XML description of a computed CPU
 */
typedef enum {
    VIR_CONNECT_BASELINE_CPU_EXPAND_FEATURES  = (1 << 0),  /* show all features */
} virConnectBaselineCPUFlags;

char *virConnectBaselineCPU(virConnectPtr conn,
                            const char **xmlCPUs,
                            unsigned int ncpus,
                            unsigned int flags);


int virNodeGetFreePages(virConnectPtr conn,
                        unsigned int npages,
                        unsigned int *pages,
                        int startcell,
                        unsigned int cellcount,
                        unsigned long long *counts,
                        unsigned int flags);

typedef enum {
    VIR_NODE_ALLOC_PAGES_ADD = 0, /* Add @pageCounts to the pages pool. This
                                     can be used only to size up the pool. */
    VIR_NODE_ALLOC_PAGES_SET = (1 << 0), /* Don't add @pageCounts, instead set
                                            passed number of pages. This can be
                                            used to free allocated pages. */
} virNodeAllocPagesFlags;

int virNodeAllocPages(virConnectPtr conn,
                      unsigned int npages,
                      unsigned int *pageSizes,
                      unsigned long long *pageCounts,
                      int startCell,
                      unsigned int cellCount,
                      unsigned int flags);


#endif /* __VIR_LIBVIRT_HOST_H__ */
