/*
 * uuid_compat.h — UUID generation cho Android NDK (không có libuuid).
 *
 * Android NDK không cung cấp libuuid — bionic có /dev/urandom để tạo UUID
 * ngẫu nhiên. Macro uuid_generate() ở đây tạo UUID v4 (random) đúng chuẩn
 * RFC 4122 theo cách libimobiledevice/libplist dùng (128-bit raw bytes).
 *
 * Học từ termux-usbmuxd: không cần cài thêm package nào — chỉ dùng
 * /dev/urandom sẵn có trên Android/Linux giống như termux làm.
 */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

/* UUID v4: 16 raw bytes (128 bit) */
typedef unsigned char uuid_t[16];

/*
 * uuid_generate_random: Đọc 16 byte từ /dev/urandom, set version=4 và
 * variant=RFC4122, giống uuid_generate() của libuuid.
 */
static inline void uuid_generate_random(uuid_t out) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        read(fd, out, 16);
        close(fd);
    } else {
        /* fallback: dùng địa chỉ stack (yếu nhưng không crash) */
        memset(out, 0, 16);
        out[0] = (unsigned char)(uintptr_t)out;
        out[1] = (unsigned char)((uintptr_t)out >> 8);
    }
    /* Set version 4 (random) */
    out[6] = (out[6] & 0x0F) | 0x40;
    /* Set variant RFC 4122 */
    out[8] = (out[8] & 0x3F) | 0x80;
}

/* Alias để tương thích với code gọi uuid_generate() */
#define uuid_generate uuid_generate_random

/*
 * uuid_unparse_upper: Chuyển 16 byte thành chuỗi UUID hoa (37 byte kể \0).
 * Ví dụ: "550E8400-E29B-41D4-A716-446655440000"
 */
static inline void uuid_unparse_upper(const uuid_t uu, char out[37]) {
    snprintf(out, 37,
        "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-"
        "%02X%02X%02X%02X%02X%02X",
        uu[0], uu[1], uu[2],  uu[3],
        uu[4], uu[5],
        uu[6], uu[7],
        uu[8], uu[9],
        uu[10], uu[11], uu[12], uu[13], uu[14], uu[15]);
}

static inline void uuid_unparse_lower(const uuid_t uu, char out[37]) {
    snprintf(out, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
        "%02x%02x%02x%02x%02x%02x",
        uu[0], uu[1], uu[2],  uu[3],
        uu[4], uu[5],
        uu[6], uu[7],
        uu[8], uu[9],
        uu[10], uu[11], uu[12], uu[13], uu[14], uu[15]);
}

/* Mặc định dùng uppercase (giống Apple style) */
#define uuid_unparse uuid_unparse_upper
