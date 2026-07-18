#pragma once
/* pairing.h — Apple device pairing flow (Pair → Trust → verify) */
#include "lockdown.h"

typedef struct {
    char *host_id;           /* UUID v4, lưu vào SharedPrefs */
    char *system_buid;       /* UUID v4 */
    char *root_cert_pem;
    char *root_key_pem;
    char *host_cert_pem;
    char *host_key_pem;
    char *device_cert_pem;
} pair_record_t;

/* Thực hiện toàn bộ pairing flow: Pair → chờ Trust → StartSession */
int  pairing_do    (lockdown_t *ld, pair_record_t *rec_out,
                    /* JNIEnv* dùng để gọi CertHelper/TlsHelper */
                    void *jni_env, void *jni_this);

/* Giải phóng pair record */
void pairing_free  (pair_record_t *rec);

/* Load/save pair record từ file (SharedPrefs qua JNI, hoặc file trực tiếp) */
int  pairing_save  (const pair_record_t *rec, const char *path);
int  pairing_load  (pair_record_t *rec, const char *path);
int  pairing_exists(const char *path);
