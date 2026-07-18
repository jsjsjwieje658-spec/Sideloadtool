#pragma once
/*
 * plist_util.h — Minimal Apple plist XML parser/builder cho Mode 2/3
 *
 * Chỉ hỗ trợ flat dict (không nested), đủ cho giao tiếp lockdownd/AFC/instproxy.
 * Dùng khi không có libplist prebuilt (Mode 3 — fully bundled).
 */
#include <stdint.h>
#include <stddef.h>

/* ── Opaque plist dict ─────────────────────────────────────────────────── */
typedef struct plist_dict plist_dict_t;

/* ── Parse / Free ──────────────────────────────────────────────────────── */
plist_dict_t *plist_parse  (const char *xml);
void          plist_free   (plist_dict_t *d);

/* ── Getters ───────────────────────────────────────────────────────────── */
/* Trả con trỏ nội bộ — KHÔNG free, hợp lệ đến khi plist_free() */
const char *plist_get_str     (const plist_dict_t *d, const char *key);
/* FIX ROOT CAUSE #2: Read <data> type values (e.g. DevicePublicKey) */
const char *plist_get_data_str(const plist_dict_t *d, const char *key);
int         plist_get_bool(const plist_dict_t *d, const char *key);
long long   plist_get_int (const plist_dict_t *d, const char *key);

/* ── Request Builders ──────────────────────────────────────────────────── */
/* Caller phải free() kết quả */
char *plist_build_start_service(const char *service_name);
char *plist_build_start_session(const char *system_buid, const char *host_id);
char *plist_build_pair_request (const char *type,
                                 const char *device_cert_pem,
                                 const char *host_cert_pem,
                                 const char *root_cert_pem,
                                 const char *host_id,
                                 const char *system_buid);
char *plist_build_install_request(const char *pkg_path);
char *plist_build_pairing_export (const char *udid,
                                   const char *host_id,
                                   const char *system_buid,
                                   const char *root_cert_pem,
                                   const char *root_key_pem,
                                   const char *host_cert_pem,
                                   const char *host_key_pem,
                                   const char *device_cert_pem);

/* ── Base64 ────────────────────────────────────────────────────────────── */
/* b64_encode: caller free() kết quả */
char  *b64_encode(const unsigned char *data, size_t len);
/* b64_decode: caller free(*out); trả 0 nếu thất bại */
size_t b64_decode(const char *b64, unsigned char **out);
