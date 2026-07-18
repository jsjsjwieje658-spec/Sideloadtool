# Sideloadtool — Bug Fixes (Trust Popup / iPhone Communication)

All bugs below were identified by deep analysis of:
- `Sideloadtool` (jni_bridge.c, plist_util.c/h, lockdown.c, usb_fd_bridge.c, usbmuxd_server.c, UsbTransport.kt)
- `termux-usbmuxd` (reference implementation)
- `termux-api` (UsbAPI.java — the correct fd-passing pattern)

---

## BUG 1 — CRITICAL · `jni_bridge.c` · `usb_bulk_write`
**Missing `AttachCurrentThread` for native threads**

`usb_bulk_write` is called from usbmuxd_server's relay threads (pthreads, not
the JVM main thread). `GetEnv()` on an unattached thread returns `JNI_EDETACHED`,
making `env = NULL`. The function then returns `-1` silently. **Every USB write
from a native thread fails** — nothing is ever sent to the iPhone.

**Fix:** Added `AttachCurrentThread` / `DetachCurrentThread` guard around the
`GetEnv()` call in `usb_bulk_write`.

---

## BUG 2 — CRITICAL · `jni_bridge.c` · `usb_bulk_read`
**Same `AttachCurrentThread` problem on the read path**

Same root cause as Bug 1: `usb_bulk_read` also returns `-1` for every read from a
native thread → iPhone response packets are never received → lockdown handshake
hangs → pairing request is never sent → Trust popup never appears.

**Fix:** Added `AttachCurrentThread` / `DetachCurrentThread` in `usb_bulk_read`.

---

## BUG 3 — CRITICAL · `usb_fd_bridge.c` · `libusb_reset_device()`
**USB bus reset invalidates the Android fd**

`libusb_reset_device()` sends a USB bus reset. The iPhone then re-enumerates as
a **new** USB device. The fd obtained from `UsbDeviceConnection` is now stale —
it references the old device descriptor. Re-calling `libusb_wrap_sys_device()`
with the same stale fd yields a broken libusb handle. All subsequent bulk
transfers fail silently. The reference implementation (`termux-usbmuxd` /
`UsbAPI.java`) never calls `reset_device`; it relies solely on `clear_halt`.

**Fix:** Removed `libusb_reset_device()` entirely. The proactive
`libusb_clear_halt()` block (already present after `discover_apple_endpoints()`)
is sufficient.

---

## BUG 4 — CRITICAL · `jni_bridge.c` · `jni_log_ui_cb`
**Log callback missing `AttachCurrentThread`**

`jni_log_ui_cb` has the same `GetEnv()` problem. Every log message from C
threads is silently dropped. Non-fatal by itself but means the user sees zero
progress messages from native code.

**Fix:** Added `AttachCurrentThread` / `DetachCurrentThread` in `jni_log_ui_cb`.

---

## BUG 5 — CRITICAL · `plist_util.c` + `plist_util.h` · `plist_get_str()` ignores `<data>` type
**`DevicePublicKey` is always NULL → pairing always fails**

`lockdown_get_value()` calls `plist_get_str(resp, "Value")` to read the
`DevicePublicKey` field. But Apple's lockdownd returns `DevicePublicKey` (and
all certificates) as `<data>` tags, which the plist parser stores as
`PTYPE_DATA`. `plist_get_str()` only matches `PTYPE_STR` → always returns NULL
for `DevicePublicKey` → `pairing_do()` fails immediately → the Pair request is
never sent to the iPhone → **Trust popup never appears**.

**Fix:**
- Added `plist_get_data()` function (returns `str_val` for `PTYPE_DATA` entries)
- Added declaration to `plist_util.h`
- `lockdown_get_value()` now falls back to `plist_get_data()` when `plist_get_str()` returns NULL

---

## BUG 6 — CRITICAL · `lockdown.c` · `lockdown_get_value()` doesn't read `<data>` type
**Companion bug to Bug 5 — the fix point in `lockdown.c`**

`lockdown_get_value()` only called `plist_get_str()`, so even with a correct
plist parser, binary-typed values (DevicePublicKey, certificates) were always
missed.

**Fix:** After `plist_get_str()` returns NULL, try `plist_get_data(resp, "Value")`.

---

## BUG 7 — HIGH · `usbmuxd_server.c` · TCP window size = 512 bytes
**Lockdownd TLS certificate exchange stalls**

`thdr->window = htons(0x0200)` advertises only 512 bytes of receive buffer.
The lockdownd TLS handshake certificate payload routinely exceeds 512 bytes.
When the iPhone sees window=512 it must fragment its response, which can stall
the connection before the Trust prompt is triggered.

**Fix:** Changed to `htons(0xFFFF)` — the maximum 16-bit TCP window (65535 bytes).

---

## BUG 8 — HIGH · `usbmuxd_server.c` · Missing `free(xml)` in Listen handler
**Memory leak on every device list poll**

The `Listen` branch in `handle_client()` was missing `free(xml)`. Every
connection to `usbmuxd_get_device_list()` leaked the entire plist request body.
Non-fatal in isolation but accumulates across reconnects and retries.

**Fix:** Added `free(xml)` after processing the Listen request.

---

## BUG 9 — HIGH · `UsbTransport.kt` · Missing `setConfiguration()` before `claimInterface()`
**Interface claim fails on some Android OEM devices**

On some MediaTek and Samsung devices the USB configuration is not activated when
`openDevice()` returns. Calling `claimInterface()` without first calling
`setConfiguration()` returns `false` silently → Mode 2/3 bulk transfers are
never set up.

**Fix:** Added `conn.setConfiguration(found.config)` before the
`claimInterface()` retry loop in `prepareForBulkTransfers()`.

---

## Files Changed

| File | Bugs Fixed |
|---|---|
| `app/src/main/cpp/jni_bridge.c` | Bug 1, Bug 2, Bug 4 |
| `app/src/main/cpp/plist_util.h` | Bug 5 (declaration) |
| `app/src/main/cpp/plist_util.c` | Bug 5 (implementation) |
| `app/src/main/cpp/lockdown.c` | Bug 6 |
| `app/src/main/cpp/usb_fd_bridge.c` | Bug 3 |
| `app/src/main/cpp/usbmuxd_server.c` | Bug 7, Bug 8 |
| `app/src/main/java/com/superalpha/sideload/bridge/UsbTransport.kt` | Bug 9 |
