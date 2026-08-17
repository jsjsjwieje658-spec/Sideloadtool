/*
 * afc.c — AFC (Apple File Conduit) protocol client.
 * Tham chiếu: libimobiledevice/src/afc.c
 * AFC dùng LITTLE-ENDIAN (ngược lại với mux là big-endian).
 */
#include "afc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <android/log.h>
#include <endian.h>

#define TAG "afc"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define AFC_READ_BUF  65536

/* ── Internal helpers ────────────────────────────────────────────────────── */

static int afc_send_pkt(afc_t *a, afc_op_t op,
                         const void *payload, uint64_t payload_len,
                         const void *data,    uint64_t data_len) {
    afc_header_t hdr;
    memcpy(hdr.magic, AFC_MAGIC, 8);
    hdr.entire_length = htole64(sizeof(hdr) + payload_len + data_len);
    hdr.this_length   = htole64(sizeof(hdr) + payload_len);
    hdr.packet_num    = htole64(a->next_pkt++);
    hdr.operation     = htole64((uint64_t)op);

    if (mux_send(a->mux, &hdr, sizeof(hdr)) < 0) return -1;
    if (payload && payload_len > 0)
        if (mux_send(a->mux, payload, (int)payload_len) < 0) return -1;
    if (data && data_len > 0)
        if (mux_send(a->mux, data, (int)data_len) < 0) return -1;
    return 0;
}

static int afc_recv_pkt(afc_t *a, afc_header_t *hdr_out,
                         void **payload_out, uint64_t *payload_len_out) {
    afc_header_t hdr;
    if (mux_recv_exact(a->mux, &hdr, sizeof(hdr)) < 0) return -1;
    memcpy(hdr_out, &hdr, sizeof(hdr));

    uint64_t entire  = le64toh(hdr.entire_length);
    uint64_t this_l  = le64toh(hdr.this_length);
    uint64_t payload = this_l - sizeof(hdr);

    if (payload_out && payload > 0) {
        *payload_out = malloc(payload + 1);
        if (mux_recv_exact(a->mux, *payload_out, (int)payload) < 0) {
            free(*payload_out); *payload_out = NULL; return -1;
        }
        ((char*)*payload_out)[payload] = '\0';
        if (payload_len_out) *payload_len_out = payload;
    } else {
        if (payload_out) *payload_out = NULL;
        if (payload_len_out) *payload_len_out = 0;
    }

    /* BUGFIX v11: Luôn drain extra bytes (entire_length > this_length), bất kể
     * payload_out có NULL hay không.  Nếu không drain, các bytes thừa sẽ nằm lại
     * trong USB buffer và làm hỏng packet tiếp theo (gây ra lỗi "unexpected op"). */
    uint64_t extra = entire - this_l;
    if (extra > 0) {
        char tmp[1024];
        while (extra > 0) {
            int n = (int)(extra < sizeof(tmp) ? extra : sizeof(tmp));
            if (mux_recv_exact(a->mux, tmp, n) < 0) return -1;
            extra -= n;
        }
    }
    return 0;
}

static int afc_check_status(afc_t *a) {
    afc_header_t hdr;
    void *payload = NULL;
    uint64_t plen = 0;
    if (afc_recv_pkt(a, &hdr, &payload, &plen) < 0) return -1;
    afc_op_t op = (afc_op_t)le64toh(hdr.operation);
    if (op != AFC_OP_STATUS) {
        LOGE("afc_check_status: unexpected op 0x%llx", (unsigned long long)op);
        free(payload);
        return -1;
    }
    uint64_t status = payload && plen >= 8 ? le64toh(*(uint64_t*)payload) : 1;
    free(payload);
    if (status != 0) {
        LOGE("afc status error: %llu", (unsigned long long)status);
        return -1;
    }
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int afc_open(afc_t *a, mux_conn_t *mux) {
    memset(a, 0, sizeof(*a));
    a->mux     = mux;
    a->next_pkt = 0;
    return 0;
}

int afc_mkdir(afc_t *a, const char *path) {
    size_t plen = strlen(path) + 1;
    if (afc_send_pkt(a, AFC_OP_MAKE_DIR, path, plen, NULL, 0) < 0) return -1;
    return afc_check_status(a);
}

int afc_push_file(afc_t *a, const char *local_path, const char *remote_path,
                  void (*progress)(int)) {
    FILE *f = fopen(local_path, "rb");
    if (!f) { LOGE("afc_push_file: fopen(%s) thất bại", local_path); return -1; }

    /* AFC_OP_FILE_OPEN: payload = [uint64_t mode, path\0] */
    size_t path_len = strlen(remote_path) + 1;
    size_t open_payload_len = 8 + path_len;
    char *open_payload = calloc(1, open_payload_len);
    uint64_t mode_le = htole64(AFC_FOPEN_WRONLY);
    memcpy(open_payload, &mode_le, 8);
    memcpy(open_payload + 8, remote_path, path_len);

    if (afc_send_pkt(a, AFC_OP_FILE_OPEN, open_payload, open_payload_len, NULL, 0) < 0) {
        free(open_payload); fclose(f); return -1;
    }
    free(open_payload);

    /* Đọc file handle từ response */
    afc_header_t hdr;
    void *resp_payload = NULL;
    uint64_t resp_len = 0;
    if (afc_recv_pkt(a, &hdr, &resp_payload, &resp_len) < 0) {
        fclose(f); return -1;
    }
    afc_op_t op = (afc_op_t)le64toh(hdr.operation);
    if (op != AFC_OP_DATA || resp_len < 8) {
        LOGE("afc_push_file: FILE_OPEN response op=0x%llx len=%llu",
             (unsigned long long)op, (unsigned long long)resp_len);
        free(resp_payload); fclose(f); return -1;
    }
    uint64_t file_handle = le64toh(*(uint64_t*)resp_payload);
    free(resp_payload);
    LOGI("afc_push_file: file handle=0x%llx", (unsigned long long)file_handle);

    /* Lấy kích thước file */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Ghi từng chunk */
    char *buf = malloc(AFC_READ_BUF + 8);
    long sent = 0;
    int err = 0;
    while (sent < file_size && !err) {
        size_t n = fread(buf + 8, 1, AFC_READ_BUF, f);
        if (n == 0) break;
        /* payload = [uint64_t file_handle, data] */
        uint64_t fh_le = htole64(file_handle);
        memcpy(buf, &fh_le, 8);
        if (afc_send_pkt(a, AFC_OP_FILE_WRITE, buf, 8 + n, NULL, 0) < 0) {
            err = 1; break;
        }
        if (afc_check_status(a) < 0) { err = 1; break; }
        sent += (long)n;
        if (progress && file_size > 0)
            progress((int)(sent * 100 / file_size));
    }
    free(buf);
    fclose(f);

    /* AFC_OP_FILE_CLOSE */
    char close_payload[8];
    uint64_t fh_le2 = htole64(file_handle);
    memcpy(close_payload, &fh_le2, 8);
    afc_send_pkt(a, AFC_OP_FILE_CLOSE, close_payload, 8, NULL, 0);
    afc_check_status(a);

    if (err) { LOGE("afc_push_file thất bại"); return -1; }
    LOGI("afc_push_file: ✅ đẩy %s (%ld bytes) lên %s", local_path, file_size, remote_path);
    return 0;
}

void afc_close(afc_t *a) {
    memset(a, 0, sizeof(*a));
}
