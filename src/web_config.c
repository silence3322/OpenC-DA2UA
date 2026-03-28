#include "web_config.h"

#include "config.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>

#pragma comment(lib, "ws2_32.lib")

static HANDLE g_thread = NULL;
static volatile LONG g_running = 0;
static SOCKET g_listen_sock = INVALID_SOCKET;
static char g_config_path[MAX_PATH_LEN];
static CRITICAL_SECTION g_status_lock;
static int g_status_lock_ready = 0;
static int g_gateway_enabled = 0;
static int g_gateway_connected = 0;
static int g_gateway_runtime_enabled = 1;
static char g_gateway_progid[MAX_STR_LEN];
static char g_gateway_message[256];
static DWORD g_gateway_updated_ms = 0;
static int g_opcua_enabled = 0;
static int g_opcua_listening = 0;
static int g_opcua_port = 0;
static char g_opcua_message[256];

#define OPCDA_CATID_10A "{63D5F430-CFE4-11d1-B2C8-0060083BA1FB}"
#define OPCDA_CATID_20  "{63D5F432-CFE4-11d1-B2C8-0060083BA1FB}"

typedef DWORD OPCHANDLE;

#ifndef CLSCTX_ACTIVATE_32_BIT_SERVER
#define CLSCTX_ACTIVATE_32_BIT_SERVER 0x00040000
#endif

typedef enum {
    OPC_NS_HIERARCHIAL = 1,
    OPC_NS_FLAT = 2
} OPCNAMESPACETYPE;

typedef enum {
    OPC_BROWSE_UP = 1,
    OPC_BROWSE_DOWN = 2,
    OPC_BROWSE_TO = 3
} OPCBROWSEDIRECTION;

typedef enum {
    OPC_BRANCH = 1,
    OPC_LEAF = 2,
    OPC_FLAT = 3
} OPCBROWSETYPE;

typedef struct IOPCServer IOPCServer;
typedef struct IOPCBrowseServerAddressSpace IOPCBrowseServerAddressSpace;

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

typedef struct IOPCBrowseServerAddressSpaceVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IOPCBrowseServerAddressSpace *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IOPCBrowseServerAddressSpace *);
    ULONG (STDMETHODCALLTYPE *Release)(IOPCBrowseServerAddressSpace *);
    HRESULT (STDMETHODCALLTYPE *QueryOrganization)(IOPCBrowseServerAddressSpace *, OPCNAMESPACETYPE *);
    HRESULT (STDMETHODCALLTYPE *ChangeBrowsePosition)(IOPCBrowseServerAddressSpace *, OPCBROWSEDIRECTION, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *BrowseOPCItemIDs)(IOPCBrowseServerAddressSpace *, OPCBROWSETYPE, LPCWSTR, VARTYPE, DWORD, IEnumString **);
    HRESULT (STDMETHODCALLTYPE *GetItemID)(IOPCBrowseServerAddressSpace *, LPCWSTR, LPWSTR *);
    HRESULT (STDMETHODCALLTYPE *BrowseAccessPaths)(IOPCBrowseServerAddressSpace *, LPCWSTR, IEnumString **);
} IOPCBrowseServerAddressSpaceVtbl;

struct IOPCBrowseServerAddressSpace {
    const IOPCBrowseServerAddressSpaceVtbl *lpVtbl;
};

static const IID IID_IOPCServer =
{0x39c13a4d, 0x011e, 0x11d0, {0x96, 0x75, 0x00, 0x20, 0xaf, 0xd8, 0xad, 0xb3}};
static const IID IID_IOPCBrowseServerAddressSpace =
{0x39c13a4f, 0x011e, 0x11d0, {0x96, 0x75, 0x00, 0x20, 0xaf, 0xd8, 0xad, 0xb3}};

static int read_default_value(HKEY key, char *out, DWORD out_sz);

static int json_append(char *dst, size_t cap, size_t *used, const char *src)
{
    size_t n = strlen(src);
    if (*used + n + 1 >= cap) return -1;
    memcpy(dst + *used, src, n);
    *used += n;
    dst[*used] = '\0';
    return 0;
}

static int json_append_escaped(char *dst, size_t cap, size_t *used, const char *src)
{
    const unsigned char *p = (const unsigned char *)src;
    while (*p) {
        if (*p == '\\' || *p == '"') {
            if (*used + 2 + 1 >= cap) return -1;
            dst[(*used)++] = '\\';
            dst[(*used)++] = (char)*p;
        } else if (*p == '\r' || *p == '\n' || *p == '\t') {
            if (*used + 2 + 1 >= cap) return -1;
            dst[(*used)++] = '\\';
            dst[(*used)++] = (*p == '\r') ? 'r' : ((*p == '\n') ? 'n' : 't');
        } else {
            if (*used + 1 + 1 >= cap) return -1;
            dst[(*used)++] = (char)*p;
        }
        ++p;
    }
    dst[*used] = '\0';
    return 0;
}

static int reg_key_exists(HKEY root, const char *subkey, REGSAM wow_flag)
{
    HKEY h = NULL;
    LONG rc = RegOpenKeyExA(root, subkey, 0, KEY_READ | wow_flag, &h);
    if (rc == ERROR_SUCCESS) {
        RegCloseKey(h);
        return 1;
    }
    return 0;
}

static int reg_read_default_value(HKEY root, const char *subkey, REGSAM wow_flag,
                                  char *out, DWORD out_sz)
{
    HKEY h = NULL;
    LONG rc = RegOpenKeyExA(root, subkey, 0, KEY_READ | wow_flag, &h);
    if (rc != ERROR_SUCCESS) return -1;
    rc = read_default_value(h, out, out_sz);
    RegCloseKey(h);
    return (rc == 0) ? 0 : -1;
}

static int utf8_to_wide(const char *src, wchar_t *dst, size_t dst_count)
{
    if (!src || !dst || dst_count == 0) return 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)dst_count);
    if (n <= 0) n = MultiByteToWideChar(CP_ACP, 0, src, -1, dst, (int)dst_count);
    if (n <= 0) {
        dst[0] = L'\0';
        return 0;
    }
    return 1;
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

static int clsid_exists_in_registry_view(REFCLSID clsid, REGSAM wow_flag)
{
    LPOLESTR w = NULL;
    char clsid_text[64];
    char key[128];
    HKEY h = NULL;
    if (StringFromCLSID(clsid, &w) != S_OK || !w) return 0;
    if (!wide_to_utf8(w, clsid_text, sizeof(clsid_text))) {
        CoTaskMemFree(w);
        return 0;
    }
    CoTaskMemFree(w);
    snprintf(key, sizeof(key), "CLSID\\%s", clsid_text);
    if (RegOpenKeyExA(HKEY_CLASSES_ROOT, key, 0, KEY_READ | wow_flag, &h) == ERROR_SUCCESS) {
        RegCloseKey(h);
        return 1;
    }
    return 0;
}

static int append_json_item_string(char *out, size_t out_sz, size_t *used,
                                   const char *text, int *count)
{
    if (*count > 0 && json_append(out, out_sz, used, ",") != 0) return -1;
    if (json_append(out, out_sz, used, "\"") != 0) return -1;
    if (json_append_escaped(out, out_sz, used, text ? text : "") != 0) return -1;
    if (json_append(out, out_sz, used, "\"") != 0) return -1;
    (*count)++;
    return 0;
}

typedef struct {
    char *out;
    size_t out_sz;
    size_t *used;
    int *count;
    int max_items;
} BrowseJsonCtx;

static int append_item_from_leaf(IOPCBrowseServerAddressSpace *browse, const wchar_t *leaf, BrowseJsonCtx *ctx)
{
    LPWSTR w_itemid = NULL;
    char itemid[MAX_PATH_LEN];
    HRESULT hr = browse->lpVtbl->GetItemID(browse, leaf, &w_itemid);
    if (FAILED(hr) || !w_itemid) {
        if (!wide_to_utf8(leaf, itemid, sizeof(itemid))) return 0;
    } else {
        if (!wide_to_utf8(w_itemid, itemid, sizeof(itemid))) {
            CoTaskMemFree(w_itemid);
            return 0;
        }
        CoTaskMemFree(w_itemid);
    }
    if (*(ctx->count) >= ctx->max_items) return 1;
    return append_json_item_string(ctx->out, ctx->out_sz, ctx->used, itemid, ctx->count);
}

static IEnumString *browse_itemids_try_filters(IOPCBrowseServerAddressSpace *browse, OPCBROWSETYPE bt)
{
    HRESULT hr;
    IEnumString *e = NULL;
    static const wchar_t *filters[] = { NULL, L"", L"*" };
    for (int i = 0; i < 3; ++i) {
        e = NULL;
        hr = browse->lpVtbl->BrowseOPCItemIDs(browse, bt, filters[i], VT_EMPTY, 0, &e);
        if (SUCCEEDED(hr) && e) return e;
    }
    return NULL;
}

static int browse_collect_recursive(IOPCBrowseServerAddressSpace *browse, BrowseJsonCtx *ctx, int depth)
{
    if (!browse || !ctx || depth > 12 || *(ctx->count) >= ctx->max_items) return 0;

    IEnumString *leaf_enum = browse_itemids_try_filters(browse, OPC_LEAF);
    if (leaf_enum) {
        for (;;) {
            ULONG fetched = 0;
            LPOLESTR name = NULL;
            HRESULT hr = leaf_enum->lpVtbl->Next(leaf_enum, 1, &name, &fetched);
            if (hr != S_OK || fetched == 0 || !name) break;
            if (append_item_from_leaf(browse, name, ctx) < 0) {
                CoTaskMemFree(name);
                leaf_enum->lpVtbl->Release(leaf_enum);
                return -1;
            }
            CoTaskMemFree(name);
            if (*(ctx->count) >= ctx->max_items) break;
        }
        leaf_enum->lpVtbl->Release(leaf_enum);
    }

    if (*(ctx->count) >= ctx->max_items) return 0;

    IEnumString *branch_enum = browse_itemids_try_filters(browse, OPC_BRANCH);
    if (!branch_enum) return 0;

    for (;;) {
        ULONG fetched = 0;
        LPOLESTR branch = NULL;
        HRESULT hr = branch_enum->lpVtbl->Next(branch_enum, 1, &branch, &fetched);
        if (hr != S_OK || fetched == 0 || !branch) break;

        HRESULT down = browse->lpVtbl->ChangeBrowsePosition(browse, OPC_BROWSE_DOWN, branch);
        if (SUCCEEDED(down)) {
            if (browse_collect_recursive(browse, ctx, depth + 1) < 0) {
                CoTaskMemFree(branch);
                branch_enum->lpVtbl->Release(branch_enum);
                return -1;
            }
            browse->lpVtbl->ChangeBrowsePosition(browse, OPC_BROWSE_UP, NULL);
        }

        CoTaskMemFree(branch);
        if (*(ctx->count) >= ctx->max_items) break;
    }

    branch_enum->lpVtbl->Release(branch_enum);
    return 0;
}

static int parse_query_param(const char *req, const char *key, char *out, size_t out_sz)
{
    const char *sp1;
    const char *sp2;
    const char *q;
    size_t klen;
    if (!req || !key || !out || out_sz == 0) return -1;
    out[0] = '\0';

    sp1 = strchr(req, ' ');
    if (!sp1) return -1;
    sp1++;
    sp2 = strchr(sp1, ' ');
    if (!sp2 || sp2 <= sp1) return -1;
    q = memchr(sp1, '?', (size_t)(sp2 - sp1));
    if (!q) return -1;
    q++;
    klen = strlen(key);

    while (q < sp2) {
        const char *amp = memchr(q, '&', (size_t)(sp2 - q));
        const char *end = amp ? amp : sp2;
        const char *eq = memchr(q, '=', (size_t)(end - q));
        if (eq && (size_t)(eq - q) == klen && _strnicmp(q, key, klen) == 0) {
            size_t n = (size_t)(end - eq - 1);
            if (n >= out_sz) n = out_sz - 1;
            memcpy(out, eq + 1, n);
            out[n] = '\0';
            for (size_t i = 0; i < n; ++i) {
                if (out[i] == '+') out[i] = ' ';
            }
            return 0;
        }
        if (!amp) break;
        q = amp + 1;
    }
    return -1;
}

static int build_opcda_browse_json(const char *progid_in, char *out, size_t out_sz)
{
    char progid[MAX_STR_LEN];
    wchar_t w_progid[MAX_STR_LEN];
    CLSID clsid;
    HRESULT hr;
    IOPCServer *server = NULL;
    IOPCBrowseServerAddressSpace *browse = NULL;
    int com_inited = 0;
    size_t used = 0;
    int count = 0;
    int max_items = 2000;

    if (!out || out_sz < 128) return -1;
    snprintf(progid, sizeof(progid), "%s", (progid_in && progid_in[0]) ? progid_in : "");
    if (!progid[0]) {
        out[0] = '\0';
        return json_append(out, out_sz, &used,
                           "{\"ok\":false,\"error\":\"missing_progid\"}");
    }

    if (!utf8_to_wide(progid, w_progid, MAX_STR_LEN)) {
        out[0] = '\0';
        return json_append(out, out_sz, &used,
                           "{\"ok\":false,\"error\":\"invalid_progid_encoding\"}");
    }

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) com_inited = 1;
    else if (hr == RPC_E_CHANGED_MODE) {
        hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr)) com_inited = 1;
    }
    if (!com_inited) {
        out[0] = '\0';
        return json_append(out, out_sz, &used,
                           "{\"ok\":false,\"error\":\"com_init_failed\"}");
    }

    hr = CLSIDFromProgID(w_progid, &clsid);
    if (FAILED(hr)) {
        CoUninitialize();
        out[0] = '\0';
        return json_append(out, out_sz, &used,
                           "{\"ok\":false,\"error\":\"progid_not_registered\"}");
    }

    {
        DWORD clsctx = CLSCTX_LOCAL_SERVER;
        int has64 = clsid_exists_in_registry_view(&clsid, KEY_WOW64_64KEY);
        int has32 = clsid_exists_in_registry_view(&clsid, KEY_WOW64_32KEY);
        if (!has64 && has32) clsctx |= CLSCTX_ACTIVATE_32_BIT_SERVER;
        hr = CoCreateInstance(&clsid, NULL, clsctx, &IID_IOPCServer, (void **)&server);
    }
    if (FAILED(hr) || !server) {
        CoUninitialize();
        out[0] = '\0';
        return json_append(out, out_sz, &used,
                           "{\"ok\":false,\"error\":\"create_instance_failed\"}");
    }

    hr = server->lpVtbl->QueryInterface(server, &IID_IOPCBrowseServerAddressSpace, (void **)&browse);
    if (FAILED(hr) || !browse) {
        server->lpVtbl->Release(server);
        CoUninitialize();
        out[0] = '\0';
        return json_append(out, out_sz, &used,
                           "{\"ok\":false,\"error\":\"browse_interface_not_supported\"}");
    }

    out[0] = '\0';
    if (json_append(out, out_sz, &used, "{\"ok\":true,\"progid\":\"") != 0 ||
        json_append_escaped(out, out_sz, &used, progid) != 0 ||
        json_append(out, out_sz, &used, "\",\"items\":[") != 0) {
        browse->lpVtbl->Release(browse);
        server->lpVtbl->Release(server);
        CoUninitialize();
        return -1;
    }

    {
        OPCNAMESPACETYPE org = OPC_NS_HIERARCHIAL;
        IEnumString *flat_enum = NULL;
        hr = browse->lpVtbl->QueryOrganization(browse, &org);
        browse->lpVtbl->ChangeBrowsePosition(browse, OPC_BROWSE_TO, L"");

        /* Prefer flat browse first (works on some DA servers even when org query is unreliable). */
        flat_enum = browse_itemids_try_filters(browse, OPC_FLAT);
        if (!flat_enum) flat_enum = browse_itemids_try_filters(browse, OPC_LEAF);
        if (flat_enum) {
            for (;;) {
                ULONG fetched = 0;
                LPOLESTR name = NULL;
                HRESULT nhr = flat_enum->lpVtbl->Next(flat_enum, 1, &name, &fetched);
                if (nhr != S_OK || fetched == 0 || !name) break;
                char itemid[MAX_PATH_LEN];
                if (wide_to_utf8(name, itemid, sizeof(itemid))) {
                    if (count >= max_items || append_json_item_string(out, out_sz, &used, itemid, &count) != 0) {
                        CoTaskMemFree(name);
                        flat_enum->lpVtbl->Release(flat_enum);
                        browse->lpVtbl->Release(browse);
                        server->lpVtbl->Release(server);
                        CoUninitialize();
                        return -1;
                    }
                }
                CoTaskMemFree(name);
            }
            flat_enum->lpVtbl->Release(flat_enum);
        }

        /* If flat browse yields nothing, fallback to recursive hierarchical browse. */
        if (count == 0 && (FAILED(hr) || org == OPC_NS_HIERARCHIAL || org == OPC_NS_FLAT)) {
            BrowseJsonCtx ctx;
            ctx.out = out;
            ctx.out_sz = out_sz;
            ctx.used = &used;
            ctx.count = &count;
            ctx.max_items = max_items;
            if (browse_collect_recursive(browse, &ctx, 0) != 0) {
                browse->lpVtbl->Release(browse);
                server->lpVtbl->Release(server);
                CoUninitialize();
                return -1;
            }
        }
    }

    if (json_append(out, out_sz, &used, "],\"truncated\":") != 0 ||
        json_append(out, out_sz, &used, (count >= max_items) ? "true" : "false") != 0 ||
        json_append(out, out_sz, &used, ",\"count\":") != 0) {
        browse->lpVtbl->Release(browse);
        server->lpVtbl->Release(server);
        CoUninitialize();
        return -1;
    }
    {
        char num[32];
        snprintf(num, sizeof(num), "%d}", count);
        if (json_append(out, out_sz, &used, num) != 0) {
            browse->lpVtbl->Release(browse);
            server->lpVtbl->Release(server);
            CoUninitialize();
            return -1;
        }
    }

    browse->lpVtbl->Release(browse);
    server->lpVtbl->Release(server);
    CoUninitialize();
    return 0;
}

static void strip_outer_quotes(char *s)
{
    size_t n;
    if (!s) return;
    n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

static int build_opcda_diagnostic_json(const AppConfig *cfg, char *out, size_t out_sz)
{
    char clsid[MAX_STR_LEN];
    char path[MAX_PATH_LEN];
    char view[16];
    const char *progid;
    REGSAM wow_flag;
    DWORD attrs;
    int found = 0;
    const char *hint = "No specific abnormality detected.";

    if (!cfg || !out || out_sz < 128) return -1;
    progid = cfg->opcda.server_progid[0] ? cfg->opcda.server_progid : "";
    clsid[0] = '\0';
    path[0] = '\0';
    snprintf(view, sizeof(view), "%s", "unknown");

    {
        char key[MAX_PATH_LEN];
        snprintf(key, sizeof(key), "%s\\CLSID", progid);
        if (progid[0] && reg_read_default_value(HKEY_CLASSES_ROOT, key, KEY_WOW64_64KEY, clsid, sizeof(clsid)) == 0) {
            snprintf(view, sizeof(view), "%s", "64-bit");
            wow_flag = KEY_WOW64_64KEY;
            found = 1;
        } else if (progid[0] && reg_read_default_value(HKEY_CLASSES_ROOT, key, KEY_WOW64_32KEY, clsid, sizeof(clsid)) == 0) {
            snprintf(view, sizeof(view), "%s", "32-bit");
            wow_flag = KEY_WOW64_32KEY;
            found = 1;
        } else {
            wow_flag = KEY_WOW64_64KEY;
        }
    }

    if (found) {
        char local_key[MAX_PATH_LEN];
        snprintf(local_key, sizeof(local_key), "CLSID\\%s\\LocalServer32", clsid);
        if (reg_read_default_value(HKEY_CLASSES_ROOT, local_key, wow_flag, path, sizeof(path)) != 0) {
            snprintf(local_key, sizeof(local_key), "CLSID\\%s\\_LocalServer32", clsid);
            reg_read_default_value(HKEY_CLASSES_ROOT, local_key, wow_flag, path, sizeof(path));
        }
        strip_outer_quotes(path);
    }

    attrs = path[0] ? GetFileAttributesA(path) : INVALID_FILE_ATTRIBUTES;

    if (strstr(g_gateway_message, "0x80070422") != NULL) {
        hint = "Error 0x80070422: related service is disabled or not started. Check dependent services of target OPCDA server.";
    } else if (strstr(g_gateway_message, "0x80040154") != NULL) {
        hint = "Error 0x80040154: class not registered. Usually missing ProgID registration or 32/64-bit mismatch.";
    } else if (!found) {
        hint = "COM registration for this ProgID was not found.";
    } else if (path[0] && attrs == INVALID_FILE_ATTRIBUTES) {
        hint = "COM registration was found, but the server executable path does not exist.";
    } else if (strcmp(view, "32-bit") == 0) {
        hint = "This ProgID is registered in the 32-bit view. Ensure the target OPCDA program and its dependencies are started.";
    }

    {
        size_t used = 0;
        out[0] = '\0';
        if (json_append(out, out_sz, &used, "{\"ok\":true,\"progid\":\"") != 0 ||
            json_append_escaped(out, out_sz, &used, progid) != 0 ||
            json_append(out, out_sz, &used, "\",\"clsid\":\"") != 0 ||
            json_append_escaped(out, out_sz, &used, clsid) != 0 ||
            json_append(out, out_sz, &used, "\",\"registryView\":\"") != 0 ||
            json_append_escaped(out, out_sz, &used, view) != 0 ||
            json_append(out, out_sz, &used, "\",\"serverPath\":\"") != 0 ||
            json_append_escaped(out, out_sz, &used, path) != 0 ||
            json_append(out, out_sz, &used, "\",\"serverPathExists\":") != 0 ||
            json_append(out, out_sz, &used, (path[0] && attrs != INVALID_FILE_ATTRIBUTES) ? "true" : "false") != 0 ||
            json_append(out, out_sz, &used, ",\"hint\":\"") != 0 ||
            json_append_escaped(out, out_sz, &used, hint) != 0 ||
            json_append(out, out_sz, &used, "\"}") != 0) {
            return -1;
        }
    }
    return 0;
}

static int read_default_value(HKEY key, char *out, DWORD out_sz)
{
    DWORD type = 0;
    DWORD cb = out_sz;
    LONG rc = RegQueryValueExA(key, NULL, NULL, &type, (LPBYTE)out, &cb);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return -1;
    if (cb == 0 || cb > out_sz) return -1;
    out[out_sz - 1] = '\0';
    return 0;
}

static int append_progid_from_clsid(const char *clsid_name,
                                    REGSAM wow_flag,
                                    char *out, size_t out_sz,
                                    size_t *used, int *count)
{
    char cat_10a[256];
    char cat_20[256];
    snprintf(cat_10a, sizeof(cat_10a), "CLSID\\%s\\Implemented Categories\\%s", clsid_name, OPCDA_CATID_10A);
    snprintf(cat_20, sizeof(cat_20), "CLSID\\%s\\Implemented Categories\\%s", clsid_name, OPCDA_CATID_20);
    if (!reg_key_exists(HKEY_CLASSES_ROOT, cat_10a, wow_flag) &&
        !reg_key_exists(HKEY_CLASSES_ROOT, cat_20, wow_flag)) {
        return 0;
    }

    char progid_path[256];
    snprintf(progid_path, sizeof(progid_path), "CLSID\\%s\\ProgID", clsid_name);

    HKEY hProgId = NULL;
    if (RegOpenKeyExA(HKEY_CLASSES_ROOT, progid_path, 0, KEY_READ | wow_flag, &hProgId) != ERROR_SUCCESS) {
        return 0;
    }

    char progid[MAX_STR_LEN];
    int wrote = 0;
    if (read_default_value(hProgId, progid, sizeof(progid)) == 0 && progid[0]) {
        int duplicate = 0;
        char needle[MAX_STR_LEN + 4];
        snprintf(needle, sizeof(needle), "\"%s\"", progid);
        if (strstr(out, needle) != NULL) duplicate = 1;

        if (!duplicate) {
            if (*count > 0 && json_append(out, out_sz, used, ",") != 0) {
                RegCloseKey(hProgId);
                return -1;
            }
            if (json_append(out, out_sz, used, "\"") != 0 ||
                json_append_escaped(out, out_sz, used, progid) != 0 ||
                json_append(out, out_sz, used, "\"") != 0) {
                RegCloseKey(hProgId);
                return -1;
            }
            ++(*count);
            wrote = 1;
        }
    }

    RegCloseKey(hProgId);
    return wrote;
}

static int discover_opcda_json(char *out, size_t out_sz)
{
    size_t used = 0;
    int count = 0;
    int opened64 = 0;
    int opened32 = 0;

    out[0] = '\0';
    if (json_append(out, out_sz, &used, "{\"ok\":true,\"items\":[") != 0) {
        return -1;
    }

    for (int pass = 0; pass < 2; ++pass) {
        REGSAM wow_flag = (pass == 0) ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;
        HKEY hClsid = NULL;
        LONG rc = RegOpenKeyExA(HKEY_CLASSES_ROOT, "CLSID", 0, KEY_READ | wow_flag, &hClsid);
        if (rc != ERROR_SUCCESS) {
            continue;
        }
        if (pass == 0) opened64 = 1;
        else opened32 = 1;

        for (DWORD i = 0;; ++i) {
            char clsid_name[128];
            DWORD clsid_len = (DWORD)(sizeof(clsid_name) - 1);
            FILETIME ft;
            rc = RegEnumKeyExA(hClsid, i, clsid_name, &clsid_len, NULL, NULL, NULL, &ft);
            if (rc == ERROR_NO_MORE_ITEMS) break;
            if (rc != ERROR_SUCCESS) continue;
            clsid_name[clsid_len] = '\0';

            int wrote = append_progid_from_clsid(clsid_name, wow_flag,
                                                 out, out_sz, &used, &count);
            if (wrote < 0) {
                RegCloseKey(hClsid);
                return -1;
            }
        }

        RegCloseKey(hClsid);
    }

    if (!opened64 && !opened32) {
        snprintf(out, out_sz, "{\"ok\":false,\"error\":\"open_registry_failed\"}");
        return -1;
    }

    if (json_append(out, out_sz, &used, "],\"scan\":{") != 0 ||
        json_append(out, out_sz, &used, opened64 ? "\"view64\":true," : "\"view64\":false,") != 0 ||
        json_append(out, out_sz, &used, opened32 ? "\"view32\":true}" : "\"view32\":false}") != 0 ||
        json_append(out, out_sz, &used, "}") != 0) {
        return -1;
    }

    return 0;
}

static int parse_port_from_endpoint(const char *endpoint)
{
    if (!endpoint) return 4840;
    const char *colon = strrchr(endpoint, ':');
    if (!colon || !colon[1]) return 4840;
    int p = atoi(colon + 1);
    if (p <= 0 || p > 65535) return 4840;
    return p;
}

static int parse_form_port(const char *body)
{
    if (!body) return -1;
    const char *p = strstr(body, "port=");
    if (!p) return -1;
    p += 5;
    return atoi(p);
}

static int form_get_value(const char *body, const char *key, char *out, size_t out_sz)
{
    if (!body || !key || !out || out_sz == 0) return -1;

    size_t key_len = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seg_len = amp ? (size_t)(amp - p) : strlen(p);
        if (seg_len > key_len + 1 && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            size_t vlen = seg_len - key_len - 1;
            if (vlen >= out_sz) vlen = out_sz - 1;
            memcpy(out, p + key_len + 1, vlen);
            out[vlen] = '\0';
            for (size_t i = 0; i < vlen; ++i) {
                if (out[i] == '+') out[i] = ' ';
            }
            for (size_t i = 0; out[i]; ++i) {
                if (out[i] == '%' && out[i + 1] && out[i + 2]) {
                    char h1 = out[i + 1];
                    char h2 = out[i + 2];
                    int v1 = (h1 >= '0' && h1 <= '9') ? (h1 - '0') : ((h1 >= 'a' && h1 <= 'f') ? (h1 - 'a' + 10) : ((h1 >= 'A' && h1 <= 'F') ? (h1 - 'A' + 10) : -1));
                    int v2 = (h2 >= '0' && h2 <= '9') ? (h2 - '0') : ((h2 >= 'a' && h2 <= 'f') ? (h2 - 'a' + 10) : ((h2 >= 'A' && h2 <= 'F') ? (h2 - 'A' + 10) : -1));
                    if (v1 >= 0 && v2 >= 0) {
                        out[i] = (char)((v1 << 4) | v2);
                        memmove(out + i + 1, out + i + 3, strlen(out + i + 3) + 1);
                    }
                }
            }
            return 0;
        }
        p = amp ? (amp + 1) : NULL;
    }

    return -1;
}

static int parse_gateway_enabled(const char *body)
{
    char tmp[16];
    if (form_get_value(body, "enabled", tmp, sizeof(tmp)) != 0) return 0;
    return atoi(tmp) ? 1 : 0;
}

static int collect_form_values(const char *body, const char *key,
                               char values[][MAX_STR_LEN], int max_values)
{
    int count = 0;
    size_t key_len;
    const char *p;
    if (!body || !key || !values || max_values <= 0) return 0;
    key_len = strlen(key);
    p = body;
    while (p && *p && count < max_values) {
        const char *amp = strchr(p, '&');
        size_t seg_len = amp ? (size_t)(amp - p) : strlen(p);
        if (seg_len > key_len + 1 && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            size_t vlen = seg_len - key_len - 1;
            if (vlen >= MAX_STR_LEN) vlen = MAX_STR_LEN - 1;
            memcpy(values[count], p + key_len + 1, vlen);
            values[count][vlen] = '\0';
            for (size_t i = 0; values[count][i]; ++i) {
                if (values[count][i] == '+') values[count][i] = ' ';
            }
            for (size_t i = 0; values[count][i]; ++i) {
                if (values[count][i] == '%' && values[count][i + 1] && values[count][i + 2]) {
                    char h1 = values[count][i + 1];
                    char h2 = values[count][i + 2];
                    int v1 = (h1 >= '0' && h1 <= '9') ? (h1 - '0') : ((h1 >= 'a' && h1 <= 'f') ? (h1 - 'a' + 10) : ((h1 >= 'A' && h1 <= 'F') ? (h1 - 'A' + 10) : -1));
                    int v2 = (h2 >= '0' && h2 <= '9') ? (h2 - '0') : ((h2 >= 'a' && h2 <= 'f') ? (h2 - 'a' + 10) : ((h2 >= 'A' && h2 <= 'F') ? (h2 - 'A' + 10) : -1));
                    if (v1 >= 0 && v2 >= 0) {
                        values[count][i] = (char)((v1 << 4) | v2);
                        memmove(values[count] + i + 1, values[count] + i + 3,
                                strlen(values[count] + i + 3) + 1);
                    }
                }
            }
            if (values[count][0]) count++;
        }
        p = amp ? (amp + 1) : NULL;
    }
    return count;
}

static int write_nodes_mapping(const char *nodes_path,
                               char items[][MAX_STR_LEN], int item_count)
{
    FILE *fp;
    if (!nodes_path || item_count <= 0) return -1;
    fp = fopen(nodes_path, "wb");
    if (!fp) return -1;

    fputs("{\n  \"String[256]\": {\n", fp);
    for (int i = 0; i < item_count; ++i) {
        const char *key = items[i];
        fputs("    \"", fp);
        for (const unsigned char *p = (const unsigned char *)key; *p; ++p) {
            if (*p == '\\' || *p == '"') fputc('\\', fp);
            fputc((int)*p, fp);
        }
        fputs("\": \"", fp);
        for (const unsigned char *p = (const unsigned char *)items[i]; *p; ++p) {
            if (*p == '\\' || *p == '"') fputc('\\', fp);
            fputc((int)*p, fp);
        }
        fputs("\"", fp);
        fputs((i + 1 < item_count) ? ",\n" : "\n", fp);
    }
    fputs("  }\n}\n", fp);
    fclose(fp);
    return 0;
}

static void json_escape_copy(char *dst, size_t cap, const char *src)
{
    size_t used = 0;
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    if (!src) return;
    json_append_escaped(dst, cap, &used, src);
}

void web_config_set_gateway_status(int enabled, int connected,
                                   const char *progid, const char *message)
{
    if (!g_status_lock_ready) return;
    EnterCriticalSection(&g_status_lock);
    g_gateway_enabled = enabled ? 1 : 0;
    g_gateway_connected = connected ? 1 : 0;
    snprintf(g_gateway_progid, sizeof(g_gateway_progid), "%s", progid ? progid : "");
    snprintf(g_gateway_message, sizeof(g_gateway_message), "%s", message ? message : "");
    g_gateway_updated_ms = GetTickCount();
    LeaveCriticalSection(&g_status_lock);
}

void web_config_set_opcua_status(int enabled, int listening, int port, const char *message)
{
    if (!g_status_lock_ready) return;
    EnterCriticalSection(&g_status_lock);
    g_opcua_enabled = enabled ? 1 : 0;
    g_opcua_listening = listening ? 1 : 0;
    g_opcua_port = port;
    snprintf(g_opcua_message, sizeof(g_opcua_message), "%s", message ? message : "");
    LeaveCriticalSection(&g_status_lock);
}

void web_config_set_gateway_runtime_enabled(int enabled)
{
    if (!g_status_lock_ready) return;
    EnterCriticalSection(&g_status_lock);
    g_gateway_runtime_enabled = enabled ? 1 : 0;
    LeaveCriticalSection(&g_status_lock);
}

int web_config_get_gateway_runtime_enabled(void)
{
    int enabled = 1;
    if (!g_status_lock_ready) return enabled;
    EnterCriticalSection(&g_status_lock);
    enabled = g_gateway_runtime_enabled;
    LeaveCriticalSection(&g_status_lock);
    return enabled;
}

static void build_status_json(char *out, size_t out_sz)
{
    char progid_esc[256];
    char message_esc[512];
    DWORD updated_ms;
    int enabled;
    int connected;
    int runtime_enabled;
    int opcua_enabled;
    int opcua_listening;
    int opcua_port;
    char opcua_msg_esc[512];

    if (!g_status_lock_ready) {
        snprintf(out, out_sz,
                 "{\"ok\":true,\"gateway\":{\"enabled\":false,\"connected\":false,"
                 "\"runtimeEnabled\":false,\"progid\":\"\",\"message\":\"status_not_ready\",\"updatedMs\":0},"
                 "\"opcua\":{\"enabled\":false,\"listening\":false,\"port\":0,\"message\":\"not_ready\"},"
                 "\"clients\":[{\"clientId\":\"opcda://(none)\",\"connectionStatus\":\"disconnected\"}]}");
        return;
    }

    EnterCriticalSection(&g_status_lock);
    enabled = g_gateway_enabled;
    connected = g_gateway_connected;
    runtime_enabled = g_gateway_runtime_enabled;
    opcua_enabled = g_opcua_enabled;
    opcua_listening = g_opcua_listening;
    opcua_port = g_opcua_port;
    json_escape_copy(progid_esc, sizeof(progid_esc), g_gateway_progid);
    json_escape_copy(message_esc, sizeof(message_esc), g_gateway_message);
    json_escape_copy(opcua_msg_esc, sizeof(opcua_msg_esc), g_opcua_message);
    updated_ms = g_gateway_updated_ms;
    LeaveCriticalSection(&g_status_lock);

    snprintf(out, out_sz,
             "{\"ok\":true,\"gateway\":{\"enabled\":%s,\"connected\":%s,"
             "\"runtimeEnabled\":%s,\"progid\":\"%s\",\"message\":\"%s\",\"updatedMs\":%lu},"
             "\"opcua\":{\"enabled\":%s,\"listening\":%s,\"port\":%d,\"message\":\"%s\"},"
             "\"clients\":[{\"clientId\":\"opcda://%s\",\"connectionStatus\":\"%s\"}]}",
             enabled ? "true" : "false",
             connected ? "true" : "false",
             runtime_enabled ? "true" : "false",
             progid_esc,
             message_esc,
             (unsigned long)updated_ms,
             opcua_enabled ? "true" : "false",
             opcua_listening ? "true" : "false",
             opcua_port,
             opcua_msg_esc,
             progid_esc[0] ? progid_esc : "(not-set)",
             enabled ? (runtime_enabled ? (connected ? "connected" : "disconnected") : "stopped") : "disabled");
}

static const char *tag_type_to_text(TagType t)
{
    switch (t) {
        case TAG_TYPE_BOOL: return "Bool";
        case TAG_TYPE_INT: return "Int";
        case TAG_TYPE_REAL: return "Real";
        case TAG_TYPE_DINT: return "Dint";
        case TAG_TYPE_STRING: return "String";
        default: return "Unknown";
    }
}

static void dirname_of_path(const char *full_path, char *out, size_t out_sz)
{
    if (!full_path || !out || out_sz == 0) return;
    snprintf(out, out_sz, "%s", full_path);
    char *p1 = strrchr(out, '/');
    char *p2 = strrchr(out, '\\');
    char *p = p1;
    if (!p || (p2 && p2 > p)) p = p2;
    if (p) {
        *p = '\0';
    } else {
        snprintf(out, out_sz, ".");
    }
}

static void join_path(char *out, size_t out_sz, const char *dir, const char *name)
{
    if (!out || out_sz == 0) return;
    if (!dir || !dir[0]) {
        snprintf(out, out_sz, "%s", name ? name : "");
        return;
    }
    snprintf(out, out_sz, "%s/%s", dir, name ? name : "");
}

static int build_datapoints_json_from_config(const AppConfig *cfg, char *out, size_t out_sz)
{
    if (!cfg || !out || out_sz < 64) return -1;

    char config_dir[MAX_PATH_LEN];
    char nodes_path[MAX_PATH_LEN];
    dirname_of_path(g_config_path, config_dir, sizeof(config_dir));
    join_path(nodes_path, sizeof(nodes_path), config_dir, cfg->opcua.tag_file);

    NodeConfig ncfg;
    if (nodes_load(nodes_path, &ncfg) != 0) {
        snprintf(out, out_sz,
                 "{\"ok\":false,\"error\":\"nodes_load_failed\",\"path\":\"%s\"}",
                 nodes_path);
        return -1;
    }

    size_t used = 0;
    out[0] = '\0';

    if (json_append(out, out_sz, &used, "{\"ok\":true,\"service\":\"") != 0 ||
        json_append_escaped(out, out_sz, &used,
                            cfg->opcda.server_progid[0] ? cfg->opcda.server_progid : "(not-set)") != 0 ||
        json_append(out, out_sz, &used, "\",\"enabled\":") != 0 ||
        json_append(out, out_sz, &used, (strcmp(cfg->opcda.mode, "opcda_com") == 0) ? "true" : "false") != 0 ||
        json_append(out, out_sz, &used, ",\"items\":[") != 0) {
        nodes_free(&ncfg);
        return -1;
    }

    for (int i = 0; i < ncfg.count; ++i) {
        const NodeDef *nd = &ncfg.nodes[i];
        if (i > 0 && json_append(out, out_sz, &used, ",") != 0) {
            nodes_free(&ncfg);
            return -1;
        }
        if (json_append(out, out_sz, &used, "{\"name\":\"") != 0 ||
            json_append_escaped(out, out_sz, &used, nd->name) != 0 ||
            json_append(out, out_sz, &used, "\",\"source\":\"") != 0 ||
            json_append_escaped(out, out_sz, &used, nd->source[0] ? nd->source : nd->name) != 0 ||
            json_append(out, out_sz, &used, "\",\"type\":\"") != 0 ||
            json_append(out, out_sz, &used, tag_type_to_text(nd->type)) != 0 ||
            json_append(out, out_sz, &used, "\"}") != 0) {
            nodes_free(&ncfg);
            return -1;
        }
    }

    if (json_append(out, out_sz, &used, "]}") != 0) {
        nodes_free(&ncfg);
        return -1;
    }

    nodes_free(&ncfg);
    return 0;
}

static void send_response(SOCKET s, const char *status,
                          const char *ctype, const char *body)
{
    char header[512];
    int body_len = (int)strlen(body);
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %d\r\n"
                     "Connection: close\r\n\r\n",
                     status, ctype, body_len);
    send(s, header, n, 0);
    send(s, body, body_len, 0);
}

static const char *find_header_case_insensitive(const char *text, const char *name)
{
    size_t nlen = strlen(name);
    for (const char *p = text; p && *p; ++p) {
        if (_strnicmp(p, name, nlen) == 0) return p;
    }
    return NULL;
}

static int recv_http_request(SOCKET client, char *buf, int cap)
{
    int total = 0;
    int header_len = -1;
    int want_body = 0;

    while (total < cap - 1) {
        int r = recv(client, buf + total, cap - 1 - total, 0);
        if (r <= 0) break;
        total += r;
        buf[total] = '\0';

        if (header_len < 0) {
            char *hdr_end = strstr(buf, "\r\n\r\n");
            if (hdr_end) {
                header_len = (int)(hdr_end - buf) + 4;
                const char *cl = find_header_case_insensitive(buf, "Content-Length:");
                if (cl) {
                    cl += 15;
                    while (*cl == ' ' || *cl == '\t') ++cl;
                    want_body = atoi(cl);
                    if (want_body < 0) want_body = 0;
                }
            }
        }

        if (header_len >= 0 && total >= header_len + want_body) {
            break;
        }
    }

    if (total <= 0) return -1;
    buf[total] = '\0';
    return total;
}

static void handle_client(SOCKET client)
{
    char req[8192];
    int r = recv_http_request(client, req, (int)sizeof(req));
    if (r <= 0) return;

    AppConfig cfg;
    if (config_load(g_config_path, &cfg) != 0) {
        send_response(client, "500 Internal Server Error", "text/plain; charset=utf-8",
                      "Failed to read config");
        return;
    }

    if (strncmp(req, "GET / ", 6) == 0 || strncmp(req, "GET /index", 10) == 0) {
        int current_port = parse_port_from_endpoint(cfg.opcua.endpoint);
        int gateway_enabled = (strcmp(cfg.opcda.mode, "opcda_com") == 0) ? 1 : 0;
        char html[32768];
        snprintf(html, sizeof(html),
                 "<!doctype html><html><head><meta charset='utf-8'>"
                 "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                 "<title>OPCUA \\u7f51\\u5173\\u914d\\u7f6e</title>"
                 "<style>body{font-family:Segoe UI,Arial,sans-serif;max-width:720px;margin:40px auto;padding:0 16px;}"
                 "h1{font-size:24px;}input,button{font-size:16px;padding:8px 10px;}"
                 "button{cursor:pointer;margin-left:8px;} .box{border:1px solid #ddd;border-radius:8px;padding:16px;}"
                 "ul{line-height:1.6;} .hint{color:#666;} .mono{font-family:Consolas,monospace;}"
                 ".status{margin:8px 0;padding:8px;border-radius:6px;background:#f7f7f7;}"
                 ".ok{color:#0a7a2f;font-weight:600;} .bad{color:#b32727;font-weight:600;}"
                 ".tree{font-family:Consolas,monospace;background:#fafafa;border:1px solid #eee;padding:8px;border-radius:6px;}"
                 ".tree ul{margin:0;padding-left:18px;}"
                 "table{border-collapse:collapse;width:100%%;}th,td{border:1px solid #ddd;padding:6px 8px;text-align:left;}"
                 "details{margin:2px 0;} summary{cursor:pointer;user-select:none;}"
                 ".leaf{margin-left:18px;color:#444;}"
                 "</style></head><body><h1>OPCUA \\u7f51\\u5173\\u914d\\u7f6e</h1>"
                 "<div class='box'><p>\\u5f53\\u524d\\u7aef\\u70b9: <b>%s</b></p>"
                 "<p>\\u5f53\\u524d OPCDA ProgID: <span class='mono'>%s</span></p>"
                 "<form method='POST' action='/set-port'>"
                 "<label>OPCUA \\u7aef\\u53e3: <input type='number' min='1' max='65535' name='port' value='%d'></label>"
                 "<button type='submit'>\\u4fdd\\u5b58\\u7aef\\u53e3</button></form>"
                 "<p>\\u4fdd\\u5b58\\u540e\\u9700\\u91cd\\u542f\\u670d\\u52a1\\uff0c\\u65b0\\u7aef\\u70b9\\u624d\\u4f1a\\u751f\\u6548\\u3002</p>"
                 "<hr><h2>\\u53d1\\u73b0\\u672c\\u673a OPCDA \\u670d\\u52a1</h2>"
                 "<p class='hint'>\\u70b9\\u51fb\\u53d1\\u73b0\\uff0c\\u5217\\u51fa\\u672c\\u673a\\u5df2\\u6ce8\\u518c\\u7684 OPCDA ProgID\\u3002</p>"
                 "<button type='button' onclick='discoverOpcda()'>\\u53d1\\u73b0 OPCDA \\u670d\\u52a1</button>"
                 "<p><label><input id='enableGateway' type='checkbox' %s> \\u542f\\u7528 OPCDA \\u5230 OPCUA \\u7f51\\u5173\\u8f6c\\u6362</label></p>"
                 "<ul id='opcdaList'></ul>"
                 "<p><button type='button' onclick='applyGateway()'>\\u5e94\\u7528\\u7f51\\u5173\\u9009\\u62e9</button>"
                 "<span id='applyMsg' class='hint'></span></p>"
                 "<p><button type='button' onclick='setGatewayRuntime(1)'>\\u542f\\u52a8\\u7f51\\u5173</button>"
                 "<button type='button' onclick='setGatewayRuntime(0)'>\\u505c\\u6b62\\u7f51\\u5173</button>"
                 "<span id='runtimeMsg' class='hint'></span></p>"
                 "<hr><h2>\\u7f51\\u5173\\u8fd0\\u884c\\u72b6\\u6001</h2>"
                 "<div id='statusBox' class='status'>\\u6b63\\u5728\\u52a0\\u8f7d\\u72b6\\u6001...</div>"
                 "<div id='opcuaBox' class='status'>\\u6b63\\u5728\\u52a0\\u8f7d OPCUA \\u76d1\\u542c\\u72b6\\u6001...</div>"
                 "<h3>\\u5ba2\\u6237\\u7aef\\u8fde\\u63a5\\u72b6\\u6001</h3>"
                 "<table><thead><tr><th>\\u5ba2\\u6237\\u7aef ID</th><th>\\u8fde\\u63a5\\u72b6\\u6001</th></tr></thead><tbody id='clientRows'><tr><td colspan='2'>\\u6b63\\u5728\\u52a0\\u8f7d...</td></tr></tbody></table>"
                 "<hr><h2>OPCDA \\u8bca\\u65ad</h2>"
                 "<div id='diagBox' class='status'>\\u6b63\\u5728\\u52a0\\u8f7d\\u8bca\\u65ad\\u4fe1\\u606f...</div>"
                 "<hr><h2>DA \\u6570\\u636e\\u70b9\\u6811</h2>"
                 "<p class='hint'>\\u6309\\u5f53\\u524d\\u6240\\u9009 DA \\u670d\\u52a1\\u548c\\u8282\\u70b9\\u6620\\u5c04\\u914d\\u7f6e\\u751f\\u6210\\u6811\\u5f62\\u6570\\u636e\\u70b9\\u3002</p>"
                 "<div id='treeTitle' class='hint'></div>"
                 "<div id='treeBox' class='tree'>\\u6b63\\u5728\\u52a0\\u8f7d\\u6570\\u636e\\u70b9...</div>"
                 "<hr><h2>DA \\u5b9e\\u65f6\\u6d4f\\u89c8\\u6570\\u636e\\u70b9</h2>"
                 "<p class='hint'>\\u76f4\\u63a5\\u4ece\\u6240\\u9009 OPCDA \\u670d\\u52a1\\u6d4f\\u89c8\\u53ef\\u7528\\u6570\\u636e\\u70b9\\uff0c\\u652f\\u6301\\u6811\\u5f62\\u5c55\\u5f00/\\u6298\\u53e0\\u3002</p>"
                 "<p><button type='button' onclick='loadBrowseTree()'>\\u6d4f\\u89c8\\u6570\\u636e\\u70b9</button>"
                 "<span id='browseMeta' class='hint'></span></p>"
                 "<p><button type='button' onclick='applyBridgeFromBrowse()'>\\u52fe\\u9009\\u540e\\u542f\\u7528\\u6570\\u636e\\u6865\\u63a5</button>"
                 "<span id='bridgeApplyMsg' class='hint'></span></p>"
                 "<div id='browseTreeBox' class='tree'>\\u70b9\\u51fb\"\\u6d4f\\u89c8\\u6570\\u636e\\u70b9\"\\u5f00\\u59cb\\u3002</div>"
                 "</div>"
                 "<script>"
                 "function zh(s){return String(s||'').replace(/\\\\u([0-9a-fA-F]{4})/g,function(_,h){return String.fromCharCode(parseInt(h,16));});}"
                 "var currentProgId=%c%s%c;"
                 "function escHtml(s){return String(s||'').replace(/[&<>\"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c];});}"
                 "function discoverOpcda(){"
                 "var ul=document.getElementById('opcdaList');"
                 "ul.innerHTML='<li>\\u6b63\\u5728\\u626b\\u63cf\\u6ce8\\u518c\\u8868...</li>';"
                 "fetch('/discover-opcda').then(function(r){return r.json();}).then(function(d){"
                 "if(!d||!d.ok){ul.innerHTML='<li>\\u670d\\u52a1\\u53d1\\u73b0\\u5931\\u8d25</li>';return;}"
                 "if(!d.items||d.items.length===0){ul.innerHTML='<li>\\u672a\\u53d1\\u73b0 OPCDA \\u670d\\u52a1</li>';return;}"
                 "ul.innerHTML='';"
                 "d.items.forEach(function(x){"
                 "var li=document.createElement('li');"
                 "var cb=document.createElement('input');cb.type='checkbox';cb.className='svc';cb.value=x;"
                 "if(currentProgId&&x===currentProgId){cb.checked=true;}"
                 "cb.onchange=function(){if(cb.checked){document.querySelectorAll('.svc').forEach(function(z){if(z!==cb)z.checked=false;}); currentProgId=cb.value; loadDataTree();}};"
                 "var txt=document.createElement('span');txt.textContent=' '+x;"
                 "li.appendChild(cb);li.appendChild(txt);ul.appendChild(li);"
                 "});"
                 "}).catch(function(){ul.innerHTML='<li>\\u53d1\\u73b0\\u8bf7\\u6c42\\u5931\\u8d25</li>';});"
                 "}"
                 "function applyGateway(){"
                 "var enable=document.getElementById('enableGateway').checked?1:0;"
                 "var checks=[].slice.call(document.querySelectorAll('.svc:checked'));"
                 "if(enable&&checks.length!==1){document.getElementById('applyMsg').textContent='\\u542f\\u7528\\u7f51\\u5173\\u65f6\\u5fc5\\u987b\\u4e14\\u53ea\\u80fd\\u9009\\u62e9\\u4e00\\u4e2a OPCDA \\u670d\\u52a1\\u3002';return;}"
                 "var progid=(checks.length>0)?checks[0].value:'';"
                 "var body='enabled='+enable+'&progid='+encodeURIComponent(progid);"
                 "fetch('/set-gateway',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})"
                 ".then(function(r){return r.text();})"
                 ".then(function(t){document.getElementById('applyMsg').textContent=t; if(checks.length===1){currentProgId=checks[0].value;} loadStatus(); loadDataTree();})"
                 ".catch(function(){document.getElementById('applyMsg').textContent='\\u5e94\\u7528\\u7f51\\u5173\\u9009\\u62e9\\u5931\\u8d25';});"
                 "}"
                 "function setGatewayRuntime(enabled){"
                 "var body='enabled='+(enabled?1:0);"
                 "fetch('/gateway-runtime',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})"
                 ".then(function(r){return r.text();})"
                 ".then(function(t){document.getElementById('runtimeMsg').textContent=t; loadStatus();})"
                 ".catch(function(){document.getElementById('runtimeMsg').textContent='\\u5207\\u6362\\u7f51\\u5173\\u8fd0\\u884c\\u72b6\\u6001\\u5931\\u8d25';});"
                 "}"
                 "function loadStatus(){"
                 "fetch('/status').then(function(r){return r.json();}).then(function(d){"
                 "var box=document.getElementById('statusBox');"
                  "if(!d||!d.ok||!d.gateway){box.textContent='\\u72b6\\u6001\\u4e0d\\u53ef\\u7528';return;}"
                 "var g=d.gateway;"
                 "var s1=g.enabled? '\\u5df2\\u542f\\u7528':'\\u5df2\\u7981\\u7528';"
                 "var sr=g.runtimeEnabled? '\\u8fd0\\u884c\\u4e2d':'\\u5df2\\u505c\\u6b62';"
                 "var s2=g.connected? '<span class=\"ok\">\\u5df2\\u8fde\\u63a5</span>' : '<span class=\"bad\">\\u672a\\u8fde\\u63a5</span>';"
                 "var p=g.progid||'(\\u672a\\u8bbe\\u7f6e)';"
                 "var m=g.message||'';"
                 "box.innerHTML='\\u7f51\\u5173: <b>'+s1+'</b> | \\u8fd0\\u884c: <b>'+sr+'</b> | DA \\u8fde\\u63a5: '+s2+'<br>ProgID: <span class=\"mono\">'+p+'</span><br>'+m;"
                 "var u=(d.opcua||{});"
                 "var ul=u.listening? '<span class=\"ok\">\\u5df2\\u76d1\\u542c</span>' : '<span class=\"bad\">\\u672a\\u76d1\\u542c</span>';"
                 "document.getElementById('opcuaBox').innerHTML='OPCUA: '+ul+' | \\u7aef\\u53e3: <b>'+(u.port||0)+'</b><br>'+(u.message||'');"
                 "var rows=document.getElementById('clientRows');"
                 "if(!d.clients||d.clients.length===0){rows.innerHTML='<tr><td colspan=\"2\">\\u6682\\u65e0\\u5ba2\\u6237\\u7aef\\u6570\\u636e</td></tr>'; }"
                 "else{rows.innerHTML=''; d.clients.forEach(function(c){var tr=document.createElement('tr'); var cs=(c.connectionStatus||''); if(cs==='connected') cs='\\u5df2\\u8fde\\u63a5'; else if(cs==='disconnected') cs='\\u672a\\u8fde\\u63a5'; else if(cs==='stopped') cs='\\u5df2\\u505c\\u6b62'; else if(cs==='disabled') cs='\\u5df2\\u7981\\u7528'; tr.innerHTML='<td>'+escHtml(c.clientId||'')+'</td><td>'+escHtml(cs)+'</td>'; rows.appendChild(tr);});}"
                 "}).catch(function(){document.getElementById('statusBox').textContent='\\u72b6\\u6001\\u8bf7\\u6c42\\u5931\\u8d25';});"
                 "}"
                 "function buildTree(items){"
                 "var root={c:{}};"
                 "items.forEach(function(it){"
                 "var path=(it.source||it.name||'').split(/[\\./]/).filter(Boolean);"
                 "if(path.length===0) path=[it.name||'\\u9879\\u76ee'];"
                 "var cur=root;"
                 "path.forEach(function(seg,idx){cur.c[seg]=cur.c[seg]||{c:{},leaf:[]}; if(idx===path.length-1){cur.c[seg].leaf.push(it);} cur=cur.c[seg];});"
                 "});"
                 "function render(node,depth){var keys=Object.keys(node.c); if(keys.length===0) return ''; var h='<ul>'; keys.sort().forEach(function(k){var n=node.c[k]; var sub=render(n,depth+1); var hasChild=(sub&&sub.length>0)||(n.leaf&&n.leaf.length>0); if(hasChild){h+='<li><details '+(depth<1?'open':'')+'><summary>'+escHtml(k)+'</summary>'; if(n.leaf&&n.leaf.length){n.leaf.forEach(function(it){h+='<div class=\"leaf\">'+escHtml((it.name||'')+' ['+(it.type||'')+']')+'</div>';});} h+=sub+'</details></li>';} else {h+='<li>'+escHtml(k)+'</li>';}}); h+='</ul>'; return h;}"
                 "return render(root,0);"
                 "}"
                 "function buildPathTree(paths){"
                 "var root={c:{}};"
                 "(paths||[]).forEach(function(p){var segs=String(p||'').split(/[\\./]/).filter(Boolean); if(segs.length===0) return; var cur=root; segs.forEach(function(seg,idx){cur.c[seg]=cur.c[seg]||{c:{},v:null}; if(idx===segs.length-1){cur.c[seg].v=String(p);} cur=cur.c[seg];});});"
                 "function render(node,depth){var keys=Object.keys(node.c); if(keys.length===0) return ''; var h='<ul>'; keys.sort().forEach(function(k){var n=node.c[k]; var sub=render(n,depth+1); if(sub){h+='<li><details '+(depth<1?'open':'')+'><summary><label><input type=\"checkbox\" class=\"browseParent\" onchange=\"toggleChildChecks(this)\"> '+escHtml(k)+'</label></summary>'+sub+'</details></li>';} else {h+='<li class=\"leaf\"><label><input type=\"checkbox\" class=\"browseLeaf\" value=\"'+escHtml(n.v||k)+'\"> '+escHtml(k)+'</label></li>';}}); h+='</ul>'; return h;}"
                 "return render(root,0);"
                 "}"
                 "function toggleChildChecks(cb){var li=cb.closest('li'); if(!li) return; var checked=cb.checked; li.querySelectorAll('input.browseParent,input.browseLeaf').forEach(function(x){if(x!==cb) x.checked=checked;});}"
                 "function loadDataTree(){"
                 "fetch('/data-tree').then(function(r){return r.json();}).then(function(d){"
                 "var box=document.getElementById('treeBox'); var title=document.getElementById('treeTitle');"
                 "if(!d||!d.ok){box.textContent='\\u52a0\\u8f7d\\u6570\\u636e\\u70b9\\u5931\\u8d25'; return;}"
                 "title.textContent='\\u670d\\u52a1: '+(currentProgId||d.service||'(\\u672a\\u8bbe\\u7f6e)')+' | \\u7f51\\u5173: '+(d.enabled?'\\u542f\\u7528':'\\u7981\\u7528');"
                 "if(!d.items||d.items.length===0){box.textContent='\\u6ca1\\u6709\\u914d\\u7f6e\\u6570\\u636e\\u70b9'; return;}"
                 "box.innerHTML=buildTree(d.items);"
                 "}).catch(function(){document.getElementById('treeBox').textContent='\\u6570\\u636e\\u70b9\\u6811\\u8bf7\\u6c42\\u5931\\u8d25';});"
                 "}"
                 "function loadBrowseTree(){"
                 "var box=document.getElementById('browseTreeBox');"
                 "var meta=document.getElementById('browseMeta');"
                 "if(!currentProgId){box.textContent='\\u8bf7\\u5148\\u9009\\u62e9 OPCDA \\u670d\\u52a1'; meta.textContent=''; return;}"
                 "box.textContent='\\u6b63\\u5728\\u6d4f\\u89c8\\u6570\\u636e\\u70b9...';"
                 "fetch('/browse-opcda?progid='+encodeURIComponent(currentProgId)).then(function(r){return r.json();}).then(function(d){"
                 "if(!d||!d.ok){box.textContent='\\u6d4f\\u89c8\\u5931\\u8d25: '+((d&&d.error)?d.error:'unknown'); meta.textContent=''; return;}"
                 "meta.textContent='\\u5df2\\u53d1\\u73b0 '+(d.count||0)+' \\u4e2a\\u6570\\u636e\\u70b9'+(d.truncated?' (\\u5df2\\u622a\\u65ad)':'');"
                 "if(!d.items||d.items.length===0){box.textContent='\\u672a\\u6d4f\\u89c8\\u5230\\u6570\\u636e\\u70b9'; return;}"
                 "box.innerHTML=buildPathTree(d.items);"
                 "}).catch(function(){box.textContent='\\u6d4f\\u89c8\\u8bf7\\u6c42\\u5931\\u8d25'; meta.textContent='';});"
                 "}"
                 "function applyBridgeFromBrowse(){"
                 "var msg=document.getElementById('bridgeApplyMsg');"
                 "if(!currentProgId){msg.textContent='\\u8bf7\\u5148\\u9009\\u62e9 OPCDA \\u670d\\u52a1'; return;}"
                 "var selected=[].slice.call(document.querySelectorAll('.browseLeaf:checked')).map(function(x){return x.value;});"
                 "if(selected.length===0){msg.textContent='\\u8bf7\\u5148\\u52fe\\u9009\\u8981\\u6865\\u63a5\\u7684\\u6570\\u636e\\u70b9'; return;}"
                 "if(selected.length>64){msg.textContent='\\u5355\\u6b21\\u6700\\u591a\\u52fe\\u9009 64 \\u4e2a\\u70b9'; return;}"
                 "var body='progid='+encodeURIComponent(currentProgId);"
                 "selected.forEach(function(v){body+='&item='+encodeURIComponent(v);});"
                 "fetch('/apply-bridge',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})"
                 ".then(function(r){return r.text();})"
                 ".then(function(t){msg.textContent=t; document.getElementById('enableGateway').checked=true; loadStatus(); loadDataTree();})"
                 ".catch(function(){msg.textContent='\\u6865\\u63a5\\u5e94\\u7528\\u5931\\u8d25';});"
                 "}"
                 "function loadDiag(){fetch('/diagnose-opcda').then(function(r){return r.json();}).then(function(d){var box=document.getElementById('diagBox'); if(!d||!d.ok){box.textContent='\\u8bca\\u65ad\\u4fe1\\u606f\\u4e0d\\u53ef\\u7528'; return;} box.innerHTML='ProgID: <span class=\"mono\">'+escHtml(d.progid||'')+'</span><br>CLSID: <span class=\"mono\">'+escHtml(d.clsid||'')+'</span><br>\\u6ce8\\u518c\\u89c6\\u56fe: '+escHtml(d.registryView||'')+'<br>\\u670d\\u52a1\\u5668\\u8def\\u5f84: <span class=\"mono\">'+escHtml(d.serverPath||'')+'</span><br>\\u8def\\u5f84\\u5b58\\u5728: '+(d.serverPathExists?'\\u662f':'\\u5426')+'<br>\\u8bca\\u65ad\\u63d0\\u793a: '+escHtml(d.hint||'');}).catch(function(){document.getElementById('diagBox').textContent='\\u8bca\\u65ad\\u8bf7\\u6c42\\u5931\\u8d25';});}"
                 "document.title=zh(document.title); document.body.innerHTML=zh(document.body.innerHTML);"
                 "discoverOpcda(); loadStatus(); loadDataTree(); loadDiag(); setInterval(loadStatus,2000);"
                 "</script></body></html>",
                 cfg.opcua.endpoint[0] ? cfg.opcua.endpoint : "opc.tcp://0.0.0.0:4840",
                 cfg.opcda.server_progid[0] ? cfg.opcda.server_progid : "(not set)",
                 current_port,
                 gateway_enabled ? "checked" : "",
                 '\'',
                 cfg.opcda.server_progid[0] ? cfg.opcda.server_progid : "",
                 '\'');
        send_response(client, "200 OK", "text/html; charset=utf-8", html);
        return;
    }

    if (strncmp(req, "GET /discover-opcda", 19) == 0 &&
        (req[19] == ' ' || req[19] == '?' || req[19] == '\r')) {
        char body[32768];
        if (discover_opcda_json(body, sizeof(body)) != 0) {
            send_response(client, "500 Internal Server Error", "application/json; charset=utf-8",
                          "{\"ok\":false,\"error\":\"discovery_failed\"}");
            return;
        }
        send_response(client, "200 OK", "application/json; charset=utf-8", body);
        return;
    }

    if (strncmp(req, "GET /status", 11) == 0 &&
        (req[11] == ' ' || req[11] == '?' || req[11] == '\r')) {
        char body[1024];
        build_status_json(body, sizeof(body));
        send_response(client, "200 OK", "application/json; charset=utf-8", body);
        return;
    }

    if (strncmp(req, "GET /data-tree", 14) == 0 &&
        (req[14] == ' ' || req[14] == '?' || req[14] == '\r')) {
        char body[32768];
        if (build_datapoints_json_from_config(&cfg, body, sizeof(body)) != 0) {
            send_response(client, "500 Internal Server Error", "application/json; charset=utf-8",
                          "{\"ok\":false,\"error\":\"data_tree_failed\"}");
            return;
        }
        send_response(client, "200 OK", "application/json; charset=utf-8", body);
        return;
    }

    if (strncmp(req, "GET /browse-opcda", 17) == 0 &&
        (req[17] == ' ' || req[17] == '?' || req[17] == '\r')) {
        char progid[MAX_STR_LEN];
        char body[262144];
        progid[0] = '\0';
        if (parse_query_param(req, "progid", progid, sizeof(progid)) != 0 || !progid[0]) {
            snprintf(progid, sizeof(progid), "%s", cfg.opcda.server_progid);
        }
        if (build_opcda_browse_json(progid, body, sizeof(body)) != 0) {
            send_response(client, "500 Internal Server Error", "application/json; charset=utf-8",
                          "{\"ok\":false,\"error\":\"browse_failed\"}");
            return;
        }
        send_response(client, "200 OK", "application/json; charset=utf-8", body);
        return;
    }

    if (strncmp(req, "GET /diagnose-opcda", 19) == 0 &&
        (req[19] == ' ' || req[19] == '?' || req[19] == '\r')) {
        char body[4096];
        if (build_opcda_diagnostic_json(&cfg, body, sizeof(body)) != 0) {
            send_response(client, "500 Internal Server Error", "application/json; charset=utf-8",
                          "{\"ok\":false,\"error\":\"diagnose_failed\"}");
            return;
        }
        send_response(client, "200 OK", "application/json; charset=utf-8", body);
        return;
    }

    if (strncmp(req, "POST /set-gateway", 17) == 0) {
        const char *body = strstr(req, "\r\n\r\n");
        if (!body) {
            send_response(client, "400 Bad Request", "text/plain; charset=utf-8", "Invalid request body");
            return;
        }
        body += 4;

        int enabled = parse_gateway_enabled(body);
        char progid[MAX_STR_LEN];
        progid[0] = '\0';
        form_get_value(body, "progid", progid, sizeof(progid));

        if (enabled && progid[0] == '\0') {
            send_response(client, "400 Bad Request", "text/plain; charset=utf-8",
                          "Enable requires exactly one selected OPCDA service");
            return;
        }

        if (config_set_opcda_gateway(g_config_path, enabled, progid) != 0) {
            send_response(client, "500 Internal Server Error", "text/plain; charset=utf-8",
                          "Failed to save gateway config");
            return;
        }

        web_config_set_gateway_status(enabled, 0, progid,
                                      "Gateway config updated. Restart service to apply.");
        web_config_set_gateway_runtime_enabled(enabled);
        send_response(client, "200 OK", "text/plain; charset=utf-8",
                      "Saved gateway selection. Restart service to apply.");
        return;
    }

    if (strncmp(req, "POST /gateway-runtime", 21) == 0) {
        const char *body = strstr(req, "\r\n\r\n");
        if (!body) {
            send_response(client, "400 Bad Request", "text/plain; charset=utf-8", "Invalid request body");
            return;
        }
        body += 4;
        int enabled = parse_gateway_enabled(body);
        int cfg_enabled = 0;
        char cfg_progid[MAX_STR_LEN];
        cfg_progid[0] = '\0';
        if (g_status_lock_ready) {
            EnterCriticalSection(&g_status_lock);
            cfg_enabled = g_gateway_enabled;
            snprintf(cfg_progid, sizeof(cfg_progid), "%s", g_gateway_progid);
            LeaveCriticalSection(&g_status_lock);
        }
        web_config_set_gateway_runtime_enabled(enabled);
        web_config_set_gateway_status(cfg_enabled, 0, cfg_progid,
                                      enabled ? "Gateway runtime started." : "Gateway runtime stopped.");
        send_response(client, "200 OK", "text/plain; charset=utf-8",
                      enabled ? "Gateway runtime started" : "Gateway runtime stopped");
        return;
    }

    if (strncmp(req, "POST /apply-bridge", 18) == 0) {
        const char *body = strstr(req, "\r\n\r\n");
        char progid[MAX_STR_LEN];
        char items[64][MAX_STR_LEN];
        char config_dir[MAX_PATH_LEN];
        char nodes_path[MAX_PATH_LEN];
        int item_count;

        if (!body) {
            send_response(client, "400 Bad Request", "text/plain; charset=utf-8", "Invalid request body");
            return;
        }
        body += 4;

        progid[0] = '\0';
        form_get_value(body, "progid", progid, sizeof(progid));
        item_count = collect_form_values(body, "item", items, 64);
        if (item_count <= 0) {
            send_response(client, "400 Bad Request", "text/plain; charset=utf-8", "No selected items");
            return;
        }
        if (!progid[0]) {
            snprintf(progid, sizeof(progid), "%s", cfg.opcda.server_progid);
        }
        if (!progid[0]) {
            send_response(client, "400 Bad Request", "text/plain; charset=utf-8", "Missing OPCDA ProgID");
            return;
        }

        dirname_of_path(g_config_path, config_dir, sizeof(config_dir));
        join_path(nodes_path, sizeof(nodes_path), config_dir, cfg.opcua.tag_file);
        if (write_nodes_mapping(nodes_path, items, item_count) != 0) {
            send_response(client, "500 Internal Server Error", "text/plain; charset=utf-8", "Failed to save nodes mapping");
            return;
        }
        if (config_set_opcda_gateway(g_config_path, 1, progid) != 0) {
            send_response(client, "500 Internal Server Error", "text/plain; charset=utf-8", "Failed to enable bridge");
            return;
        }

        web_config_set_gateway_runtime_enabled(1);
        web_config_set_gateway_status(1, 0, progid,
                                      "Bridge mapping saved. Runtime enabled, waiting for next poll.");
        send_response(client, "200 OK", "text/plain; charset=utf-8",
                      "Saved selected points and enabled bridge runtime.");
        return;
    }

    if (strncmp(req, "POST /set-port", 14) == 0) {
        const char *body = strstr(req, "\r\n\r\n");
        if (!body) {
            send_response(client, "400 Bad Request", "text/plain; charset=utf-8", "Invalid request body");
            return;
        }
        body += 4;
        int port = parse_form_port(body);
        if (port <= 0 || port > 65535) {
            send_response(client, "400 Bad Request", "text/plain; charset=utf-8", "Invalid port");
            return;
        }

        if (config_set_opcua_port(g_config_path, port) != 0) {
            send_response(client, "500 Internal Server Error", "text/plain; charset=utf-8",
                          "Failed to save config");
            return;
        }

        LOG_WARN_MSG("web-config: OPCUA port updated to %d in config file", port);
        send_response(client, "200 OK", "text/plain; charset=utf-8",
                      "Port saved. Restart service to apply new OPCUA endpoint.");
        return;
    }

    send_response(client, "404 Not Found", "text/plain; charset=utf-8", "Not found");
}

static DWORD WINAPI web_thread(LPVOID arg)
{
    (void)arg;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOG_ERROR_MSG("web-config: WSAStartup failed");
        return 0;
    }

    g_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen_sock == INVALID_SOCKET) {
        LOG_ERROR_MSG("web-config: socket create failed");
        WSACleanup();
        return 0;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(18080);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(g_listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_WARN_MSG("web-config: port 18080 busy, web config disabled");
        closesocket(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
        WSACleanup();
        return 0;
    }

    if (listen(g_listen_sock, 8) != 0) {
        LOG_ERROR_MSG("web-config: listen failed");
        closesocket(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
        WSACleanup();
        return 0;
    }

    LOG_WARN_MSG("web-config: open http://127.0.0.1:18080 to configure OPCUA port");

    while (InterlockedCompareExchange(&g_running, 0, 0)) {
        fd_set readfds;
        struct timeval tv;
        FD_ZERO(&readfds);
        FD_SET(g_listen_sock, &readfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int sel = select(0, &readfds, NULL, NULL, &tv);
        if (sel <= 0) continue;

        SOCKET client = accept(g_listen_sock, NULL, NULL);
        if (client == INVALID_SOCKET) continue;
        handle_client(client);
        closesocket(client);
    }

    if (g_listen_sock != INVALID_SOCKET) {
        closesocket(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
    }
    WSACleanup();
    return 0;
}

int web_config_start(const char *config_path)
{
    if (!config_path) return -1;
    if (InterlockedCompareExchange(&g_running, 0, 0)) return 0;

    if (!g_status_lock_ready) {
        InitializeCriticalSection(&g_status_lock);
        g_status_lock_ready = 1;
    }

    snprintf(g_config_path, sizeof(g_config_path), "%s", config_path);
    AppConfig cfg;
    if (config_load(config_path, &cfg) == 0) {
        int cfg_enabled = (strcmp(cfg.opcda.mode, "opcda_com") == 0) ? 1 : 0;
        web_config_set_gateway_runtime_enabled(cfg_enabled);
        web_config_set_gateway_status(cfg_enabled, 0, cfg.opcda.server_progid,
                                      "Gateway status initializing");
        web_config_set_opcua_status(0, 0, parse_port_from_endpoint(cfg.opcua.endpoint),
                                    "OPCUA status initializing");
    } else {
        web_config_set_gateway_runtime_enabled(0);
        web_config_set_gateway_status(0, 0, "", "Gateway status initializing");
        web_config_set_opcua_status(0, 0, 0, "OPCUA status initializing");
    }
    InterlockedExchange(&g_running, 1);
    g_thread = CreateThread(NULL, 0, web_thread, NULL, 0, NULL);
    if (!g_thread) {
        InterlockedExchange(&g_running, 0);
        return -1;
    }
    return 0;
}

void web_config_stop(void)
{
    if (!InterlockedCompareExchange(&g_running, 0, 0)) return;
    InterlockedExchange(&g_running, 0);

    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }

    if (g_status_lock_ready) {
        DeleteCriticalSection(&g_status_lock);
        g_status_lock_ready = 0;
    }
}

#else
int web_config_start(const char *config_path)
{
    (void)config_path;
    return 0;
}

void web_config_stop(void)
{
}

void web_config_set_gateway_status(int enabled, int connected,
                                   const char *progid, const char *message)
{
    (void)enabled;
    (void)connected;
    (void)progid;
    (void)message;
}

void web_config_set_opcua_status(int enabled, int listening, int port, const char *message)
{
    (void)enabled;
    (void)listening;
    (void)port;
    (void)message;
}

void web_config_set_gateway_runtime_enabled(int enabled)
{
    (void)enabled;
}

int web_config_get_gateway_runtime_enabled(void)
{
    return 1;
}
#endif
