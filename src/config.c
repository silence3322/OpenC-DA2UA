/*
 * config.c – JSON configuration loader for OpenC-DA2UA
 *
 * Reads config.json and the node-definition JSON file.
 * Uses cJSON (bundled in third_party/cJSON).
 */
#include "config.h"
#include "../third_party/cJSON/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static char *read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return NULL; }

    size_t read = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    buf[read] = '\0';
    return buf;
}

static void safe_strncpy(char *dst, const char *src, size_t n)
{
    strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int config_load(const char *config_path, AppConfig *cfg)
{
    if (!config_path || !cfg) return -1;
    memset(cfg, 0, sizeof(*cfg));

    char *text = read_file(config_path);
    if (!text) {
        fprintf(stderr, "config: cannot open '%s'\n", config_path);
        return -1;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        fprintf(stderr, "config: JSON parse error: %s\n", cJSON_GetErrorPtr());
        return -1;
    }

    /* OPCDA_CLIENT */
    cJSON *da = cJSON_GetObjectItemCaseSensitive(root, "OPCDA_CLIENT");
    if (da) {
        cJSON *ip = cJSON_GetObjectItemCaseSensitive(da, "IP");
        if (cJSON_IsString(ip))
            safe_strncpy(cfg->opcda.ip, ip->valuestring, sizeof(cfg->opcda.ip));

        cJSON *db = cJSON_GetObjectItemCaseSensitive(da, "DB_Number");
        if (cJSON_IsString(db))
            cfg->opcda.db_number = atoi(db->valuestring);
        else if (cJSON_IsNumber(db))
            cfg->opcda.db_number = db->valueint;
    }

    /* OPCUA_SERVER */
    cJSON *ua = cJSON_GetObjectItemCaseSensitive(root, "OPCUA_SERVER");
    if (ua) {
        cJSON *ep  = cJSON_GetObjectItemCaseSensitive(ua, "EndPoint");
        cJSON *uri = cJSON_GetObjectItemCaseSensitive(ua, "uri");
        cJSON *tf  = cJSON_GetObjectItemCaseSensitive(ua, "TagFile");
        if (cJSON_IsString(ep))  safe_strncpy(cfg->opcua.endpoint, ep->valuestring,  sizeof(cfg->opcua.endpoint));
        if (cJSON_IsString(uri)) safe_strncpy(cfg->opcua.uri,      uri->valuestring,  sizeof(cfg->opcua.uri));
        if (cJSON_IsString(tf))  safe_strncpy(cfg->opcua.tag_file, tf->valuestring,  sizeof(cfg->opcua.tag_file));
    }

    /* security */
    cJSON *sec = cJSON_GetObjectItemCaseSensitive(root, "security");
    if (sec) {
        cJSON *num  = cJSON_GetObjectItemCaseSensitive(sec, "security_num");
        cJSON *cert = cJSON_GetObjectItemCaseSensitive(sec, "certificate");
        cJSON *key  = cJSON_GetObjectItemCaseSensitive(sec, "private-key");

        int snum = 0;
        if (cJSON_IsString(num)) snum = atoi(num->valuestring);
        else if (cJSON_IsNumber(num)) snum = num->valueint;
        cfg->security.mode = (SecurityMode)snum;

        if (cJSON_IsString(cert)) safe_strncpy(cfg->security.certificate, cert->valuestring, sizeof(cfg->security.certificate));
        if (cJSON_IsString(key))  safe_strncpy(cfg->security.private_key, key->valuestring,  sizeof(cfg->security.private_key));
    }

    cJSON_Delete(root);
    return 0;
}

int nodes_load(const char *nodes_path, NodeConfig *ncfg)
{
    if (!nodes_path || !ncfg) return -1;
    memset(ncfg, 0, sizeof(*ncfg));

    char *text = read_file(nodes_path);
    if (!text) {
        fprintf(stderr, "nodes: cannot open '%s'\n", nodes_path);
        return -1;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        fprintf(stderr, "nodes: JSON parse error: %s\n", cJSON_GetErrorPtr());
        return -1;
    }

    /*
     * Expected format (mirrors the Python reference project):
     * {
     *   "Bool":      { "TagName": "byte_offset.bit_offset", ... },
     *   "Int":       { "TagName": "byte_offset",            ... },
     *   "Real":      { "TagName": "byte_offset",            ... },
     *   "Dint":      { "TagName": "byte_offset",            ... },
     *   "String[N]": { "TagName": "byte_offset",            ... }
     * }
     */
    struct {
        const char *key;
        TagType     type;
    } type_map[] = {
        { "Bool",      TAG_TYPE_BOOL   },
        { "Int",       TAG_TYPE_INT    },
        { "Real",      TAG_TYPE_REAL   },
        { "Dint",      TAG_TYPE_DINT   },
        { "String",    TAG_TYPE_STRING },
        { NULL, 0 }
    };

    cJSON *type_obj = root->child;
    while (type_obj) {
        /* Determine tag type */
        TagType ttype = (TagType)-1;
        int str_len   = 256;

        for (int i = 0; type_map[i].key != NULL; i++) {
            if (strncmp(type_obj->string, type_map[i].key,
                        strlen(type_map[i].key)) == 0) {
                ttype = type_map[i].type;
                /* Parse optional length suffix, e.g. "String[12]" */
                if (ttype == TAG_TYPE_STRING) {
                    const char *p = strchr(type_obj->string, '[');
                    if (p) str_len = atoi(p + 1);
                }
                break;
            }
        }

        if ((int)ttype == -1) {
            type_obj = type_obj->next;
            continue;
        }

        /* Iterate tag names */
        cJSON *tag = type_obj->child;
        while (tag && ncfg->count < MAX_NODES) {
            NodeDef *nd = &ncfg->nodes[ncfg->count];
            safe_strncpy(nd->name, tag->string, sizeof(nd->name));
            nd->type    = ttype;
            nd->str_len = str_len;

            const char *val_str = NULL;
            if (cJSON_IsString(tag)) val_str = tag->valuestring;

            if (val_str) {
                char buf[64];
                safe_strncpy(buf, val_str, sizeof(buf));
                char *dot = strchr(buf, '.');
                if (dot) {
                    *dot = '\0';
                    nd->byte_offset = atoi(buf);
                    nd->bit_offset  = atoi(dot + 1);
                } else {
                    nd->byte_offset = (int)atof(val_str);
                    nd->bit_offset  = 0;
                }
            }

            ncfg->count++;
            tag = tag->next;
        }

        type_obj = type_obj->next;
    }

    cJSON_Delete(root);
    return 0;
}

void config_free(AppConfig *cfg)  { (void)cfg; }
void nodes_free(NodeConfig *ncfg) { (void)ncfg; }
