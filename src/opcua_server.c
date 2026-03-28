/*
 * opcua_server.c – OPC-UA server implementation for OpenC-DA2UA
 */
#include "opcua_server.h"
#include "logger.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#if defined(HAVE_OPEN62541)
#  if defined(_WIN32)
#    include <windows.h>
#  else
#    include <pthread.h>
#  endif
#endif

#ifdef HAVE_OPEN62541
#  include <open62541/server.h>
#  include <open62541/server_config_default.h>
#  include <open62541/plugin/log_stdout.h>
#endif

#ifdef HAVE_OPEN62541
typedef struct {
    int index;
    TagType type;
    char name[MAX_STR_LEN];
    struct OpcUAServer *owner;
} NodeMeta;

#define MAX_FOLDER_NODES 2048

typedef struct {
    char path[MAX_STR_LEN];
    UA_NodeId node_id;
} FolderMeta;
#endif

struct OpcUAServer {
#ifdef HAVE_OPEN62541
    UA_Server *ua_server;
    UA_NodeId ns_node_ids[MAX_NODES];
    char node_names[MAX_NODES][MAX_STR_LEN];
    NodeMeta node_meta[MAX_NODES];
    FolderMeta folder_meta[MAX_FOLDER_NODES];
    int node_count;
    int folder_count;
    UA_UInt16 ns_index;
    OpcUaWriteHandler write_handler;
    void *write_ctx;
    int suppress_writeback;
#else
    int dummy;
#endif
    int running;
#ifdef HAVE_OPEN62541
#  ifdef _WIN32
    HANDLE thread;
#  else
    pthread_t thread;
#  endif
#endif
};

#ifdef HAVE_OPEN62541
static int extract_port_from_endpoint(const char *endpoint)
{
    if (!endpoint || !endpoint[0]) return 4840;
    const char *colon = strrchr(endpoint, ':');
    if (!colon || !colon[1]) return 4840;
    for (const char *p = colon + 1; *p; ++p) {
        if (!isdigit((unsigned char)*p)) return 4840;
    }
    int port = atoi(colon + 1);
    if (port <= 0 || port > 65535) return 4840;
    return port;
}

static int find_folder_index(const OpcUAServer *srv, const char *path)
{
    for (int i = 0; i < srv->folder_count; ++i) {
        if (strncmp(srv->folder_meta[i].path, path, MAX_STR_LEN) == 0) return i;
    }
    return -1;
}

static int ensure_folder_path(OpcUAServer *srv, const char *full_name, UA_NodeId *out_parent)
{
    char pathbuf[MAX_STR_LEN];
    char accum[MAX_STR_LEN];
    char *ctx = NULL;
    char *seg = NULL;
    UA_NodeId current = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);

    if (!srv || !full_name || !out_parent) return -1;
    snprintf(pathbuf, sizeof(pathbuf), "%s", full_name);
    accum[0] = '\0';

    seg = strtok_s(pathbuf, ".", &ctx);
    while (seg) {
        char *next = strtok_s(NULL, ".", &ctx);
        if (!next) break; /* Last segment is variable name */

        if (accum[0]) {
            strncat(accum, ".", sizeof(accum) - strlen(accum) - 1);
        }
        strncat(accum, seg, sizeof(accum) - strlen(accum) - 1);

        int idx = find_folder_index(srv, accum);
        if (idx >= 0) {
            current = srv->folder_meta[idx].node_id;
            seg = next;
            continue;
        }

        if (srv->folder_count >= MAX_FOLDER_NODES) return -1;

        FolderMeta *fm = &srv->folder_meta[srv->folder_count];
        memset(fm, 0, sizeof(*fm));
        snprintf(fm->path, sizeof(fm->path), "%s", accum);

        UA_ObjectAttributes oattr = UA_ObjectAttributes_default;
        oattr.displayName = UA_LOCALIZEDTEXT("en-US", seg);
        oattr.description = UA_LOCALIZEDTEXT("en-US", seg);

        UA_NodeId folder_id = UA_NODEID_STRING_ALLOC(srv->ns_index, fm->path);
        UA_QualifiedName fq = UA_QUALIFIEDNAME(srv->ns_index, seg);
        UA_StatusCode sc = UA_Server_addObjectNode(
            srv->ua_server,
            folder_id,
            current,
            UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
            fq,
            UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE),
            oattr,
            NULL,
            &fm->node_id);
        UA_NodeId_clear(&folder_id);
        if (sc != UA_STATUSCODE_GOOD) return -1;

        current = fm->node_id;
        srv->folder_count++;
        seg = next;
    }

    *out_parent = current;
    return 0;
}

static int ua_data_to_tag_value(const UA_DataValue *data, TagType type, TagValue *out)
{
    if (!data || !data->hasValue || !out) return -1;
    if (!UA_Variant_isScalar(&data->value) || !data->value.data) return -1;

    memset(out, 0, sizeof(*out));
    out->type = type;

    switch (type) {
        case TAG_TYPE_BOOL:
            if (data->value.type != &UA_TYPES[UA_TYPES_BOOLEAN]) return -1;
            out->value.bool_val = (*(UA_Boolean *)data->value.data) ? 1 : 0;
            return 0;
        case TAG_TYPE_INT:
            if (data->value.type != &UA_TYPES[UA_TYPES_INT16]) return -1;
            out->value.int_val = *(UA_Int16 *)data->value.data;
            return 0;
        case TAG_TYPE_DINT:
            if (data->value.type != &UA_TYPES[UA_TYPES_INT32]) return -1;
            out->value.dint_val = *(UA_Int32 *)data->value.data;
            return 0;
        case TAG_TYPE_REAL:
            if (data->value.type != &UA_TYPES[UA_TYPES_FLOAT]) return -1;
            out->value.real_val = *(UA_Float *)data->value.data;
            return 0;
        case TAG_TYPE_STRING: {
            if (data->value.type != &UA_TYPES[UA_TYPES_STRING]) return -1;
            UA_String *s = (UA_String *)data->value.data;
            size_t n = (size_t)s->length;
            if (n >= sizeof(out->value.str_val)) n = sizeof(out->value.str_val) - 1;
            if (n > 0) memcpy(out->value.str_val, s->data, n);
            out->value.str_val[n] = '\0';
            return 0;
        }
        default:
            return -1;
    }
}

static void on_node_written(UA_Server *server,
                            const UA_NodeId *sessionId, void *sessionContext,
                            const UA_NodeId *nodeId, void *nodeContext,
                            const UA_NumericRange *range,
                            const UA_DataValue *data)
{
    (void)server;
    (void)sessionId;
    (void)sessionContext;
    (void)nodeId;
    (void)range;

    if (!nodeContext) return;

    NodeMeta *meta = (NodeMeta *)nodeContext;
    OpcUAServer *srv = meta->owner;
    if (!srv || !srv->write_handler) return;
    if (srv->suppress_writeback) return;

    TagValue tv;
    memset(&tv, 0, sizeof(tv));
    strncpy(tv.name, meta->name, sizeof(tv.name) - 1);
    tv.name[sizeof(tv.name) - 1] = '\0';

    if (ua_data_to_tag_value(data, meta->type, &tv) != 0) {
        LOG_WARN_MSG("opcua: write callback conversion failed for '%s'", meta->name);
        return;
    }

    if (srv->write_handler(meta->index, &tv, srv->write_ctx) != 0) {
        LOG_WARN_MSG("opcua: write-through to OPCDA failed for '%s'", meta->name);
    }
}

#  ifdef _WIN32
static DWORD WINAPI server_thread(LPVOID arg)
{
    OpcUAServer *srv = (OpcUAServer *)arg;
    UA_StatusCode sc = UA_Server_run(srv->ua_server, (volatile UA_Boolean *)&srv->running);
    if (sc != UA_STATUSCODE_GOOD)
        LOG_ERROR_MSG("opcua: server exited with status 0x%08X", sc);
    return 0;
}
#  else
static void *server_thread(void *arg)
{
    OpcUAServer *srv = (OpcUAServer *)arg;
    UA_StatusCode sc = UA_Server_run(srv->ua_server, (volatile UA_Boolean *)&srv->running);
    if (sc != UA_STATUSCODE_GOOD)
        LOG_ERROR_MSG("opcua: server exited with status 0x%08X", sc);
    return NULL;
}
#  endif
#endif

OpcUAServer *opcua_server_create(const OpcUAConfig *cfg,
                                 const SecurityConfig *sec)
{
    if (!cfg || !sec) return NULL;

    OpcUAServer *srv = (OpcUAServer *)calloc(1, sizeof(*srv));
    if (!srv) return NULL;

#ifdef HAVE_OPEN62541
    srv->ua_server = UA_Server_new();
    if (!srv->ua_server) { free(srv); return NULL; }

    UA_ServerConfig *ucfg = UA_Server_getConfig(srv->ua_server);
    UA_ServerConfig_setDefault(ucfg);

    int endpoint_port = extract_port_from_endpoint(cfg->endpoint);
    UA_String endpoint = UA_String_fromChars(cfg->endpoint);
    UA_ServerConfig_setMinimalCustomBuffer(ucfg, (UA_UInt16)endpoint_port,
                                           &endpoint, 0, 0);
    UA_String_clear(&endpoint);

    srv->ns_index = UA_Server_addNamespace(srv->ua_server, cfg->uri);

    if (sec->mode == SECURITY_NONE) {
        LOG_INFO_MSG("opcua: security = None");
    } else {
        LOG_INFO_MSG("opcua: security mode %d configured", sec->mode);
    }

    LOG_INFO_MSG("opcua: server created, endpoint=%s", cfg->endpoint);
#else
    (void)sec;
    LOG_WARN_MSG("opcua: open62541 not compiled in – server is a stub");
#endif

    return srv;
}

int opcua_server_add_nodes(OpcUAServer *srv, const NodeConfig *ncfg,
                           OpcUaWriteHandler write_handler, void *write_ctx)
{
    if (!srv || !ncfg) return -1;

#ifdef HAVE_OPEN62541
    srv->node_count = 0;
    srv->folder_count = 0;
    srv->write_handler = write_handler;
    srv->write_ctx = write_ctx;

    for (int i = 0; i < ncfg->count && i < MAX_NODES; i++) {
        const NodeDef *nd = &ncfg->nodes[i];
        const char *tree_name = nd->source[0] ? nd->source : nd->name;

        UA_VariableAttributes attr = UA_VariableAttributes_default;
        attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;

        switch (nd->type) {
            case TAG_TYPE_BOOL: {
                UA_Boolean v = UA_FALSE;
                UA_Variant_setScalarCopy(&attr.value, &v, &UA_TYPES[UA_TYPES_BOOLEAN]);
                break;
            }
            case TAG_TYPE_INT: {
                UA_Int16 v = 0;
                UA_Variant_setScalarCopy(&attr.value, &v, &UA_TYPES[UA_TYPES_INT16]);
                break;
            }
            case TAG_TYPE_DINT: {
                UA_Int32 v = 0;
                UA_Variant_setScalarCopy(&attr.value, &v, &UA_TYPES[UA_TYPES_INT32]);
                break;
            }
            case TAG_TYPE_REAL: {
                UA_Float v = 0.0f;
                UA_Variant_setScalarCopy(&attr.value, &v, &UA_TYPES[UA_TYPES_FLOAT]);
                break;
            }
            case TAG_TYPE_STRING: {
                UA_String v = UA_STRING_NULL;
                UA_Variant_setScalarCopy(&attr.value, &v, &UA_TYPES[UA_TYPES_STRING]);
                break;
            }
        }

        const char *leaf_name = tree_name;
        const char *last_dot = strrchr(tree_name, '.');
        if (last_dot && last_dot[1]) leaf_name = last_dot + 1;

        attr.displayName = UA_LOCALIZEDTEXT("en-US", (char *)leaf_name);
        attr.description = UA_LOCALIZEDTEXT("en-US", (char *)tree_name);

        NodeMeta *meta = &srv->node_meta[srv->node_count];
        memset(meta, 0, sizeof(*meta));
        meta->index = i;
        meta->type = nd->type;
        meta->owner = srv;
        strncpy(meta->name, nd->name, sizeof(meta->name) - 1);
        meta->name[sizeof(meta->name) - 1] = '\0';

        UA_NodeId node_id = UA_NODEID_STRING_ALLOC(srv->ns_index, nd->name);
        UA_QualifiedName qname = UA_QUALIFIEDNAME(srv->ns_index, (char *)leaf_name);
        UA_NodeId parent_id = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
        UA_NodeId ref_type  = UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT);

        if (ensure_folder_path(srv, tree_name, &parent_id) != 0) {
            LOG_WARN_MSG("opcua: ensure folder path failed for '%s'", tree_name);
            UA_Variant_clear(&attr.value);
            UA_NodeId_clear(&node_id);
            continue;
        }

        UA_StatusCode sc = UA_Server_addVariableNode(
            srv->ua_server,
            node_id, parent_id, ref_type,
            qname,
            UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
            attr, meta, &srv->ns_node_ids[srv->node_count]);

        UA_Variant_clear(&attr.value);
        UA_NodeId_clear(&node_id);

        if (sc != UA_STATUSCODE_GOOD) {
            LOG_WARN_MSG("opcua: add node '%s' failed: 0x%08X", nd->name, sc);
            continue;
        }

        UA_ValueCallback cb;
        memset(&cb, 0, sizeof(cb));
        cb.onWrite = on_node_written;
        UA_Server_setVariableNode_valueCallback(srv->ua_server,
                                                srv->ns_node_ids[srv->node_count],
                                                cb);

        strncpy(srv->node_names[srv->node_count], nd->name, MAX_STR_LEN - 1);
        srv->node_names[srv->node_count][MAX_STR_LEN - 1] = '\0';
        srv->node_count++;
    }

    LOG_INFO_MSG("opcua: %d nodes registered", srv->node_count);
    return 0;
#else
    (void)ncfg;
    (void)write_handler;
    (void)write_ctx;
    return -1;
#endif
}

int opcua_server_start(OpcUAServer *srv)
{
    if (!srv) return -1;
    srv->running = 1;

#ifdef HAVE_OPEN62541
#  ifdef _WIN32
    srv->thread = CreateThread(NULL, 0, server_thread, srv, 0, NULL);
    if (!srv->thread) {
#  else
    if (pthread_create(&srv->thread, NULL, server_thread, srv) != 0) {
#  endif
        LOG_ERROR_MSG("opcua: failed to start server thread");
        srv->running = 0;
        return -1;
    }
    LOG_INFO_MSG("opcua: server started");
    return 0;
#else
    LOG_WARN_MSG("opcua: open62541 not compiled in – running in stub mode");
    return 0;
#endif
}

void opcua_server_update(OpcUAServer *srv, const TagValueSet *tvs)
{
    if (!srv || !tvs) return;

#ifdef HAVE_OPEN62541
    srv->suppress_writeback = 1;
    for (int i = 0; i < tvs->count; i++) {
        const TagValue *tv = &tvs->items[i];

        UA_NodeId *nid = NULL;
        for (int j = 0; j < srv->node_count; j++) {
            if (strncmp(srv->node_names[j], tv->name, MAX_STR_LEN) == 0) {
                nid = &srv->ns_node_ids[j];
                break;
            }
        }
        if (!nid) continue;

        UA_Variant var;
        UA_Variant_init(&var);

        switch (tv->type) {
            case TAG_TYPE_BOOL: {
                UA_Boolean v = (UA_Boolean)tv->value.bool_val;
                UA_Variant_setScalarCopy(&var, &v, &UA_TYPES[UA_TYPES_BOOLEAN]);
                break;
            }
            case TAG_TYPE_INT: {
                UA_Int16 v = (UA_Int16)tv->value.int_val;
                UA_Variant_setScalarCopy(&var, &v, &UA_TYPES[UA_TYPES_INT16]);
                break;
            }
            case TAG_TYPE_DINT: {
                UA_Int32 v = (UA_Int32)tv->value.dint_val;
                UA_Variant_setScalarCopy(&var, &v, &UA_TYPES[UA_TYPES_INT32]);
                break;
            }
            case TAG_TYPE_REAL: {
                UA_Float v = (UA_Float)tv->value.real_val;
                UA_Variant_setScalarCopy(&var, &v, &UA_TYPES[UA_TYPES_FLOAT]);
                break;
            }
            case TAG_TYPE_STRING: {
                UA_String v = UA_String_fromChars(tv->value.str_val);
                UA_Variant_setScalarCopy(&var, &v, &UA_TYPES[UA_TYPES_STRING]);
                UA_String_clear(&v);
                break;
            }
        }

        UA_Server_writeValue(srv->ua_server, *nid, var);
        UA_Variant_clear(&var);
    }
    srv->suppress_writeback = 0;
#else
    (void)tvs;
#endif
}

void opcua_server_stop(OpcUAServer *srv)
{
    if (!srv || !srv->running) return;
    srv->running = 0;

#ifdef HAVE_OPEN62541
#  ifdef _WIN32
    if (srv->thread) {
        WaitForSingleObject(srv->thread, INFINITE);
        CloseHandle(srv->thread);
        srv->thread = NULL;
    }
#  else
    pthread_join(srv->thread, NULL);
#  endif
    LOG_INFO_MSG("opcua: server stopped");
#endif
}

void opcua_server_destroy(OpcUAServer *srv)
{
    if (!srv) return;
    opcua_server_stop(srv);

#ifdef HAVE_OPEN62541
    if (srv->ua_server) {
        UA_Server_delete(srv->ua_server);
        srv->ua_server = NULL;
    }
#endif
    free(srv);
}
