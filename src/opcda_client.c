/*
 * opcda_client.c – OPC-DA client implementation for OpenC-DA2UA
 *
 * Uses the snap7 C library to communicate with Siemens S7 PLCs.
 * snap7 is the same underlying library as the Python python-snap7 module
 * used in the reference Open-DA2UA project.
 *
 * Compile-time dependency:  libsnap7  (http://snap7.sourceforge.net/)
 * Link with:  -lsnap7
 */
#include "opcda_client.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_SNAP7
#  include <snap7.h>
#endif

/* ------------------------------------------------------------------
 * Internal data structure
 * ------------------------------------------------------------------ */
struct OpcDAClient {
#ifdef HAVE_SNAP7
    S7Object handle;
#else
    int      dummy; /* placeholder when snap7 is not available */
#endif
    int      connected;
};

/* ------------------------------------------------------------------
 * Helpers – extract typed values from a raw byte buffer
 * (mirrors snap7.util in the Python reference project)
 * Only compiled when snap7 is available since they are only used there.
 * ------------------------------------------------------------------ */

#ifdef HAVE_SNAP7
static int get_bool(const uint8_t *buf, int byte_offset, int bit_offset)
{
    return (buf[byte_offset] >> bit_offset) & 0x01;
}

static int16_t get_int(const uint8_t *buf, int byte_offset)
{
    return (int16_t)((buf[byte_offset] << 8) | buf[byte_offset + 1]);
}

static int32_t get_dint(const uint8_t *buf, int byte_offset)
{
    return (int32_t)(((uint32_t)buf[byte_offset]     << 24) |
                     ((uint32_t)buf[byte_offset + 1] << 16) |
                     ((uint32_t)buf[byte_offset + 2] <<  8) |
                      (uint32_t)buf[byte_offset + 3]);
}

static float get_real(const uint8_t *buf, int byte_offset)
{
    uint32_t bits = ((uint32_t)buf[byte_offset]     << 24) |
                    ((uint32_t)buf[byte_offset + 1] << 16) |
                    ((uint32_t)buf[byte_offset + 2] <<  8) |
                     (uint32_t)buf[byte_offset + 3];
    float val;
    memcpy(&val, &bits, sizeof(val));
    return val;
}

static void get_string(const uint8_t *buf, int byte_offset, int max_len,
                       char *out, size_t out_size)
{
    /* S7 STRING layout: [max_len(1)] [actual_len(1)] [chars...] */
    int actual = (int)(uint8_t)buf[byte_offset + 1];
    if (actual > max_len - 2) actual = max_len - 2;
    if ((size_t)actual >= out_size) actual = (int)out_size - 1;
    memcpy(out, buf + byte_offset + 2, (size_t)actual);
    out[actual] = '\0';
}
#endif /* HAVE_SNAP7 */

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

OpcDAClient *opcda_client_create(void)
{
    OpcDAClient *c = (OpcDAClient *)calloc(1, sizeof(*c));
    if (!c) return NULL;

#ifdef HAVE_SNAP7
    c->handle = Cli_Create();
    if (!c->handle) {
        free(c);
        return NULL;
    }
#endif
    return c;
}

int opcda_client_connect(OpcDAClient *client, const char *ip)
{
    if (!client || !ip) return -1;

#ifdef HAVE_SNAP7
    int ret = Cli_ConnectTo(client->handle, ip, 0, 1); /* rack=0, slot=1 */
    if (ret != 0) {
        LOG_ERROR_MSG("opcda: connect to %s failed (snap7 error %d)", ip, ret);
        return ret;
    }
    client->connected = 1;
    LOG_INFO_MSG("opcda: connected to PLC at %s", ip);
    return 0;
#else
    (void)ip;
    LOG_WARN_MSG("opcda: snap7 not compiled in – connection skipped");
    client->connected = 0;
    return -1;
#endif
}

int opcda_client_read(OpcDAClient *client, int db_number,
                      const NodeConfig *ncfg, TagValueSet *tvs)
{
    if (!client || !ncfg || !tvs) return -1;
    tvs->count = 0;

#ifdef HAVE_SNAP7
    if (!client->connected) return -1;

    /* Determine the minimum read length: highest (byte_offset + type_size) */
    int max_end = 0;
    for (int i = 0; i < ncfg->count; i++) {
        const NodeDef *nd = &ncfg->nodes[i];
        int end = nd->byte_offset;
        switch (nd->type) {
            case TAG_TYPE_BOOL:   end += 1; break;
            case TAG_TYPE_INT:    end += 2; break;
            case TAG_TYPE_REAL:   end += 4; break;
            case TAG_TYPE_DINT:   end += 4; break;
            case TAG_TYPE_STRING: end += nd->str_len; break;
        }
        if (end > max_end) max_end = end;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)max_end);
    if (!buf) return -1;

    int ret = Cli_DBRead(client->handle, db_number, 0, max_end, buf);
    if (ret != 0) {
        LOG_ERROR_MSG("opcda: DBRead DB%d failed (snap7 error %d)", db_number, ret);
        free(buf);
        return ret;
    }

    for (int i = 0; i < ncfg->count && tvs->count < MAX_NODES; i++) {
        const NodeDef *nd = &ncfg->nodes[i];
        TagValue *tv = &tvs->items[tvs->count];

        strncpy(tv->name, nd->name, sizeof(tv->name) - 1);
        tv->name[sizeof(tv->name) - 1] = '\0';
        tv->type = nd->type;

        switch (nd->type) {
            case TAG_TYPE_BOOL:
                tv->value.bool_val = get_bool(buf, nd->byte_offset, nd->bit_offset);
                break;
            case TAG_TYPE_INT:
                tv->value.int_val = get_int(buf, nd->byte_offset);
                break;
            case TAG_TYPE_DINT:
                tv->value.dint_val = get_dint(buf, nd->byte_offset);
                break;
            case TAG_TYPE_REAL:
                tv->value.real_val = get_real(buf, nd->byte_offset);
                break;
            case TAG_TYPE_STRING:
                get_string(buf, nd->byte_offset, nd->str_len,
                           tv->value.str_val, sizeof(tv->value.str_val));
                break;
        }
        tvs->count++;
    }

    free(buf);
    return 0;
#else
    (void)db_number;
    LOG_WARN_MSG("opcda: snap7 not compiled in – read skipped");
    return -1;
#endif
}

void opcda_client_disconnect(OpcDAClient *client)
{
    if (!client) return;
#ifdef HAVE_SNAP7
    if (client->connected) {
        Cli_Disconnect(client->handle);
        client->connected = 0;
        LOG_INFO_MSG("opcda: disconnected from PLC");
    }
#endif
}

void opcda_client_destroy(OpcDAClient *client)
{
    if (!client) return;
    opcda_client_disconnect(client);
#ifdef HAVE_SNAP7
    Cli_Destroy(&client->handle);
#endif
    free(client);
}

int opcda_client_is_connected(const OpcDAClient *client)
{
    return client ? client->connected : 0;
}
