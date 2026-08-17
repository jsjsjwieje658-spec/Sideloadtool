#pragma once
/* install_proxy.h — com.apple.mobile.installation_proxy protocol */
#include "usbmux.h"
#include "plist_util.h"

typedef struct {
    mux_conn_t *mux;
} install_proxy_t;

int  install_proxy_open   (install_proxy_t *ip, mux_conn_t *mux);
int  install_proxy_install(install_proxy_t *ip, const char *pkg_path,
                            void (*progress_cb)(const char *status, int pct));
void install_proxy_close  (install_proxy_t *ip);
