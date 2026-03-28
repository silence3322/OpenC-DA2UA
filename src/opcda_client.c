/*
 * opcda_client.c – OPC-DA client implementation for OpenC-DA2UA
 *
 * Modes:
 * 1) Native OPC DA COM (Windows, IOPC* interfaces)
 * 2) snap7 (legacy S7 DB mode)
 * 3) stub simulation (when snap7 is unavailable)
 */
#include "opcda_client.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  include <objbase.h>
#  include <oleauto.h>
#endif

#ifdef HAVE_SNAP7
#  include <snap7.h>
#endif

#ifdef _WIN32
typedef DWORD OPCHANDLE;

#ifndef CLSCTX_ACTIVATE_32_BIT_SERVER
#define CLSCTX_ACTIVATE_32_BIT_SERVER 0x00040000
#endif

typedef enum {
    OPC_DS_CACHE  = 1,
    OPC_DS_DEVICE = 2
} OPCDATASOURCE;

typedef struct tagOPCITEMDEF {
    LPWSTR    szAccessPath;
    LPWSTR    szItemID;
    BOOL      bActive;
    OPCHANDLE hClient;
    DWORD     dwBlobSize;
    BYTE      *pBlob;
    VARTYPE   vtRequestedDataType;
    WORD      wReserved;
} OPCITEMDEF;

typedef struct tagOPCITEMRESULT {
    OPCHANDLE hServer;
    VARTYPE   vtCanonicalDataType;
    WORD      wReserved;
    DWORD     dwAccessRights;
    DWORD     dwBlobSize;
    BYTE      *pBlob;
} OPCITEMRESULT;

typedef struct tagOPCITEMSTATE {
    OPCHANDLE hClient;
    FILETIME  ftTimeStamp;
    WORD      wQuality;
    WORD      wReserved;
    VARIANT   vDataValue;
} OPCITEMSTATE;

typedef struct IOPCServer IOPCServer;
typedef struct IOPCItemMgt IOPCItemMgt;
typedef struct IOPCSyncIO IOPCSyncIO;

typedef struct IOPCServerVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IOPCServer *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IOPCServer *);
    ULONG (STDMETHODCALLTYPE *Release)(IOPCServer *);
    HRESULT (STDMETHODCALLTYPE *AddGroup)(
        IOPCServer *, LPCWSTR, BOOL, DWORD, OPCHANDLE, LONG *, FLOAT *, DWORD,
        OPCHANDLE *, DWORD *, REFIID, LPUNKNOWN *);
    HRESULT (STDMETHODCALLTYPE *GetErrorString)(IOPCServer *, HRESULT, LPWSTR *);
    HRESULT (STDMETHODCALLTYPE *GetGroupByName)(IOPCServer *, LPCWSTR, REFIID, LPUNKNOWN *);
    HRESULT (STDMETHODCALLTYPE *GetStatus)(IOPCServer *, void **);
    HRESULT (STDMETHODCALLTYPE *RemoveGroup)(IOPCServer *, OPCHANDLE, BOOL);
    HRESULT (STDMETHODCALLTYPE *CreateGroupEnumerator)(IOPCServer *, DWORD, REFIID, LPUNKNOWN *);
} IOPCServerVtbl;

struct IOPCServer {
    const IOPCServerVtbl *lpVtbl;
};

typedef struct IOPCItemMgtVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IOPCItemMgt *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IOPCItemMgt *);
    ULONG (STDMETHODCALLTYPE *Release)(IOPCItemMgt *);
    HRESULT (STDMETHODCALLTYPE *AddItems)(
        IOPCItemMgt *, DWORD, OPCITEMDEF *, OPCITEMRESULT **, HRESULT **);
    HRESULT (STDMETHODCALLTYPE *ValidateItems)(IOPCItemMgt *, DWORD, OPCITEMDEF *, BOOL, OPCITEMRESULT **, HRESULT **);
    HRESULT (STDMETHODCALLTYPE *RemoveItems)(IOPCItemMgt *, DWORD, OPCHANDLE *, HRESULT **);
    HRESULT (STDMETHODCALLTYPE *SetActiveState)(IOPCItemMgt *, DWORD, OPCHANDLE *, BOOL, HRESULT **);
    HRESULT (STDMETHODCALLTYPE *SetClientHandles)(IOPCItemMgt *, DWORD, OPCHANDLE *, OPCHANDLE *, HRESULT **);
    HRESULT (STDMETHODCALLTYPE *SetDatatypes)(IOPCItemMgt *, DWORD, OPCHANDLE *, VARTYPE *, HRESULT **);
    HRESULT (STDMETHODCALLTYPE *CreateEnumerator)(IOPCItemMgt *, REFIID, LPUNKNOWN *);
} IOPCItemMgtVtbl;

struct IOPCItemMgt {
    const IOPCItemMgtVtbl *lpVtbl;
};

typedef struct IOPCSyncIOVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IOPCSyncIO *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IOPCSyncIO *);
    ULONG (STDMETHODCALLTYPE *Release)(IOPCSyncIO *);
    HRESULT (STDMETHODCALLTYPE *Read)(IOPCSyncIO *, OPCDATASOURCE, DWORD, OPCHANDLE *, OPCITEMSTATE **, HRESULT **);
    HRESULT (STDMETHODCALLTYPE *Write)(IOPCSyncIO *, DWORD, OPCHANDLE *, VARIANT *, HRESULT **);
} IOPCSyncIOVtbl;

struct IOPCSyncIO {
    const IOPCSyncIOVtbl *lpVtbl;
};

static const IID IID_IOPCServer =
{0x39c13a4d, 0x011e, 0x11d0, {0x96, 0x75, 0x00, 0x20, 0xaf, 0xd8, 0xad, 0xb3}};
static const IID IID_IOPCItemMgt =
{0x39c13a54, 0x011e, 0x11d0, {0x96, 0x75, 0x00, 0x20, 0xaf, 0xd8, 0xad, 0xb3}};
static const IID IID_IOPCSyncIO =
{0x39c13a52, 0x011e, 0x11d0, {0x96, 0x75, 0x00, 0x20, 0xaf, 0xd8, 0xad, 0xb3}};
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
#ifdef _WIN32
    int        com_initialized;
    int        using_opcda_com;
    IOPCServer *opc_server;
    IOPCItemMgt *item_mgt;
    IOPCSyncIO *sync_io;
    IUnknown   *group_unknown;
    OPCHANDLE   group_handle;
    OPCHANDLE   server_handles[MAX_NODES];
    int         item_bound[MAX_NODES];
#endif
    int      warned_stub_mode;
    unsigned long cycle;
    int      connected;
    unsigned long last_error_code;
    char     last_error_text[256];
};

#ifdef _WIN32
static int str_starts_with(const char *s, const char *prefix)
{
    if (!s || !prefix) return 0;
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

static void opcda_set_error(OpcDAClient *client, HRESULT hr, const char *text)
{
    if (!client) return;
    client->last_error_code = (unsigned long)hr;
    snprintf(client->last_error_text, sizeof(client->last_error_text), "%s", text ? text : "");
}

static int utf8_to_wide(const char *src, wchar_t *dst, size_t dst_count)
{
    if (!src || !dst || dst_count == 0) return 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)dst_count);
    if (n <= 0) {
        n = MultiByteToWideChar(CP_ACP, 0, src, -1, dst, (int)dst_count);
    }
    if (n <= 0) {
        dst[0] = L'\0';
        return 0;
    }
    return 1;
}

static int wide_to_utf8(const wchar_t *src, char *dst, size_t dst_count);

static void clsid_to_string_a(REFCLSID clsid, char *dst, size_t dst_count)
{
    LPOLESTR w = NULL;
    if (!dst || dst_count == 0) return;
    dst[0] = '\0';
    if (StringFromCLSID(clsid, &w) != S_OK || !w) return;
    wide_to_utf8(w, dst, dst_count);
    CoTaskMemFree(w);
}

static int clsid_exists_in_registry_view(REFCLSID clsid, REGSAM wow_flag)
{
    char clsid_text[64];
    char key[128];
    HKEY h = NULL;
    clsid_to_string_a(clsid, clsid_text, sizeof(clsid_text));
    if (!clsid_text[0]) return 0;
    snprintf(key, sizeof(key), "CLSID\\%s", clsid_text);
    if (RegOpenKeyExA(HKEY_CLASSES_ROOT, key, 0, KEY_READ | wow_flag, &h) == ERROR_SUCCESS) {
        RegCloseKey(h);
        return 1;
    }
    return 0;
}

static int wide_to_utf8(const wchar_t *src, char *dst, size_t dst_count)
{
    if (!src || !dst || dst_count == 0) return 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, (int)dst_count,
                                NULL, NULL);
    if (n <= 0) {
        dst[0] = '\0';
        return 0;
    }
    return 1;
}

static void com_release(void **pp)
{
    if (pp && *pp) {
        IUnknown *u = (IUnknown *)(*pp);
        u->lpVtbl->Release(u);
        *pp = NULL;
    }
}

static int is_local_host(const char *host)
{
    if (!host || !*host) return 1;
    return _stricmp(host, "localhost") == 0 || _stricmp(host, ".") == 0 ||
           _stricmp(host, "127.0.0.1") == 0;
}

static VARTYPE requested_vt_from_tag(TagType t)
{
    switch (t) {
        case TAG_TYPE_BOOL:   return VT_BOOL;
        case TAG_TYPE_INT:    return VT_I2;
        case TAG_TYPE_REAL:   return VT_R4;
        case TAG_TYPE_DINT:   return VT_I4;
        case TAG_TYPE_STRING: return VT_BSTR;
        default: return VT_EMPTY;
    }
}

static int variant_to_tag_value(const VARIANT *src, TagType type, TagValue *out)
{
    if (!src || !out) return 0;

    VARIANT v;
    VariantInit(&v);
    if (FAILED(VariantCopy(&v, (VARIANT *)src))) return 0;

    switch (type) {
        case TAG_TYPE_BOOL:
            if (SUCCEEDED(VariantChangeType(&v, &v, 0, VT_BOOL))) {
                out->value.bool_val = (v.boolVal == VARIANT_TRUE) ? 1 : 0;
                VariantClear(&v);
                return 1;
            }
            break;
        case TAG_TYPE_INT:
            if (SUCCEEDED(VariantChangeType(&v, &v, 0, VT_I2))) {
                out->value.int_val = v.iVal;
                VariantClear(&v);
                return 1;
            }
            break;
        case TAG_TYPE_DINT:
            if (SUCCEEDED(VariantChangeType(&v, &v, 0, VT_I4))) {
                out->value.dint_val = v.lVal;
                VariantClear(&v);
                return 1;
            }
            break;
        case TAG_TYPE_REAL:
            if (SUCCEEDED(VariantChangeType(&v, &v, 0, VT_R4))) {
                out->value.real_val = v.fltVal;
                VariantClear(&v);
                return 1;
            }
            break;
        case TAG_TYPE_STRING:
            if (SUCCEEDED(VariantChangeType(&v, &v, 0, VT_BSTR)) && v.bstrVal) {
                if (!wide_to_utf8(v.bstrVal, out->value.str_val,
                                  sizeof(out->value.str_val))) {
                    out->value.str_val[0] = '\0';
                }
                VariantClear(&v);
                return 1;
            }
            break;
    }

    VariantClear(&v);
    return 0;
}

static void tag_value_to_variant(const TagValue *tv, VARIANT *out)
{
    VariantInit(out);
    if (!tv) return;

    switch (tv->type) {
        case TAG_TYPE_BOOL:
            out->vt = VT_BOOL;
            out->boolVal = tv->value.bool_val ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        case TAG_TYPE_INT:
            out->vt = VT_I2;
            out->iVal = tv->value.int_val;
            break;
        case TAG_TYPE_DINT:
            out->vt = VT_I4;
            out->lVal = tv->value.dint_val;
            break;
        case TAG_TYPE_REAL:
            out->vt = VT_R4;
            out->fltVal = tv->value.real_val;
            break;
        case TAG_TYPE_STRING: {
            wchar_t wbuf[256];
            if (utf8_to_wide(tv->value.str_val, wbuf, 256)) {
                out->vt = VT_BSTR;
                out->bstrVal = SysAllocString(wbuf);
            }
            break;
        }
    }
}

static void opcda_com_cleanup(OpcDAClient *client)
{
    if (!client) return;

    if (client->opc_server && client->group_handle != 0U) {
        client->opc_server->lpVtbl->RemoveGroup(client->opc_server, client->group_handle, FALSE);
        client->group_handle = 0;
    }

    for (int i = 0; i < MAX_NODES; i++) {
        client->server_handles[i] = 0;
        client->item_bound[i] = 0;
    }

    com_release((void **)&client->sync_io);
    com_release((void **)&client->item_mgt);
    com_release((void **)&client->group_unknown);
    com_release((void **)&client->opc_server);

    if (client->com_initialized) {
        CoUninitialize();
        client->com_initialized = 0;
    }

    client->using_opcda_com = 0;
    client->connected = 0;
}

static int opcda_com_connect(OpcDAClient *client, const char *target)
{
    if (!client || !target) return -1;

    const char *progid = target;
    const char *host = "";
    char progid_buf[MAX_STR_LEN];
    char host_buf[MAX_STR_LEN];
    const char *at = strchr(target, '@');

    if (at) {
        size_t p_len = (size_t)(at - target);
        if (p_len >= sizeof(progid_buf)) p_len = sizeof(progid_buf) - 1;
        memcpy(progid_buf, target, p_len);
        progid_buf[p_len] = '\0';
        snprintf(host_buf, sizeof(host_buf), "%s", at + 1);
        progid = progid_buf;
        host = host_buf;
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        client->com_initialized = 1;
    } else if (hr != RPC_E_CHANGED_MODE) {
        opcda_set_error(client, hr, "CoInitializeEx failed");
        LOG_ERROR_MSG("opcda(com): CoInitializeEx failed (0x%08X)", (unsigned int)hr);
        return -1;
    }

    CLSID clsid;
    wchar_t w_progid[MAX_STR_LEN];
    if (!utf8_to_wide(progid, w_progid, MAX_STR_LEN)) {
        opcda_set_error(client, E_INVALIDARG, "invalid progid encoding");
        LOG_ERROR_MSG("opcda(com): invalid progid encoding: %s", progid);
        opcda_com_cleanup(client);
        return -1;
    }

    hr = CLSIDFromProgID(w_progid, &clsid);
    if (FAILED(hr)) {
        opcda_set_error(client, hr, "ProgID not registered");
        LOG_ERROR_MSG("opcda(com): ProgID not registered: %s", progid);
        opcda_com_cleanup(client);
        return -1;
    }

    {
        DWORD clsctx = CLSCTX_SERVER;
        int has_64 = clsid_exists_in_registry_view(&clsid, KEY_WOW64_64KEY);
        int has_32 = clsid_exists_in_registry_view(&clsid, KEY_WOW64_32KEY);
        if (!has_64 && has_32) {
            clsctx |= CLSCTX_ACTIVATE_32_BIT_SERVER;
            LOG_WARN_MSG("opcda(com): using 32-bit COM activation for progid=%s", progid);
        }

        if (is_local_host(host)) {
            hr = CoCreateInstance(&clsid, NULL, clsctx,
                                  &IID_IOPCServer, (void **)&client->opc_server);
        } else {
            wchar_t w_host[MAX_STR_LEN];
            COSERVERINFO si;
            MULTI_QI qi;

            if (!utf8_to_wide(host, w_host, MAX_STR_LEN)) {
                opcda_set_error(client, E_INVALIDARG, "invalid host encoding");
                LOG_ERROR_MSG("opcda(com): invalid host encoding: %s", host);
                opcda_com_cleanup(client);
                return -1;
            }

            memset(&si, 0, sizeof(si));
            memset(&qi, 0, sizeof(qi));
            si.pwszName = w_host;
            qi.pIID = &IID_IOPCServer;

            hr = CoCreateInstanceEx(&clsid, NULL, clsctx, &si, 1, &qi);
            if (SUCCEEDED(hr)) hr = qi.hr;
            if (SUCCEEDED(hr) && qi.pItf) {
                client->opc_server = (IOPCServer *)qi.pItf;
            }
        }
    }

    if (FAILED(hr) || !client->opc_server) {
        opcda_set_error(client, hr, "CoCreateInstance failed");
        LOG_ERROR_MSG("opcda(com): CoCreateInstance failed progid=%s host=%s hr=0x%08X",
                      progid, host[0] ? host : "localhost", (unsigned int)hr);
        opcda_com_cleanup(client);
        return -1;
    }

    DWORD revised_update_rate = 0;

    hr = client->opc_server->lpVtbl->AddGroup(
        client->opc_server,
        L"OpenC_DA2UA",
        TRUE,
        100,
        1,
        NULL,
        NULL,
        LOCALE_SYSTEM_DEFAULT,
        &client->group_handle,
        &revised_update_rate,
        &IID_IUnknown,
        &client->group_unknown);

    if (FAILED(hr) || !client->group_unknown) {
        opcda_set_error(client, hr, "AddGroup failed");
        LOG_ERROR_MSG("opcda(com): AddGroup failed hr=0x%08X", (unsigned int)hr);
        opcda_com_cleanup(client);
        return -1;
    }

    hr = client->group_unknown->lpVtbl->QueryInterface(
        client->group_unknown, &IID_IOPCItemMgt, (void **)&client->item_mgt);
    if (FAILED(hr) || !client->item_mgt) {
        opcda_set_error(client, hr, "QueryInterface(IOPCItemMgt) failed");
        LOG_ERROR_MSG("opcda(com): QueryInterface(IOPCItemMgt) failed hr=0x%08X", (unsigned int)hr);
        opcda_com_cleanup(client);
        return -1;
    }

    hr = client->group_unknown->lpVtbl->QueryInterface(
        client->group_unknown, &IID_IOPCSyncIO, (void **)&client->sync_io);
    if (FAILED(hr) || !client->sync_io) {
        opcda_set_error(client, hr, "QueryInterface(IOPCSyncIO) failed");
        LOG_ERROR_MSG("opcda(com): QueryInterface(IOPCSyncIO) failed hr=0x%08X", (unsigned int)hr);
        opcda_com_cleanup(client);
        return -1;
    }

    client->using_opcda_com = 1;
    client->connected = 1;
    opcda_set_error(client, S_OK, "");
    LOG_WARN_MSG("opcda(com): connected via IOPC* progid=%s host=%s", progid, host[0] ? host : "localhost");
    return 0;
}

static int opcda_com_bind_item(OpcDAClient *client, int idx, const NodeDef *nd)
{
    if (!client || !nd || !client->item_mgt) return -1;
    if (idx < 0 || idx >= MAX_NODES) return -1;
    if (client->item_bound[idx]) return 0;

    wchar_t w_item[MAX_PATH_LEN];
    const char *item_id = nd->source[0] ? nd->source : nd->name;
    if (!utf8_to_wide(item_id, w_item, MAX_PATH_LEN)) {
        LOG_WARN_MSG("opcda(com): cannot convert item id '%s'", item_id);
        return -1;
    }

    OPCITEMDEF def;
    OPCITEMRESULT *results = NULL;
    HRESULT *errs = NULL;
    HRESULT hr;

    memset(&def, 0, sizeof(def));
    def.szAccessPath = L"";
    def.szItemID = w_item;
    def.bActive = TRUE;
    def.hClient = (OPCHANDLE)(idx + 1);
    def.vtRequestedDataType = requested_vt_from_tag(nd->type);

    hr = client->item_mgt->lpVtbl->AddItems(client->item_mgt, 1, &def, &results, &errs);
    if (FAILED(hr) || !results || !errs || FAILED(errs[0])) {
        HRESULT item_hr = (errs ? errs[0] : E_FAIL);
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "AddItems failed for '%s'", item_id);
        opcda_set_error(client, FAILED(item_hr) ? item_hr : hr, errbuf);
        LOG_WARN_MSG("opcda(com): AddItems failed for '%s' (hr=0x%08X, item=0x%08X)",
                     item_id, (unsigned int)hr, (unsigned int)item_hr);
        if (results) {
            if (results[0].pBlob) CoTaskMemFree(results[0].pBlob);
            CoTaskMemFree(results);
        }
        if (errs) CoTaskMemFree(errs);
        return -1;
    }

    client->server_handles[idx] = results[0].hServer;
    client->item_bound[idx] = 1;

    if (results[0].pBlob) CoTaskMemFree(results[0].pBlob);
    CoTaskMemFree(results);
    CoTaskMemFree(errs);
    return 0;
}
#endif

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

#ifdef _WIN32
    if (str_starts_with(ip, "opcda_com:")) {
        return opcda_com_connect(client, ip + (int)strlen("opcda_com:"));
    }
#endif

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
    client->connected = 1;
    if (!client->warned_stub_mode) {
        LOG_WARN_MSG("opcda: snap7 not compiled in – running DA source in placeholder mode (%s)", ip);
        client->warned_stub_mode = 1;
    }
    return 0;
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
    if (!client->connected) return -1;

#ifdef _WIN32
    if (client->using_opcda_com && client->sync_io && client->item_mgt) {
        int read_count = 0;

        for (int i = 0; i < ncfg->count && tvs->count < MAX_NODES; i++) {
            const NodeDef *nd = &ncfg->nodes[i];
            TagValue *tv = &tvs->items[tvs->count];
            const char *item_id = nd->source[0] ? nd->source : nd->name;

            strncpy(tv->name, nd->name, sizeof(tv->name) - 1);
            tv->name[sizeof(tv->name) - 1] = '\0';
            tv->type = nd->type;

            if (!client->item_bound[i]) {
                if (opcda_com_bind_item(client, i, nd) != 0) {
                    continue;
                }
            }

            OPCITEMSTATE *states = NULL;
            HRESULT *errs = NULL;
            OPCHANDLE handle = client->server_handles[i];
            HRESULT hr = client->sync_io->lpVtbl->Read(
                client->sync_io, OPC_DS_DEVICE, 1, &handle, &states, &errs);

            if (FAILED(hr) || !states || !errs || FAILED(errs[0])) {
                HRESULT item_hr = (errs ? errs[0] : E_FAIL);
                char errbuf[256];
                snprintf(errbuf, sizeof(errbuf), "Read failed for '%s'", item_id);
                opcda_set_error(client, FAILED(item_hr) ? item_hr : hr, errbuf);
                LOG_WARN_MSG("opcda(com): Read failed for '%s' (hr=0x%08X item=0x%08X)",
                             item_id, (unsigned int)hr, (unsigned int)item_hr);
                if (states) {
                    VariantClear(&states[0].vDataValue);
                    CoTaskMemFree(states);
                }
                if (errs) CoTaskMemFree(errs);
                continue;
            }

            if (!variant_to_tag_value(&states[0].vDataValue, nd->type, tv)) {
                LOG_WARN_MSG("opcda(com): type conversion failed for '%s'", item_id);
                VariantClear(&states[0].vDataValue);
                CoTaskMemFree(states);
                CoTaskMemFree(errs);
                continue;
            }
            VariantClear(&states[0].vDataValue);
            CoTaskMemFree(states);
            CoTaskMemFree(errs);
            tvs->count++;
            read_count++;
        }

        if (read_count == 0 && ncfg->count > 0) return -1;
        if (read_count > 0) opcda_set_error(client, S_OK, "");
        return 0;
    }
#endif

    for (int i = 0; i < ncfg->count && tvs->count < MAX_NODES; i++) {
        const NodeDef *nd = &ncfg->nodes[i];
        TagValue *tv = &tvs->items[tvs->count];

        strncpy(tv->name, nd->name, sizeof(tv->name) - 1);
        tv->name[sizeof(tv->name) - 1] = '\0';
        tv->type = nd->type;

        switch (nd->type) {
            case TAG_TYPE_BOOL:
                tv->value.bool_val = (int)((client->cycle + (unsigned long)i) % 2UL);
                break;
            case TAG_TYPE_INT:
                tv->value.int_val = (int16_t)(100 + (int)((client->cycle + (unsigned long)i) % 100UL));
                break;
            case TAG_TYPE_DINT:
                tv->value.dint_val = (int32_t)(1000 + (int)(client->cycle + (unsigned long)i));
                break;
            case TAG_TYPE_REAL:
                tv->value.real_val = (float)(client->cycle + (unsigned long)i) / 10.0f;
                break;
            case TAG_TYPE_STRING:
                snprintf(tv->value.str_val, sizeof(tv->value.str_val), "%s", nd->source[0] ? nd->source : nd->name);
                break;
        }

        tvs->count++;
    }

    client->cycle++;
    return 0;
#endif
}

int opcda_client_write(OpcDAClient *client, int tag_index,
                       const NodeConfig *ncfg, const TagValue *tv)
{
    if (!client || !ncfg || !tv) return -1;
    if (tag_index < 0 || tag_index >= ncfg->count || tag_index >= MAX_NODES) return -1;

#ifdef _WIN32
    if (client->using_opcda_com && client->sync_io && client->item_mgt) {
        const NodeDef *nd = &ncfg->nodes[tag_index];
        if (!client->item_bound[tag_index]) {
            if (opcda_com_bind_item(client, tag_index, nd) != 0) return -1;
        }

        OPCHANDLE handle = client->server_handles[tag_index];
        VARIANT v;
        HRESULT *errs = NULL;
        tag_value_to_variant(tv, &v);

        HRESULT hr = client->sync_io->lpVtbl->Write(client->sync_io, 1, &handle, &v, &errs);
        VariantClear(&v);

        if (FAILED(hr) || !errs || FAILED(errs[0])) {
            HRESULT item_hr = (errs ? errs[0] : E_FAIL);
            char errbuf[256];
            snprintf(errbuf, sizeof(errbuf), "Write failed for '%s'",
                     nd->source[0] ? nd->source : nd->name);
            opcda_set_error(client, FAILED(item_hr) ? item_hr : hr, errbuf);
            if (errs) CoTaskMemFree(errs);
            LOG_WARN_MSG("opcda(com): Write failed for '%s' (hr=0x%08X item=0x%08X)",
                         nd->source[0] ? nd->source : nd->name,
                         (unsigned int)hr, (unsigned int)item_hr);
            return -1;
        }

        CoTaskMemFree(errs);
        opcda_set_error(client, S_OK, "");
        return 0;
    }
#endif

    (void)tag_index;
    return 0;
}

void opcda_client_disconnect(OpcDAClient *client)
{
    if (!client) return;

#ifdef _WIN32
    if (client->using_opcda_com) {
        opcda_com_cleanup(client);
        LOG_INFO_MSG("opcda(com): disconnected");
        return;
    }
#endif

#ifdef HAVE_SNAP7
    if (client->connected) {
        Cli_Disconnect(client->handle);
        client->connected = 0;
        LOG_INFO_MSG("opcda: disconnected from PLC");
    }
#endif

    client->connected = 0;
}

void opcda_client_destroy(OpcDAClient *client)
{
    if (!client) return;
    opcda_client_disconnect(client);
#ifdef HAVE_SNAP7
    Cli_Destroy(&client->handle);
#endif
#ifdef _WIN32
    opcda_com_cleanup(client);
#endif
    free(client);
}

int opcda_client_is_connected(const OpcDAClient *client)
{
    return client ? client->connected : 0;
}

unsigned long opcda_client_last_error_code(const OpcDAClient *client)
{
    return client ? client->last_error_code : 0UL;
}

const char *opcda_client_last_error_text(const OpcDAClient *client)
{
    return client ? client->last_error_text : "";
}
