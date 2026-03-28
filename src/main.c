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

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
/* ------------------------------------------------------------------
 * Graceful shutdown via SIGINT / SIGTERM
 * ------------------------------------------------------------------ */
static volatile int g_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
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

    /* ---- Create and start OPC-UA server ---- */
    OpcUAServer *ua_srv = opcua_server_create(&cfg.opcua, &cfg.security);
    if (!ua_srv) {
        LOG_ERROR_MSG("Failed to create OPC-UA server");
        config_free(&cfg);
        logger_close();
        return EXIT_FAILURE;
    }

    if (opcua_server_add_nodes(ua_srv, &ncfg) != 0) {
        LOG_WARN_MSG("Some nodes could not be added to the OPC-UA server");
    }

    if (opcua_server_start(ua_srv) != 0) {
        LOG_ERROR_MSG("Failed to start OPC-UA server");
        opcua_server_destroy(ua_srv);
        config_free(&cfg);
        logger_close();
        return EXIT_FAILURE;
    }

    LOG_WARN_MSG("OPC-UA server started at %s", cfg.opcua.endpoint);

    /* ---- Create OPC-DA (S7) client ---- */
    OpcDAClient *da_client = opcda_client_create();
    if (!da_client) {
        LOG_ERROR_MSG("Failed to create OPC-DA client");
        opcua_server_stop(ua_srv);
        opcua_server_destroy(ua_srv);
        config_free(&cfg);
        logger_close();
        return EXIT_FAILURE;
    }

    if (opcda_client_connect(da_client, cfg.opcda.ip) != 0) {
        LOG_ERROR_MSG("Failed to connect to PLC at %s", cfg.opcda.ip);
        /* Continue anyway – the poll loop will retry */
    }

    /* ---- Signal handlers ---- */
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* ---- Main poll loop (mirrors the Python while True loop) ---- */
    LOG_WARN_MSG("Entering poll loop  (Ctrl-C to stop)");

    while (g_running) {
        if (!opcda_client_is_connected(da_client)) {
            /* Attempt reconnect */
            LOG_WARN_MSG("PLC not connected – retrying...");
            opcda_client_connect(da_client, cfg.opcda.ip);
            struct timespec ts_retry = { 5, 0 };
            nanosleep(&ts_retry, NULL);
            continue;
        }

        TagValueSet tvs;
        if (opcda_client_read(da_client, cfg.opcda.db_number, &ncfg, &tvs) == 0) {
            opcua_server_update(ua_srv, &tvs);
        } else {
            LOG_WARN_MSG("DB read failed – will retry");
        }

        /* 100 ms poll interval */
        struct timespec ts = { 0, 100000000L }; /* 100 ms */
        nanosleep(&ts, NULL);
    }

    /* ---- Graceful shutdown ---- */
    LOG_WARN_MSG("Shutting down...");

    if (opcda_client_is_connected(da_client)) {
        opcda_client_disconnect(da_client);
        LOG_WARN_MSG("OPC-DA client disconnected");
    }
    opcda_client_destroy(da_client);

    opcua_server_stop(ua_srv);
    LOG_WARN_MSG("OPC-UA server stopped");
    opcua_server_destroy(ua_srv);

    config_free(&cfg);
    nodes_free(&ncfg);
    logger_close();

    return EXIT_SUCCESS;
}
