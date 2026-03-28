# OpenC-DA2UA

C语言重制版本 [Open-DA2UA](https://github.com/936804292/da2ua)，兼容性更强。

OpenC-DA2UA is a **C language** middleware that bridges **OPC-DA** (Siemens S7 PLCs via [snap7](http://snap7.sourceforge.net/)) and **OPC-UA** (via [open62541](https://open62541.org/)). It is a C remake of the Python Open-DA2UA project with stronger cross-platform compatibility and lower runtime overhead.

---

## Project Structure

```
OpenC-DA2UA/
├── CMakeLists.txt          # CMake build script
├── Makefile                # Alternative GNU Make build script
├── config/
│   ├── config.json         # Main application configuration
│   └── nodes.json          # OPC-UA node / PLC tag definitions
├── src/
│   ├── main.c              # Entry point – mirrors app.py from Open-DA2UA
│   ├── config.c/h          # JSON configuration loader
│   ├── logger.c/h          # Timestamped file logger
│   ├── opcda_client.c/h    # OPC-DA client (snap7 S7 communication)
│   └── opcua_server.c/h    # OPC-UA server (open62541)
├── third_party/
│   └── cJSON/              # Bundled single-file JSON parser
├── tests/
│   ├── test_config.c       # Unit tests for the config module
│   └── test_logger.c       # Unit tests for the logger module
├── logs/                   # Runtime log files (auto-created)
├── security/               # TLS certificates and private keys
└── docs/
    └── API.md              # Full API reference
```

---

## Dependencies

| Library | Purpose | Required? |
|---------|---------|-----------|
| [snap7](http://snap7.sourceforge.net/) | Siemens S7 PLC communication (OPC-DA side) | Yes (runtime) |
| [open62541](https://open62541.org/) | OPC-UA server | Yes (runtime) |
| [cJSON](https://github.com/DaveGamble/cJSON) | JSON parsing | Bundled |
| pthreads | Background server thread | System |

The code compiles **without** snap7 and open62541 installed (feature flags `HAVE_SNAP7` / `HAVE_OPEN62541` are off by default). This allows you to build and run the tests on any POSIX system.

### Install dependencies (Ubuntu / Debian)

```bash
# snap7
sudo apt-get install libsnap7-dev

# open62541 (v1.3+)
sudo apt-get install libopen62541-dev
# or build from source: https://github.com/open62541/open62541
```

---

## Build

### CMake (recommended)

```bash
mkdir build && cd build

# Without optional backends (stub mode – good for tests):
cmake ..
make

# With snap7 and open62541:
cmake .. -DWITH_SNAP7=ON -DWITH_OPEN62541=ON
make
```

### GNU Make

```bash
# Stub mode:
make

# With both backends:
make WITH_SNAP7=1 WITH_OPEN62541=1
```

---

## Configuration

Edit `config/config.json`:

```json
{
    "OPCDA_CLIENT": {
        "IP": "192.168.20.219",
        "DB_Number": "2"
    },
    "OPCUA_SERVER": {
        "EndPoint": "opc.tcp://0.0.0.0:48411",
        "TagFile": "nodes.json",
        "uri": "http://example.com/opcda2ua"
    },
    "security": {
        "security_num": "0",
        "certificate": "kz_cert.der",
        "private-key": "kz_private_key.pem"
    }
}
```

Edit `config/nodes.json` to define which PLC data-block addresses are exposed as OPC-UA nodes:

```json
{
    "Bool":      { "Tag_B0": "0.0", "Tag_B1": "0.1" },
    "Int":       { "Tag_I0": "126", "Tag_I1": "128" },
    "Real":      { "Tag_R0": "2126.0" },
    "Dint":      { "Tag_D0": "200" },
    "String[256]": { "Tag_S0": "6126" }
}
```

---

## Security (TLS)

Generate a self-signed certificate:

```bash
openssl req -x509 -newkey rsa:2048 \
    -keyout security/kz_private_key.pem \
    -out security/kz_cert.pem \
    -days 365 -nodes -config ssl.conf
openssl x509 -outform der \
    -in security/kz_cert.pem \
    -out security/kz_cert.der
```

Set `"security_num": "1"` in `config.json` to enable `Basic256Sha256_SignAndEncrypt`.

---

## Run

```bash
./opcda2ua [config_dir]
```

`config_dir` defaults to `./config`.

Press **Ctrl-C** to stop gracefully.

---

## Tests

```bash
# CMake
cd build && ctest --output-on-failure

# GNU Make
make test
```

---

## API Reference

See [`docs/API.md`](docs/API.md) for the full module API.
