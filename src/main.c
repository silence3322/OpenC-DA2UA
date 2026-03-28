/*
 * main.c – Entry point for OpenC-DA2UA
 *
 * OPC-DA (Siemens S7 via snap7) → OPC-UA (open62541) bridge.
 * Mirrors the run() function in the Python reference project app.py.
 */
#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "logger.h"
#include "opcda_client.h"
#include "opcua_server.h"
#include "web_config.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#  include <windows.h>
#endif
/* ------------------------------------------------------------------
 * Graceful shutdown via SIGINT / SIGTERM
 * ------------------------------------------------------------------ */
static volatile int g_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* Cross-platform sleep helper (milliseconds). */
static void sleep_ms(unsigned int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000U);
    ts.tv_nsec = (long)((ms % 1000U) * 1000000UL);
    nanosleep(&ts, NULL);
#endif
}

static int str_ieq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static int opcda_mode_disabled(const char *mode)
{
    return str_ieq(mode, "disabled") || str_ieq(mode, "none");
}

static int opcda_mode_com(const char *mode)
{
    return str_ieq(mode, "opcda_com");
}

static int endpoint_port_or_default(const char *endpoint)
{
    if (!endpoint || !endpoint[0]) return 4840;
    const char *colon = strrchr(endpoint, ':');
    if (!colon || !colon[1]) return 4840;
    int p = atoi(colon + 1);
    if (p <= 0 || p > 65535) return 4840;
    return p;
}

static void format_opcda_error_message(const OpcDAClient *client, const char *progid,
                                       char *out, size_t out_sz)
{
    unsigned long hr = opcda_client_last_error_code(client);
    const char *text = opcda_client_last_error_text(client);
    if (!out || out_sz == 0) return;

    if (hr == 0x80070422UL) {
        snprintf(out, out_sz,
                 "OPCDA connect failed: 0x%08lX. Related service is disabled or not started. Check WinCC/Kepware service state for %s.",
                 hr, (progid && progid[0]) ? progid : "target");
    } else if (hr == 0x80040154UL) {
        snprintf(out, out_sz,
                 "OPCDA connect failed: 0x%08lX. ProgID is not registered correctly or 32/64-bit registry views do not match.",
                 hr);
    } else if (hr != 0UL) {
        snprintf(out, out_sz,
                 "OPCDA connect failed: 0x%08lX (%s)",
                 hr, (text && text[0]) ? text : "unknown");
    } else {
        snprintf(out, out_sz, "OPCDA connect failed, waiting for retry...");
    }
}

static void format_opcda_runtime_message(const OpcDAClient *client, char *out, size_t out_sz)
{
    unsigned long hr = opcda_client_last_error_code(client);
    const char *text = opcda_client_last_error_text(client);
    if (!out || out_sz == 0) return;

    if (hr != 0UL) {
        snprintf(out, out_sz, "OPCDA runtime issue: 0x%08lX (%s)",
                 hr, (text && text[0]) ? text : "unknown");
    } else {
        snprintf(out, out_sz, "OPCDA read failed. Waiting next retry...");
    }
}

typedef struct {
    OpcDAClient *client;
    const NodeConfig *ncfg;
} WriteBridgeCtx;

static int on_opcua_write_to_opcda(int node_index, const TagValue *value, void *user_ctx)
{
    WriteBridgeCtx *ctx = (WriteBridgeCtx *)user_ctx;
    if (!ctx || !ctx->client || !ctx->ncfg || !value) return -1;
    return opcda_client_write(ctx->client, node_index, ctx->ncfg, value);
}

/* ------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------ */

/* Resolve a path relative to a base directory.
 * dst must have room for MAX_PATH_LEN characters. */
static void resolve_path(char *dst, const char *base_dir, const char *rel)
{
    if (rel[0] == '/' || rel[0] == '\\') {
        /* Already absolute */
        strncpy(dst, rel, MAX_PATH_LEN - 1);
        dst[MAX_PATH_LEN - 1] = '\0';
    } else {
        snprintf(dst, MAX_PATH_LEN, "%s/%s", base_dir, rel);
    }
}

/* ------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    /* Accept optional config directory as first argument */
    const char *config_dir = (argc > 1) ? argv[1] : "./config";

    /* ---- Logging ---- */
    if (logger_init("./logs", LOG_WARN) != 0) {
        fprintf(stderr, "warning: could not create log file, logging to stderr only\n");
    }

    LOG_WARN_MSG("OpenC-DA2UA starting up");

    /* ---- Load main configuration ---- */
    char config_path[MAX_PATH_LEN];
    snprintf(config_path, sizeof(config_path), "%s/config.json", config_dir);

    AppConfig cfg;
    if (config_load(config_path, &cfg) != 0) {
        LOG_ERROR_MSG("Failed to load configuration from '%s'", config_path);
        logger_close();
        return EXIT_FAILURE;
    }

    /* ---- Load node definitions ---- */
    char nodes_path[MAX_PATH_LEN];
    resolve_path(nodes_path, config_dir, cfg.opcua.tag_file);

    NodeConfig ncfg;
    if (nodes_load(nodes_path, &ncfg) != 0) {
        LOG_ERROR_MSG("Failed to load node definitions from '%s'", nodes_path);
        config_free(&cfg);
        logger_close();
        return EXIT_FAILURE;
    }

    LOG_INFO_MSG("Loaded %d nodes from '%s'", ncfg.count, nodes_path);

    if (web_config_start(config_path) != 0) {
        LOG_WARN_MSG("web-config: failed to start on http://127.0.0.1:18080");
    }

    /* ---- Create and start OPC-UA server ---- */
    OpcUAServer *ua_srv = opcua_server_create(&cfg.opcua, &cfg.security);
    if (!ua_srv) {
        LOG_ERROR_MSG("Failed to create OPC-UA server");
        config_free(&cfg);
        logger_close();
        return EXIT_FAILURE;
    }

    WriteBridgeCtx bridge;
    bridge.client = NULL;
    bridge.ncfg = &ncfg;

    if (opcua_server_add_nodes(ua_srv, &ncfg, on_opcua_write_to_opcda, &bridge) != 0) {
        LOG_WARN_MSG("Some nodes could not be added to the OPC-UA server");
    }

    if (opcua_server_start(ua_srv) != 0) {
        LOG_ERROR_MSG("Failed to start OPC-UA server");
        web_config_set_opcua_status(1, 0, endpoint_port_or_default(cfg.opcua.endpoint),
                                    "OPCUA server start failed");
        opcua_server_destroy(ua_srv);
        config_free(&cfg);
        logger_close();
        return EXIT_FAILURE;
    }

#ifdef HAVE_OPEN62541
    web_config_set_opcua_status(1, 1, endpoint_port_or_default(cfg.opcua.endpoint),
                                "OPCUA server listening");
#else
    web_config_set_opcua_status(0, 0, endpoint_port_or_default(cfg.opcua.endpoint),
                                "open62541 disabled: OPCUA port is not listening");
#endif

    LOG_WARN_MSG("OPC-UA server started at %s", cfg.opcua.endpoint);

    /* ---- Create OPC-DA (S7) client ---- */
    int da_disabled = opcda_mode_disabled(cfg.opcda.mode);
    int da_com_mode = opcda_mode_com(cfg.opcda.mode);
    const char *da_target = cfg.opcda.ip;
    char da_target_buf[MAX_PATH_LEN];
    OpcDAClient *da_client = NULL;

    if (!da_disabled) {
        web_config_set_gateway_status(1, 0, cfg.opcda.server_progid,
                                      "Gateway enabled. Connecting OPCDA source...");
        if (da_com_mode && cfg.opcda.server_progid[0] != '\0') {
            if (cfg.opcda.host[0] != '\0') {
                snprintf(da_target_buf, sizeof(da_target_buf), "opcda_com:%s@%s",
                         cfg.opcda.server_progid, cfg.opcda.host);
            } else {
                snprintf(da_target_buf, sizeof(da_target_buf), "opcda_com:%s",
                         cfg.opcda.server_progid);
            }
            da_target = da_target_buf;
        }

        da_client = opcda_client_create();
        if (!da_client) {
            LOG_ERROR_MSG("Failed to create OPC-DA client");
            opcua_server_stop(ua_srv);
            opcua_server_destroy(ua_srv);
            config_free(&cfg);
            logger_close();
            return EXIT_FAILURE;
        }

        if (opcda_client_connect(da_client, da_target) != 0) {
            char diag[256];
            LOG_ERROR_MSG("Failed to connect OPC-DA source: %s", da_target);
            /* Continue anyway – the poll loop will retry */
            format_opcda_error_message(da_client, cfg.opcda.server_progid, diag, sizeof(diag));
            web_config_set_gateway_status(1, 0, cfg.opcda.server_progid, diag);
        } else {
            web_config_set_gateway_status(1, 1, cfg.opcda.server_progid,
                                          "OPCDA connected, gateway running.");
        }
        bridge.client = da_client;
    } else {
        LOG_WARN_MSG("OPC-DA client disabled by configuration (mode=%s)",
                     cfg.opcda.mode[0] ? cfg.opcda.mode : "disabled");
        web_config_set_gateway_status(0, 0, cfg.opcda.server_progid,
                                      "Gateway disabled by configuration.");
    }

    /* ---- Signal handlers ---- */
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* ---- Main poll loop (mirrors the Python while True loop) ---- */
    LOG_WARN_MSG("Entering poll loop  (Ctrl-C to stop)");

    while (g_running) {
        int runtime_enabled = web_config_get_gateway_runtime_enabled();

        if (!da_disabled && !runtime_enabled) {
            if (da_client && opcda_client_is_connected(da_client)) {
                opcda_client_disconnect(da_client);
            }
            web_config_set_gateway_status(1, 0, cfg.opcda.server_progid,
                                          "Gateway runtime stopped by user.");
            sleep_ms(200U);
            continue;
        }

        if (!da_disabled && !opcda_client_is_connected(da_client)) {
            /* Attempt reconnect */
            char diag[256];
            LOG_WARN_MSG("OPC-DA source not connected – retrying...");
            opcda_client_connect(da_client, da_target);
            format_opcda_error_message(da_client, cfg.opcda.server_progid, diag, sizeof(diag));
            web_config_set_gateway_status(1, 0, cfg.opcda.server_progid, diag);
            sleep_ms(5000U);
            continue;
        }

        if (!da_disabled) {
            TagValueSet tvs;
            if (ncfg.count <= 0) {
                web_config_set_gateway_status(1, 1, cfg.opcda.server_progid,
                                              "OPCDA connected. No mapped points yet. Browse KEP points and configure nodes.json before binding.");
            } else if (opcda_client_read(da_client, cfg.opcda.db_number, &ncfg, &tvs) == 0) {
                opcua_server_update(ua_srv, &tvs);
                web_config_set_gateway_status(1, 1, cfg.opcda.server_progid,
                                              "Gateway active. OPCDA read/update OK.");
            } else {
                char diag[256];
                LOG_WARN_MSG("OPC-DA read failed – will retry");
                format_opcda_runtime_message(da_client, diag, sizeof(diag));
                web_config_set_gateway_status(1, 1, cfg.opcda.server_progid, diag);
            }
        }

        /* 100 ms poll interval */
        sleep_ms(100U);
    }

    /* ---- Graceful shutdown ---- */
    LOG_WARN_MSG("Shutting down...");
    web_config_set_gateway_status(!da_disabled, 0, cfg.opcda.server_progid,
                                  "Gateway stopping...");

    if (da_client && opcda_client_is_connected(da_client)) {
        opcda_client_disconnect(da_client);
        LOG_WARN_MSG("OPC-DA client disconnected");
    }
    if (da_client) {
        opcda_client_destroy(da_client);
    }

    opcua_server_stop(ua_srv);
    web_config_set_opcua_status(0, 0, endpoint_port_or_default(cfg.opcua.endpoint),
                                "OPCUA server stopped");
    LOG_WARN_MSG("OPC-UA server stopped");
    opcua_server_destroy(ua_srv);
    web_config_stop();

    config_free(&cfg);
    nodes_free(&ncfg);
    logger_close();

    return EXIT_SUCCESS;
}
