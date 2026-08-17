#pragma once
/* lockdown.h — Lockdown protocol client (plist over TCP-mux, 4-byte BE length) */
#include "usbmux.h"
#include "plist_util.h"

typedef struct {
    mux_conn_t *mux;
    /* TLS state (sau StartSession) */
    int         tls_active;
    /* con trỏ lưu SSLEngine reference, set bởi TlsHelper.handshake() */
    long        tls_jptr;
    /* host cert PEM (dùng cho TLS) */
    char       *host_cert_pem;
    char       *host_key_pem;
} lockdown_t;

int  lockdown_open    (lockdown_t *ld, mux_conn_t *mux);
int  lockdown_exchange(lockdown_t *ld, const char *req_xml,
                        plist_dict_t **resp_out);
int  lockdown_get_value(lockdown_t *ld, const char *domain,
                         const char *key, char **val_out);
int  lockdown_start_service(lockdown_t *ld, const char *service,
                              int *port_out, int *ssl_out);
int  lockdown_start_tls(lockdown_t *ld);
void lockdown_close   (lockdown_t *ld);
