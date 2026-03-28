#ifndef OPCDA_CLIENT_H
#define OPCDA_CLIENT_H

#include "config.h"
#include <stdint.h>

/* Opaque handle for an S7 PLC connection */
typedef struct OpcDAClient OpcDAClient;

/* Tag value union returned from a DB read */
typedef struct {
    char     name[MAX_STR_LEN];
    TagType  type;
    union {
        int      bool_val;   /* TAG_TYPE_BOOL  */
        int16_t  int_val;    /* TAG_TYPE_INT   */
        int32_t  dint_val;   /* TAG_TYPE_DINT  */
        float    real_val;   /* TAG_TYPE_REAL  */
        char     str_val[256]; /* TAG_TYPE_STRING */
    } value;
} TagValue;

/* Collection of tag values read in a single cycle */
typedef struct {
    TagValue items[MAX_NODES];
    int      count;
} TagValueSet;

/* Create a new client handle */
OpcDAClient *opcda_client_create(void);

/* Connect to a Siemens S7 PLC.
 * Returns 0 on success, non-zero on failure. */
int opcda_client_connect(OpcDAClient *client, const char *ip);

/* Read one data block and populate tvs.
 * Returns 0 on success, non-zero on failure. */
int opcda_client_read(OpcDAClient *client, int db_number,
                      const NodeConfig *ncfg, TagValueSet *tvs);

/* Disconnect from the PLC */
void opcda_client_disconnect(OpcDAClient *client);

/* Destroy the client handle */
void opcda_client_destroy(OpcDAClient *client);

/* Return non-zero if currently connected */
int opcda_client_is_connected(const OpcDAClient *client);

#endif /* OPCDA_CLIENT_H */
