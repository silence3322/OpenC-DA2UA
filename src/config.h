#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

#define MAX_PATH_LEN    256
#define MAX_STR_LEN     128
#define MAX_NODES       1024

/* Security modes */
typedef enum {
    SECURITY_NONE = 0,
    SECURITY_BASIC256SHA256_SIGN_ENCRYPT = 1,
    SECURITY_BASIC256SHA256_SIGN = 2
} SecurityMode;

/* OPC-DA client configuration */
typedef struct {
    char ip[MAX_STR_LEN];
    int  db_number;
} OpcDAConfig;

/* OPC-UA server configuration */
typedef struct {
    char endpoint[MAX_STR_LEN];
    char uri[MAX_STR_LEN];
    char tag_file[MAX_PATH_LEN];
} OpcUAConfig;

/* Security configuration */
typedef struct {
    SecurityMode mode;
    char certificate[MAX_PATH_LEN];
    char private_key[MAX_PATH_LEN];
} SecurityConfig;

/* Top-level application configuration */
typedef struct {
    OpcDAConfig  opcda;
    OpcUAConfig  opcua;
    SecurityConfig security;
} AppConfig;

/* Data type tags supported */
typedef enum {
    TAG_TYPE_BOOL,
    TAG_TYPE_INT,
    TAG_TYPE_REAL,
    TAG_TYPE_DINT,
    TAG_TYPE_STRING
} TagType;

/* A single node/tag definition */
typedef struct {
    char     name[MAX_STR_LEN];   /* tag name shown in OPC-UA */
    TagType  type;
    int      byte_offset;
    int      bit_offset;          /* only for Bool */
    int      str_len;             /* only for String */
} NodeDef;

/* Collection of node definitions */
typedef struct {
    NodeDef  nodes[MAX_NODES];
    int      count;
} NodeConfig;

/* Read the application configuration from config_path */
int config_load(const char *config_path, AppConfig *cfg);

/* Read the node definitions from nodes_path */
int nodes_load(const char *nodes_path, NodeConfig *ncfg);

/* Free any resources held by config structures (currently a no-op) */
void config_free(AppConfig *cfg);
void nodes_free(NodeConfig *ncfg);

#endif /* CONFIG_H */
