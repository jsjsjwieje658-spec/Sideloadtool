/*
 * plist_util.c — Minimal Apple plist XML parser/builder (Mode 2/3 fallback)
 *
 * BUGFIX CRITICAL: File này trước đây RỖNG (0 bytes) — tất cả hàm plist,
 * base64 đều thiếu implementation → Mode 2 và Mode 3 không thể link/chạy.
 *
 * Parser: dùng string-search đơn giản — đủ cho flat dict plist của Apple.
 * Không dùng bất kỳ thư viện XML nào để tránh dependency.
 */
#include "plist_util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <android/log.h>

#define TAG "plist_util"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* ════════════════════════════════════════════════════════════════════════
 * Base64
 * ════════════════════════════════════════════════════════════════════════ */

static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *b64_encode(const unsigned char *data, size_t len) {
    if (!data || len == 0) {
        char *empty = malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    size_t out_len = 4 * ((len + 2) / 3) + 1;
    char *out = malloc(out_len);
    if (!out) return NULL;

    size_t i = 0, o = 0;
    while (i + 2 < len) {
        uint32_t v = ((uint32_t)data[i] << 16) |
                     ((uint32_t)data[i+1] << 8) |
                      (uint32_t)data[i+2];
        out[o++] = B64_CHARS[(v >> 18) & 0x3F];
        out[o++] = B64_CHARS[(v >> 12) & 0x3F];
        out[o++] = B64_CHARS[(v >>  6) & 0x3F];
        out[o++] = B64_CHARS[(v >>  0) & 0x3F];
        i += 3;
    }
    if (i + 1 == len) {
        uint32_t v = (uint32_t)data[i] << 16;
        out[o++] = B64_CHARS[(v >> 18) & 0x3F];
        out[o++] = B64_CHARS[(v >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (i + 2 == len) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i+1] << 8);
        out[o++] = B64_CHARS[(v >> 18) & 0x3F];
        out[o++] = B64_CHARS[(v >> 12) & 0x3F];
        out[o++] = B64_CHARS[(v >>  6) & 0x3F];
        out[o++] = '=';
    }
    out[o] = '\0';
    return out;
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

size_t b64_decode(const char *b64, unsigned char **out) {
    if (!b64 || !out) return 0;
    size_t in_len = strlen(b64);
    /* skip whitespace / newlines to find real length */
    size_t real_len = 0;
    for (size_t i = 0; i < in_len; i++) {
        if (!isspace((unsigned char)b64[i])) real_len++;
    }
    if (real_len == 0) {
        *out = (unsigned char *)calloc(1, 1);
        return 0;
    }
    size_t max_out = (real_len / 4) * 3 + 4;
    unsigned char *buf = malloc(max_out + 1);
    if (!buf) { *out = NULL; return 0; }

    size_t o = 0;
    int acc = 0, bits = 0;
    for (size_t i = 0; i < in_len; i++) {
        char c = b64[i];
        if (isspace((unsigned char)c)) continue;
        if (c == '=') break;
        int v = b64_val(c);
        if (v < 0) continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            buf[o++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    buf[o] = '\0';  /* null-terminate for string use */
    *out = buf;
    return o;
}

/* ════════════════════════════════════════════════════════════════════════
 * Plist dict: flat key-value store
 * ════════════════════════════════════════════════════════════════════════ */

#define PLIST_MAX_ENTRIES 64

typedef enum { PTYPE_STR, PTYPE_BOOL, PTYPE_INT, PTYPE_DATA } plist_type_t;

typedef struct {
    char        *key;
    plist_type_t type;
    char        *str_val;   /* for PTYPE_STR / PTYPE_DATA */
    int          bool_val;  /* for PTYPE_BOOL */
    long long    int_val;   /* for PTYPE_INT */
} plist_entry_t;

struct plist_dict {
    plist_entry_t entries[PLIST_MAX_ENTRIES];
    int           count;
};

static char *xml_extract_tag_content(const char *xml, const char *open_tag,
                                      const char *close_tag) {
    const char *p = strstr(xml, open_tag);
    if (!p) return NULL;
    p += strlen(open_tag);
    const char *e = strstr(p, close_tag);
    if (!e) return NULL;
    size_t len = (size_t)(e - p);
    char *val = malloc(len + 1);
    if (!val) return NULL;
    memcpy(val, p, len);
    val[len] = '\0';
    return val;
}

/* Unescape basic XML entities */
static char *xml_unescape(char *s) {
    if (!s) return s;
    char *src = s, *dst = s;
    while (*src) {
        if (*src == '&') {
            if (strncmp(src, "&amp;",  5) == 0) { *dst++ = '&';  src += 5; }
            else if (strncmp(src, "&lt;",  4) == 0) { *dst++ = '<';  src += 4; }
            else if (strncmp(src, "&gt;",  4) == 0) { *dst++ = '>';  src += 4; }
            else if (strncmp(src, "&apos;",6) == 0) { *dst++ = '\''; src += 6; }
            else if (strncmp(src, "&quot;",6) == 0) { *dst++ = '"';  src += 6; }
            else { *dst++ = *src++; }
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    return s;
}

plist_dict_t *plist_parse(const char *xml) {
    if (!xml) return NULL;
    plist_dict_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;

    const char *p = xml;
    while (d->count < PLIST_MAX_ENTRIES) {
        /* Find next <key>...</key> */
        const char *key_start = strstr(p, "<key>");
        if (!key_start) break;
        key_start += 5;
        const char *key_end = strstr(key_start, "</key>");
        if (!key_end) break;

        size_t klen = (size_t)(key_end - key_start);
        char *key = malloc(klen + 1);
        if (!key) break;
        memcpy(key, key_start, klen);
        key[klen] = '\0';

        /* Skip past </key> */
        const char *after_key = key_end + 6;

        /* Skip whitespace */
        while (*after_key && isspace((unsigned char)*after_key)) after_key++;

        plist_entry_t *e = &d->entries[d->count];
        e->key = key;

        if (strncmp(after_key, "<string>", 8) == 0) {
            e->type = PTYPE_STR;
            char *v = xml_extract_tag_content(after_key, "<string>", "</string>");
            e->str_val = v ? xml_unescape(v) : strdup("");
            p = strstr(after_key, "</string>") + 9;
        } else if (strncmp(after_key, "<integer>", 9) == 0) {
            e->type = PTYPE_INT;
            char *v = xml_extract_tag_content(after_key, "<integer>", "</integer>");
            if (v) { e->int_val = atoll(v); free(v); }
            p = strstr(after_key, "</integer>") + 10;
        } else if (strncmp(after_key, "<true/>", 7) == 0 ||
                   strncmp(after_key, "<true />",8) == 0) {
            e->type = PTYPE_BOOL;
            e->bool_val = 1;
            p = after_key + 7;
        } else if (strncmp(after_key, "<false/>",8) == 0 ||
                   strncmp(after_key, "<false />",9) == 0) {
            e->type = PTYPE_BOOL;
            e->bool_val = 0;
            p = after_key + 8;
        } else if (strncmp(after_key, "<data>", 6) == 0) {
            e->type = PTYPE_DATA;
            char *v = xml_extract_tag_content(after_key, "<data>", "</data>");
            e->str_val = v ? v : strdup("");
            p = strstr(after_key, "</data>") + 7;
        } else {
            /* Unknown tag — skip to next </key> context */
            free(key);
            p = after_key;
            continue;
        }

        d->count++;
        if (!p || !*p) break;
    }
    return d;
}

void plist_free(plist_dict_t *d) {
    if (!d) return;
    for (int i = 0; i < d->count; i++) {
        free(d->entries[i].key);
        free(d->entries[i].str_val);
    }
    free(d);
}

const char *plist_get_str(const plist_dict_t *d, const char *key) {
    if (!d || !key) return NULL;
    for (int i = 0; i < d->count; i++) {
        if (d->entries[i].type == PTYPE_STR &&
            strcmp(d->entries[i].key, key) == 0)
            return d->entries[i].str_val;
    }
    return NULL;
}

/*
 * FIX ROOT CAUSE #2 (CRITICAL): plist_get_data_str()
 *
 * Apple lockdownd trả DevicePublicKey dưới dạng <data>BASE64</data>, KHÔNG
 * phải <string>. Hàm plist_get_str() chỉ match PTYPE_STR nên LUÔN trả NULL
 * cho trường này. Kết quả: pairing_do() gọi lockdown_get_value("DevicePublicKey")
 * → nhận NULL → không thể tạo cert chain → pairing LUÔN thất bại.
 *
 * Hàm này trả base64 thô (chưa decode) của trường <data>. Caller tự decode.
 * Con trỏ hợp lệ đến khi plist_free() được gọi.
 */
const char *plist_get_data_str(const plist_dict_t *d, const char *key) {
    if (!d || !key) return NULL;
    for (int i = 0; i < d->count; i++) {
        if (d->entries[i].type == PTYPE_DATA &&
            strcmp(d->entries[i].key, key) == 0)
            return d->entries[i].str_val;
    }
    return NULL;
}

int plist_get_bool(const plist_dict_t *d, const char *key) {
    if (!d || !key) return 0;
    for (int i = 0; i < d->count; i++) {
        if (strcmp(d->entries[i].key, key) == 0) {
            if (d->entries[i].type == PTYPE_BOOL) return d->entries[i].bool_val;
            if (d->entries[i].type == PTYPE_INT)  return d->entries[i].int_val != 0;
            if (d->entries[i].type == PTYPE_STR && d->entries[i].str_val)
                return strcmp(d->entries[i].str_val, "true") == 0;
        }
    }
    return 0;
}

long long plist_get_int(const plist_dict_t *d, const char *key) {
    if (!d || !key) return 0;
    for (int i = 0; i < d->count; i++) {
        if (strcmp(d->entries[i].key, key) == 0) {
            if (d->entries[i].type == PTYPE_INT) return d->entries[i].int_val;
            if (d->entries[i].type == PTYPE_STR && d->entries[i].str_val)
                return atoll(d->entries[i].str_val);
        }
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════
 * XML Plist header/footer helpers
 * ════════════════════════════════════════════════════════════════════════ */

#define PLIST_HEADER \
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" \
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\"" \
    " \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n" \
    "<plist version=\"1.0\"><dict>\n"
#define PLIST_FOOTER "</dict></plist>"

/* PEM → base64 (strip headers + newlines, re-encode) */
static char *pem_to_data_b64(const char *pem) {
    if (!pem || !*pem) return strdup("");
    /* Strip PEM header/footer lines, keep only base64 body */
    const char *p = pem;
    /* Skip past first "-----BEGIN..." line */
    const char *body_start = strchr(p, '\n');
    if (body_start) body_start++;
    else body_start = p;

    /* Find "-----END..." line */
    const char *end_tag = strstr(body_start, "-----END");
    size_t body_len = end_tag ? (size_t)(end_tag - body_start) : strlen(body_start);

    /* Collect non-whitespace chars */
    char *clean = malloc(body_len + 1);
    if (!clean) return strdup("");
    size_t j = 0;
    for (size_t i = 0; i < body_len; i++) {
        if (!isspace((unsigned char)body_start[i]))
            clean[j++] = body_start[i];
    }
    clean[j] = '\0';

    /* Decode back to DER, then re-encode as single-line base64 */
    unsigned char *der = NULL;
    size_t der_len = b64_decode(clean, &der);
    free(clean);
    if (!der || der_len == 0) { free(der); return strdup(""); }
    char *out = b64_encode(der, der_len);
    free(der);
    return out ? out : strdup("");
}

/* ════════════════════════════════════════════════════════════════════════
 * Request builders
 * ════════════════════════════════════════════════════════════════════════ */

char *plist_build_start_service(const char *service_name) {
    char *out = NULL;
    int r = asprintf(&out,
        PLIST_HEADER
        "<key>Label</key><string>SideloadAndroid</string>\n"
        "<key>Request</key><string>StartService</string>\n"
        "<key>Service</key><string>%s</string>\n"
        PLIST_FOOTER, service_name);
    return r >= 0 ? out : NULL;
}

char *plist_build_start_session(const char *system_buid, const char *host_id) {
    char *out = NULL;
    int r = asprintf(&out,
        PLIST_HEADER
        "<key>Label</key><string>SideloadAndroid</string>\n"
        "<key>Request</key><string>StartSession</string>\n"
        "<key>SystemBUID</key><string>%s</string>\n"
        "<key>HostID</key><string>%s</string>\n"
        PLIST_FOOTER,
        system_buid ? system_buid : "00000000-0000-0000-0000-000000000000",
        host_id     ? host_id     : "");
    return r >= 0 ? out : NULL;
}

char *plist_build_pair_request(const char *type,
                                const char *device_cert_pem,
                                const char *host_cert_pem,
                                const char *root_cert_pem,
                                const char *host_id,
                                const char *system_buid) {
    /* Convert PEM → raw DER base64 for Apple plist <data> tags */
    char *dev_b64  = pem_to_data_b64(device_cert_pem);
    char *host_b64 = pem_to_data_b64(host_cert_pem);
    char *root_b64 = pem_to_data_b64(root_cert_pem);

    char *out = NULL;
    int r = asprintf(&out,
        PLIST_HEADER
        "<key>Label</key><string>SideloadAndroid</string>\n"
        "<key>Request</key><string>%s</string>\n"
        "<key>PairRecord</key><dict>\n"
        "  <key>DeviceCertificate</key><data>%s</data>\n"
        "  <key>HostCertificate</key><data>%s</data>\n"
        "  <key>HostID</key><string>%s</string>\n"
        "  <key>RootCertificate</key><data>%s</data>\n"
        "  <key>SystemBUID</key><string>%s</string>\n"
        "</dict>\n"
        "<key>ProtocolVersion</key><string>2</string>\n"
        PLIST_FOOTER,
        type ? type : "Pair",
        dev_b64  ? dev_b64  : "",
        host_b64 ? host_b64 : "",
        host_id  ? host_id  : "",
        root_b64 ? root_b64 : "",
        system_buid ? system_buid : "00000000-0000-0000-0000-000000000000");

    free(dev_b64); free(host_b64); free(root_b64);
    return r >= 0 ? out : NULL;
}

char *plist_build_install_request(const char *pkg_path) {
    char *out = NULL;
    int r = asprintf(&out,
        PLIST_HEADER
        "<key>Command</key><string>Install</string>\n"
        "<key>PackagePath</key><string>%s</string>\n"
        "<key>ClientOptions</key><dict>\n"
        "  <key>PackageType</key><string>Developer</string>\n"
        "</dict>\n"
        PLIST_FOOTER,
        pkg_path ? pkg_path : "");
    return r >= 0 ? out : NULL;
}

char *plist_build_pairing_export(const char *udid,
                                  const char *host_id,
                                  const char *system_buid,
                                  const char *root_cert_pem,
                                  const char *root_key_pem,
                                  const char *host_cert_pem,
                                  const char *host_key_pem,
                                  const char *device_cert_pem) {
    char *rc_b64 = pem_to_data_b64(root_cert_pem);
    char *rk_b64 = pem_to_data_b64(root_key_pem);
    char *hc_b64 = pem_to_data_b64(host_cert_pem);
    char *hk_b64 = pem_to_data_b64(host_key_pem);
    char *dc_b64 = pem_to_data_b64(device_cert_pem);

    char *out = NULL;
    asprintf(&out,
        PLIST_HEADER
        "<key>DeviceCertificate</key><data>%s</data>\n"
        "<key>HostCertificate</key><data>%s</data>\n"
        "<key>HostID</key><string>%s</string>\n"
        "<key>HostPrivateKey</key><data>%s</data>\n"
        "<key>RootCertificate</key><data>%s</data>\n"
        "<key>RootPrivateKey</key><data>%s</data>\n"
        "<key>SystemBUID</key><string>%s</string>\n"
        "<key>UDID</key><string>%s</string>\n"
        PLIST_FOOTER,
        dc_b64 ? dc_b64 : "",
        hc_b64 ? hc_b64 : "",
        host_id ? host_id : "",
        hk_b64 ? hk_b64 : "",
        rc_b64 ? rc_b64 : "",
        rk_b64 ? rk_b64 : "",
        system_buid ? system_buid : "00000000-0000-0000-0000-000000000000",
        udid ? udid : "unknown");

    free(rc_b64); free(rk_b64); free(hc_b64); free(hk_b64); free(dc_b64);
    return out;
}
