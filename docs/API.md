# OpenC-DA2UA API Reference

OpenC-DA2UA is a C language middleware that bridges **OPC-DA** (Siemens S7 PLCs via snap7) and **OPC-UA** (open62541). It is the C remake of the Python [Open-DA2UA](https://github.com/936804292/da2ua) project with stronger cross-platform compatibility.

---

## Modules

| Module | Header | Source |
|---|---|---|
| Configuration loader | `src/config.h` | `src/config.c` |
| Logger | `src/logger.h` | `src/logger.c` |
| OPC-DA client | `src/opcda_client.h` | `src/opcda_client.c` |
| OPC-UA server | `src/opcua_server.h` | `src/opcua_server.c` |

---

## Configuration (`config.h / config.c`)

### `int config_load(const char *config_path, AppConfig *cfg)`

Parses `config.json` and populates `cfg`.

| Parameter | Description |
|---|---|
| `config_path` | Absolute or relative path to `config.json` |
| `cfg` | Output structure |

Returns `0` on success, `-1` on failure.

---

### `int nodes_load(const char *nodes_path, NodeConfig *ncfg)`

Parses the node-definition JSON file (e.g. `nodes.json`) and populates `ncfg`.

The JSON format mirrors the Python project:

```json
{
    "Bool":      { "TagName": "byte_offset.bit_offset" },
    "Int":       { "TagName": "byte_offset" },
    "Real":      { "TagName": "byte_offset" },
    "Dint":      { "TagName": "byte_offset" },
    "String[N]": { "TagName": "byte_offset" }
}
```

Returns `0` on success, `-1` on failure.

---

### `AppConfig` fields

```c
typedef struct {
    OpcDAConfig    opcda;      // PLC connection settings
    OpcUAConfig    opcua;      // UA server settings
    SecurityConfig security;   // TLS/certificate settings
} AppConfig;
```

| Field | Type | Description |
|---|---|---|
| `opcda.ip` | `char[]` | PLC IP address |
| `opcda.db_number` | `int` | S7 data block number |
| `opcua.endpoint` | `char[]` | OPC-UA endpoint URL (e.g. `opc.tcp://0.0.0.0:48411`) |
| `opcua.uri` | `char[]` | OPC-UA namespace URI |
| `opcua.tag_file` | `char[]` | Path to node-definition JSON (relative to config dir) |
| `security.mode` | `SecurityMode` | `SECURITY_NONE`, `SECURITY_BASIC256SHA256_SIGN_ENCRYPT`, or `SECURITY_BASIC256SHA256_SIGN` |
| `security.certificate` | `char[]` | DER certificate path (relative to `security/` dir) |
| `security.private_key` | `char[]` | PEM private key path (relative to `security/` dir) |

---

## Logger (`logger.h / logger.c`)

### `int logger_init(const char *log_dir, LogLevel min_level)`

Creates a timestamped log file inside `log_dir`.  
Returns `0` on success, `-1` on failure.

### `void logger_log(LogLevel level, const char *fmt, ...)`

Writes a `printf`-style message at the specified level.

### Convenience macros

```c
LOG_DEBUG_MSG(fmt, ...)
LOG_INFO_MSG(fmt, ...)
LOG_WARN_MSG(fmt, ...)
LOG_ERROR_MSG(fmt, ...)
```

### `void logger_close(void)`

Flushes and closes the log file.

---

## OPC-DA Client (`opcda_client.h / opcda_client.c`)

Wraps the [snap7](http://snap7.sourceforge.net/) C library for Siemens S7 PLC communication.

### `OpcDAClient *opcda_client_create(void)`

Allocates a new client handle.  Returns `NULL` on failure.

### `int opcda_client_connect(OpcDAClient *client, const char *ip)`

Connects to a PLC at `ip` (rack=0, slot=1).  
Returns `0` on success.

### `int opcda_client_read(OpcDAClient *client, int db_number, const NodeConfig *ncfg, TagValueSet *tvs)`

Reads one cycle of data from `db_number` and populates `tvs`.

### `void opcda_client_disconnect(OpcDAClient *client)`

Closes the PLC connection.

### `void opcda_client_destroy(OpcDAClient *client)`

Frees the client handle.

### `int opcda_client_is_connected(const OpcDAClient *client)`

Returns non-zero if the client is currently connected.

---

## OPC-UA Server (`opcua_server.h / opcua_server.c`)

Wraps the [open62541](https://open62541.org/) C library.

### `OpcUAServer *opcua_server_create(const OpcUAConfig *cfg, const SecurityConfig *sec)`

Creates and configures the server.  Returns `NULL` on failure.

### `int opcua_server_add_nodes(OpcUAServer *srv, const NodeConfig *ncfg)`

Registers all nodes from `ncfg` as OPC-UA variables.  
Returns `0` on success.

### `int opcua_server_start(OpcUAServer *srv)`

Starts the server in a background thread.  
Returns `0` on success.

### `void opcua_server_update(OpcUAServer *srv, const TagValueSet *tvs)`

Pushes fresh tag values from `tvs` into the live OPC-UA nodes.

### `void opcua_server_stop(OpcUAServer *srv)`

Signals the background thread to stop and waits for it.

### `void opcua_server_destroy(OpcUAServer *srv)`

Stops and frees the server.

---

## Supported Data Types

| JSON key | C `TagType` | OPC-UA type | S7 bytes |
|---|---|---|---|
| `Bool` | `TAG_TYPE_BOOL` | `Boolean` | 1 |
| `Int` | `TAG_TYPE_INT` | `Int16` | 2 |
| `Real` | `TAG_TYPE_REAL` | `Float` | 4 |
| `Dint` | `TAG_TYPE_DINT` | `Int32` | 4 |
| `String[N]` | `TAG_TYPE_STRING` | `String` | N |
