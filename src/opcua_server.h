#ifndef OPCUA_SERVER_H
#define OPCUA_SERVER_H

#include "config.h"
#include "opcda_client.h"

/* Opaque handle for the OPC-UA server */
typedef struct OpcUAServer OpcUAServer;

typedef int (*OpcUaWriteHandler)(int node_index, const TagValue *value, void *user_ctx);

/* Create and configure the OPC-UA server.
 * Returns NULL on failure. */
OpcUAServer *opcua_server_create(const OpcUAConfig *cfg,
                                 const SecurityConfig *sec);

/* Add all nodes defined in ncfg.
 * Returns 0 on success. */
int opcua_server_add_nodes(OpcUAServer *srv, const NodeConfig *ncfg,
                           OpcUaWriteHandler write_handler, void *write_ctx);

/* Start the server (non-blocking – spawns background thread).
 * Returns 0 on success. */
int opcua_server_start(OpcUAServer *srv);

/* Push a fresh set of tag values into the live nodes. */
void opcua_server_update(OpcUAServer *srv, const TagValueSet *tvs);

/* Stop and destroy the server */
void opcua_server_stop(OpcUAServer *srv);
void opcua_server_destroy(OpcUAServer *srv);

#endif /* OPCUA_SERVER_H */
