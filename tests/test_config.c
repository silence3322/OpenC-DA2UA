/*
 * test_config.c – Unit tests for the configuration loader
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Write a temporary file and return a pointer to its path in a static buffer */
static const char *write_temp_file(const char *name, const char *content)
{
    static char path[256];
    snprintf(path, sizeof(path), "/tmp/%s", name);
    FILE *fp = fopen(path, "w");
    assert(fp != NULL);
    fputs(content, fp);
    fclose(fp);
    return path;
}

/* ------------------------------------------------------------------ */
static void test_config_load_basic(void)
{
    const char *json =
        "{"
        "  \"OPCDA_CLIENT\": { \"IP\": \"10.0.0.1\", \"DB_Number\": \"5\" },"
        "  \"OPCUA_SERVER\": {"
        "    \"EndPoint\": \"opc.tcp://0.0.0.0:4840\","
        "    \"TagFile\":  \"nodes.json\","
        "    \"uri\":      \"http://test.local\""
        "  },"
        "  \"security\": {"
        "    \"security_num\": \"0\","
        "    \"certificate\": \"cert.der\","
        "    \"private-key\": \"key.pem\""
        "  }"
        "}";

    const char *path = write_temp_file("test_config_basic.json", json);

    AppConfig cfg;
    int ret = config_load(path, &cfg);
    assert(ret == 0);

    assert(strcmp(cfg.opcda.ip, "10.0.0.1") == 0);
    assert(cfg.opcda.db_number == 5);
    assert(strcmp(cfg.opcua.endpoint, "opc.tcp://0.0.0.0:4840") == 0);
    assert(strcmp(cfg.opcua.uri, "http://test.local") == 0);
    assert(strcmp(cfg.opcua.tag_file, "nodes.json") == 0);
    assert(cfg.security.mode == SECURITY_NONE);
    assert(strcmp(cfg.security.certificate, "cert.der") == 0);
    assert(strcmp(cfg.security.private_key, "key.pem") == 0);

    config_free(&cfg);
    printf("PASS  test_config_load_basic\n");
}

/* ------------------------------------------------------------------ */
static void test_config_load_invalid_path(void)
{
    AppConfig cfg;
    int ret = config_load("/tmp/nonexistent_file_xyz.json", &cfg);
    assert(ret != 0);
    printf("PASS  test_config_load_invalid_path\n");
}

/* ------------------------------------------------------------------ */
static void test_config_load_invalid_json(void)
{
    const char *path = write_temp_file("test_bad.json", "{ invalid json }}}");
    AppConfig cfg;
    int ret = config_load(path, &cfg);
    assert(ret != 0);
    printf("PASS  test_config_load_invalid_json\n");
}

/* ------------------------------------------------------------------ */
static void test_nodes_load_basic(void)
{
    const char *json =
        "{"
        "  \"Bool\": { \"Tag_B0\": \"0.0\", \"Tag_B1\": \"0.1\" },"
        "  \"Int\":  { \"Tag_I0\": \"10\",  \"Tag_I1\": \"12\"  },"
        "  \"Real\": { \"Tag_R0\": \"100.0\" },"
        "  \"Dint\": { \"Tag_D0\": \"200\" },"
        "  \"String[256]\": { \"Tag_S0\": \"300\" }"
        "}";

    const char *path = write_temp_file("test_nodes_basic.json", json);

    NodeConfig ncfg;
    int ret = nodes_load(path, &ncfg);
    assert(ret == 0);
    assert(ncfg.count == 7);

    /* Check first Bool node */
    int found_b0 = 0, found_r0 = 0, found_s0 = 0;
    for (int i = 0; i < ncfg.count; i++) {
        NodeDef *nd = &ncfg.nodes[i];
        if (strcmp(nd->name, "Tag_B0") == 0) {
            assert(nd->type == TAG_TYPE_BOOL);
            assert(nd->byte_offset == 0);
            assert(nd->bit_offset  == 0);
            found_b0 = 1;
        }
        if (strcmp(nd->name, "Tag_R0") == 0) {
            assert(nd->type == TAG_TYPE_REAL);
            assert(nd->byte_offset == 100);
            found_r0 = 1;
        }
        if (strcmp(nd->name, "Tag_S0") == 0) {
            assert(nd->type == TAG_TYPE_STRING);
            assert(nd->byte_offset == 300);
            assert(nd->str_len == 256);
            found_s0 = 1;
        }
    }
    assert(found_b0 && found_r0 && found_s0);

    nodes_free(&ncfg);
    printf("PASS  test_nodes_load_basic\n");
}

/* ------------------------------------------------------------------ */
static void test_nodes_load_empty(void)
{
    const char *path = write_temp_file("test_nodes_empty.json", "{}");
    NodeConfig ncfg;
    int ret = nodes_load(path, &ncfg);
    assert(ret == 0);
    assert(ncfg.count == 0);
    printf("PASS  test_nodes_load_empty\n");
}

/* ------------------------------------------------------------------ */
int main(void)
{
    test_config_load_basic();
    test_config_load_invalid_path();
    test_config_load_invalid_json();
    test_nodes_load_basic();
    test_nodes_load_empty();

    printf("\nAll config tests passed.\n");
    return EXIT_SUCCESS;
}
