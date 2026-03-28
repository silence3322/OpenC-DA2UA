# OpenC-DA2UA

OpenC-DA2UA is a C middleware that bridges a local OPC-DA server to OPC-UA.

This project is now centered on Windows OPC-DA COM and OPC-UA interoperability:
- Native OPC-DA COM access (`IOPCServer`, `IOPCItemMgt`, `IOPCSyncIO`)
- OPC-UA data publish for external clients
- OPC-UA write-through to OPC-DA items
- Local web page for OPC-UA port configuration

---

## Project Structure

```text
OpenC-DA2UA/
|-- CMakeLists.txt
|-- config/
|   |-- config.json
|   `-- nodes.json
|-- docs/
|   `-- API.md
|-- src/
|   |-- main.c
|   |-- config.c
|   |-- config.h
|   |-- logger.c
|   |-- logger.h
|   |-- opcda_client.c
|   |-- opcda_client.h
|   |-- opcua_server.c
|   |-- opcua_server.h
|   |-- web_config.c
|   `-- web_config.h
|-- tests/
|   |-- test_config.c
|   `-- test_logger.c
`-- third_party/
        `-- cJSON/
```

---

## Runtime Dependencies

| Component | Purpose |
|---|---|
| OPC-DA Server (COM) | Source data service (Kepware / Matrikon / custom DA server) |
| open62541 | OPC-UA server implementation |
| cJSON | JSON parsing (bundled) |
| WinSock2 | Embedded web config page (`127.0.0.1:18080`) |

Notes:
- If `WITH_OPEN62541=OFF`, the program runs in stub mode and does not expose a real OPC-UA endpoint.
- OPC-DA access in this refactor is primarily `Mode=opcda_com`.

---

## Build

### Windows (MSVC, recommended)

1. Ensure your open62541 installation provides:
- `include/open62541/server.h`
- `lib/open62541.lib` (or `open62541d.lib`)

2. Set environment variable:

```powershell
$env:OPEN62541_ROOT = "D:/deps/open62541"
```

3. Configure and build:

```powershell
cmake -S . -B build -DWITH_OPEN62541=ON
cmake --build build --config Debug
```

If open62541 cannot be found, CMake prints a stub-mode warning.

### Linux / POSIX (optional)

```bash
cmake -S . -B build -DWITH_OPEN62541=ON
cmake --build build
```

---

## Configuration

### `config/config.json`

```json
{
    "OPCDA_CLIENT": {
        "Mode": "opcda_com",
        "ServerProgID": "Matrikon.OPC.Simulation.1",
        "Host": "localhost",
        "IP": "",
        "DB_Number": "0"
    },
    "OPCUA_SERVER": {
        "EndPoint": "opc.tcp://0.0.0.0:48411",
        "TagFile": "nodes.json",
        "uri": "http://example.com/opcda2ua"
    },
    "WEB_CONFIG": {
        "Listen": "http://127.0.0.1:18080",
        "Note": "Use browser to update OPCUA port, restart service after saving"
    },
    "security": {
        "security_num": "0",
        "certificate": "kz_cert.der",
        "private-key": "kz_private_key.pem"
    }
}
```

### `config/nodes.json`

`nodes.json` maps OPC-UA node names to OPC-DA ItemIDs.

```json
{
    "Bool": { "Line_Run": "Random.Boolean" },
    "Int": { "Batch_Count": "Random.Int2" },
    "Real": { "Temperature": "Random.Real8" },
    "Dint": { "Total_Output": "Random.Int4" },
    "String[256]": { "Operator": "Bucket Brigade.String" }
}
```

---

## Run

```powershell
.\build\Debug\opcda2ua.exe .\config
```

`config_dir` defaults to `./config` if omitted.

---

## Web Port Configuration

After service starts, open:

```text
http://127.0.0.1:18080
```

You can change OPC-UA port there. The page writes to `config/config.json` (`OPCUA_SERVER.EndPoint`).

Restart service after saving to apply the new endpoint.

---

## Behavior Summary

- OPC-DA -> OPC-UA:
    Program reads configured OPC-DA ItemIDs and updates OPC-UA nodes.
- OPC-UA write -> OPC-DA:
    When an OPC-UA client writes a node value, middleware writes through to the mapped OPC-DA item.
- Internal update guard:
    DA->UA update path suppresses write-back callback to avoid feedback loops.

---

## API Reference

See [docs/API.md](docs/API.md).
