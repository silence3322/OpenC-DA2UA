# OpenC-DA2UA API Reference

OpenC-DA2UA bridges OPC-DA COM services to OPC-UA.

---

## Modules

| Module | Header | Source |
|---|---|---|
| Configuration | `src/config.h` | `src/config.c` |
| Logger | `src/logger.h` | `src/logger.c` |
| OPC-DA client | `src/opcda_client.h` | `src/opcda_client.c` |
| OPC-UA server | `src/opcua_server.h` | `src/opcua_server.c` |
| Web config server | `src/web_config.h` | `src/web_config.c` |

---

## Configuration API

### `int config_load(const char *config_path, AppConfig *cfg)`

Loads `config.json` into `cfg`.

Returns:
- `0` success
- `-1` failure

### `int config_set_opcua_port(const char *config_path, int port)`

Persists OPC-UA port by rewriting `OPCUA_SERVER.EndPoint` in `config.json`.

Returns:
- `0` success
- `-1` failure

### `int nodes_load(const char *nodes_path, NodeConfig *ncfg)`

Loads node mappings from `nodes.json`.

Each mapping value is stored as `NodeDef.source` and treated as OPC-DA ItemID in COM mode.

Returns:
- `0` success
- `-1` failure

---

## Config Data Model

### `OpcDAConfig`

| Field | Description |
|---|---|
| `mode` | DA mode (`opcda_com`, `disabled`, etc.) |
| `server_progid` | OPC-DA server ProgID |
| `host` | Hostname / IP for COM server |
| `ip` | Legacy field for non-COM path |
| `db_number` | Legacy field for non-COM path |

### `OpcUAConfig`

| Field | Description |
|---|---|
| `endpoint` | OPC-UA endpoint URL (`opc.tcp://0.0.0.0:48411`) |
| `uri` | OPC-UA namespace URI |
| `tag_file` | Node mapping file path (relative to config dir) |

### `NodeDef`

| Field | Description |
|---|---|
| `name` | OPC-UA variable node name |
| `source` | OPC-DA ItemID source string |
| `type` | Value type (`TagType`) |

---

## OPC-DA Client API

### `OpcDAClient *opcda_client_create(void)`

Creates DA client handle.

### `int opcda_client_connect(OpcDAClient *client, const char *target)`

Connects to DA source.

For COM mode, `target` format is:
- `opcda_com:<ProgID>@<Host>`

### `int opcda_client_read(OpcDAClient *client, int db_number, const NodeConfig *ncfg, TagValueSet *tvs)`

Reads one cycle of values from mapped nodes into `tvs`.

### `int opcda_client_write(OpcDAClient *client, int tag_index, const NodeConfig *ncfg, const TagValue *tv)`

Writes one value to DA item mapped at `tag_index`.

### `void opcda_client_disconnect(OpcDAClient *client)`

Disconnects DA client.

### `void opcda_client_destroy(OpcDAClient *client)`

Frees DA client.

### `int opcda_client_is_connected(const OpcDAClient *client)`

Returns non-zero when connected.

---

## OPC-UA Server API

### `OpcUAServer *opcua_server_create(const OpcUAConfig *cfg, const SecurityConfig *sec)`

Creates OPC-UA server instance.

### `int opcua_server_add_nodes(OpcUAServer *srv, const NodeConfig *ncfg, OpcUaWriteHandler write_handler, void *write_ctx)`

Adds mapped nodes and registers optional write-through callback.

`OpcUaWriteHandler` receives:
- node index
- value from UA write
- user context pointer

### `int opcua_server_start(OpcUAServer *srv)`

Starts background UA server thread.

### `void opcua_server_update(OpcUAServer *srv, const TagValueSet *tvs)`

Pushes DA values into UA nodes.

Implementation suppresses write callback during this internal update path to avoid feedback loops.

### `void opcua_server_stop(OpcUAServer *srv)`

Stops UA server thread.

### `void opcua_server_destroy(OpcUAServer *srv)`

Stops and frees UA server.

---

## Web Config API

### `int web_config_start(const char *config_path)`

Starts local HTTP configuration service (`127.0.0.1:18080`).

### `void web_config_stop(void)`

Stops web config service.

---

## Logger API

### `int logger_init(const char *log_dir, LogLevel min_level)`
### `void logger_log(LogLevel level, const char *fmt, ...)`
### `void logger_close(void)`

Convenience macros:

```c
LOG_DEBUG_MSG(fmt, ...)
LOG_INFO_MSG(fmt, ...)
LOG_WARN_MSG(fmt, ...)
LOG_ERROR_MSG(fmt, ...)
```

---

## Type Mapping

| JSON key | C `TagType` | OPC-UA type |
|---|---|---|
| `Bool` | `TAG_TYPE_BOOL` | `Boolean` |
| `Int` | `TAG_TYPE_INT` | `Int16` |
| `Real` | `TAG_TYPE_REAL` | `Float` |
| `Dint` | `TAG_TYPE_DINT` | `Int32` |
| `String[N]` | `TAG_TYPE_STRING` | `String` |
