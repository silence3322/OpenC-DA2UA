/*
 * opcua_server.c – OPC-UA server implementation for OpenC-DA2UA
 *
 * Uses the open62541 C library (https://open62541.org/).
 * Compile-time dependency:  libopen62541
 * Link with:  -lopen62541
 */
#include "opcua_server.h"
#include "logger.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifdef HAVE_OPEN62541
#  include <open62541/server.h>
#  include <open62541/server_config_default.h>
#  include <open62541/plugin/log_stdout.h>
#endif

/* ------------------------------------------------------------------
 * Internal data structure
 * ------------------------------------------------------------------ */
struct OpcUAServer {
#ifdef HAVE_OPEN62541
    UA_Server       *ua_server;
    UA_NodeId        ns_node_ids[MAX_NODES]; /* variable node IDs */
    char             node_names[MAX_NODES][MAX_STR_LEN];
    int              node_count;
    UA_UInt16        ns_index;
#else
    int              dummy;
#endif
    int              running;
    pthread_t        thread;
};

/* ------------------------------------------------------------------
 * Background thread
 * ------------------------------------------------------------------ */
#ifdef HAVE_OPEN62541
static void *server_thread(void *arg)
{
    OpcUAServer *srv = (OpcUAServer *)arg;
    UA_StatusCode sc = UA_Server_run(srv->ua_server, (volatile UA_Boolean *)&srv->running);
    if (sc != UA_STATUSCODE_GOOD)
        LOG_ERROR_MSG("opcua: server exited with status 0x%08X", sc);
    return NULL;
}
#endif

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

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

    /* Endpoint */
    UA_String endpoint = UA_String_fromChars(cfg->endpoint);
    ucfg->customHostname = UA_String_fromChars(""); /* use system hostname */
    UA_ServerConfig_setMinimalCustomBuffer(ucfg, (UA_UInt16)4840,
                                           &endpoint, 0, 0);
    UA_String_clear(&endpoint);

    /* Namespace */
    UA_String ns_uri = UA_String_fromChars(cfg->uri);
    srv->ns_index = UA_Server_addNamespace(srv->ua_server, cfg->uri);
    UA_String_clear(&ns_uri);

    /* Security */
    if (sec->mode == SECURITY_NONE) {
        LOG_INFO_MSG("opcua: security = None");
    } else {
        /* Certificate and key loading requires open62541 crypto plugin */
        LOG_INFO_MSG("opcua: security mode %d configured (certificates must "
                     "be loaded separately)", sec->mode);
    }

    LOG_INFO_MSG("opcua: server created, endpoint=%s", cfg->endpoint);
#else
    (void)sec;
    LOG_WARN_MSG("opcua: open62541 not compiled in – server is a stub");
#endif

    return srv;
}

int opcua_server_add_nodes(OpcUAServer *srv, const NodeConfig *ncfg)
{
    if (!srv || !ncfg) return -1;

#ifdef HAVE_OPEN62541
    srv->node_count = 0;

    for (int i = 0; i < ncfg->count && i < MAX_NODES; i++) {
        const NodeDef *nd = &ncfg->nodes[i];

        UA_VariableAttributes attr = UA_VariableAttributes_default;
        attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;

        /* Set an appropriate initial variant */
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

        attr.displayName = UA_LOCALIZEDTEXT("en-US", nd->name);
        attr.description = UA_LOCALIZEDTEXT("en-US", nd->name);

        UA_NodeId node_id = UA_NODEID_STRING_ALLOC(srv->ns_index, nd->name);
        UA_QualifiedName qname = UA_QUALIFIEDNAME(srv->ns_index, nd->name);
        UA_NodeId parent_id = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
        UA_NodeId ref_type  = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);

        UA_StatusCode sc = UA_Server_addVariableNode(
            srv->ua_server,
            node_id, parent_id, ref_type,
            qname,
            UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
            attr, NULL, &srv->ns_node_ids[srv->node_count]);

        UA_Variant_clear(&attr.value);
        UA_NodeId_clear(&node_id);

        if (sc != UA_STATUSCODE_GOOD) {
            LOG_WARN_MSG("opcua: add node '%s' failed: 0x%08X", nd->name, sc);
            continue;
        }

        strncpy(srv->node_names[srv->node_count], nd->name,
                MAX_STR_LEN - 1);
        srv->node_names[srv->node_count][MAX_STR_LEN - 1] = '\0';
        srv->node_count++;
    }

    LOG_INFO_MSG("opcua: %d nodes registered", srv->node_count);
    return 0;
#else
    (void)ncfg;
    return -1;
#endif
}

int opcua_server_start(OpcUAServer *srv)
{
    if (!srv) return -1;
    srv->running = 1;

#ifdef HAVE_OPEN62541
    if (pthread_create(&srv->thread, NULL, server_thread, srv) != 0) {
        LOG_ERROR_MSG("opcua: failed to start server thread");
        srv->running = 0;
        return -1;
    }
    LOG_INFO_MSG("opcua: server started");
    return 0;
#else
    LOG_WARN_MSG("opcua: open62541 not compiled in – server not started");
    return -1;
#endif
}

void opcua_server_update(OpcUAServer *srv, const TagValueSet *tvs)
{
    if (!srv || !tvs) return;

#ifdef HAVE_OPEN62541
    for (int i = 0; i < tvs->count; i++) {
        const TagValue *tv = &tvs->items[i];

        /* Find the matching node ID */
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
#else
    (void)tvs;
#endif
}

void opcua_server_stop(OpcUAServer *srv)
{
    if (!srv || !srv->running) return;
    srv->running = 0;

#ifdef HAVE_OPEN62541
    pthread_join(srv->thread, NULL);
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
