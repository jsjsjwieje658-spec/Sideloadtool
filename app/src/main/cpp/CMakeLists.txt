cmake_minimum_required(VERSION 3.22)
project(sideloadnative C)

# =============================================================================
#  Ba chế độ hoạt động (tự động phát hiện theo prebuilt/)
#
#  Mode 1 — libimobiledevice thật  (sau khi scripts/build_all.sh chạy xong)
#    Sources: jni_bridge_imd.c  usb_fd_bridge.c  usbmuxd_server.c
#    Static links: libimobiledevice-1.0.a  libusbmuxd-2.0.a  libplist-2.0.a
#                  libimobiledevice-glue-1.0.a  libusb-1.0.a  libssl.a  libcrypto.a
#
#    Luồng Mode 1:
#      Java: UsbTransport.open() → claimInterface() → getFileDescriptor()
#      JNI:  nativeSetUsbFd(fd) → libusb_wrap_sys_device() + discover_endpoints()
#                               → usbmuxd_server_start() → server thread listen
#      JNI:  nativeConnect() → idevice_new_with_options()
#                             → libusbmuxd → USBMUXD_SOCKET_ADDRESS
#                             → server: ListDevices + Connect(62078)
#                             → mux_do_setup() + mux_connect() qua libusb
#                             → tunnel (socket ↔ USB)
#
#  Mode 2 — Custom protocol layer  (chỉ có libplist, hoặc không có gì)
#    Sources: jni_bridge.c  usbmux.c  lockdown.c  pairing.c  afc.c  install_proxy.c
#    Links:   libplist-2.0.a (nếu có)
#
#  Mode 3 — Fully bundled fallback (không cần build gì)
#    Sources: giống Mode 2 nhưng không link libplist, dùng plist_util.c
# =============================================================================

set(PREBUILT_DIR "${CMAKE_SOURCE_DIR}/prebuilt/${ANDROID_ABI}")
set(PREBUILT_LIB "${PREBUILT_DIR}/lib")
set(PREBUILT_INC "${PREBUILT_DIR}/include")

# Override bằng -DPREBUILT_ROOT nếu được truyền từ build.gradle.kts
if(DEFINED PREBUILT_ROOT)
  set(PREBUILT_DIR "${PREBUILT_ROOT}/${ANDROID_ABI}")
  set(PREBUILT_LIB "${PREBUILT_DIR}/lib")
  set(PREBUILT_INC "${PREBUILT_DIR}/include")
endif()

# ── Phát hiện mode ────────────────────────────────────────────────────────────
if(EXISTS "${PREBUILT_LIB}/libimobiledevice-1.0.a")
  set(BUILD_MODE 1)
  message(STATUS "🟢 Mode 1: libimobiledevice thật + usbmuxd server nội bộ")
elseif(EXISTS "${PREBUILT_LIB}/libplist-2.0.a")
  set(BUILD_MODE 2)
  message(STATUS "🟡 Mode 2: Custom protocol + libplist parser")
else()
  set(BUILD_MODE 3)
  message(STATUS "🔴 Mode 3: Fully bundled fallback — chạy scripts/build_all.sh để dùng Mode 1")
endif()

# =============================================================================
#  Mode 1 — libimobiledevice thật + usbmuxd server
# =============================================================================
if(BUILD_MODE EQUAL 1)

  add_library(sideloadnative SHARED
    jni_bridge_imd.c    # JNI API: nativeInit/Connect/Pair/Sideload (libimobiledevice)
    usb_fd_bridge.c     # Android USB fd → libusb handle + endpoint discovery
    usbmuxd_server.c    # Mini usbmuxd Unix socket server (cho libusbmuxd kết nối)
    usbmux.c            # usbmux TCP-over-USB protocol (dùng bởi usbmuxd_server)
  )

  # Import static libraries (thứ tự: cao → thấp level)
  foreach(LIB IN ITEMS
    imobiledevice-1.0
    usbmuxd-2.0
    imobiledevice-glue-1.0
    plist-2.0
    usb-1.0
    ssl
    crypto
  )
    add_library(lib${LIB} STATIC IMPORTED)
    set_target_properties(lib${LIB} PROPERTIES
      IMPORTED_LOCATION "${PREBUILT_LIB}/lib${LIB}.a"
    )
  endforeach()

  # NOTE: libusb cài header vào include/libusb-1.0/libusb.h
  # Thiếu "${PREBUILT_INC}/libusb-1.0" → "fatal error: 'libusb.h' file not found"
  target_include_directories(sideloadnative PRIVATE
    "${PREBUILT_INC}"
    "${PREBUILT_INC}/libimobiledevice"
    "${PREBUILT_INC}/libimobiledevice-glue"
    "${PREBUILT_INC}/libplist"
    "${PREBUILT_INC}/libusb-1.0"
  )

  target_compile_definitions(sideloadnative PRIVATE
    HAVE_LIBIMOBILEDEVICE=1
  )

  target_link_libraries(sideloadnative
    # libimobiledevice stack (thứ tự quan trọng: high-level → low-level)
    libimobiledevice-1.0
    libusbmuxd-2.0
    libimobiledevice-glue-1.0
    libplist-2.0
    libusb-1.0
    libssl
    libcrypto
    # Android system libs
    log
    android
    z
  )

# =============================================================================
#  Mode 2 — Custom protocol + libplist
# =============================================================================
elseif(BUILD_MODE EQUAL 2)

  add_library(sideloadnative SHARED
    jni_bridge.c
    usbmux.c
    lockdown.c
    pairing.c
    afc.c
    install_proxy.c
    plist_util.c
  )

  add_library(libplist-2.0 STATIC IMPORTED)
  set_target_properties(libplist-2.0 PROPERTIES
    IMPORTED_LOCATION "${PREBUILT_LIB}/libplist-2.0.a"
  )

  target_include_directories(sideloadnative PRIVATE
    "${PREBUILT_INC}"
    "${PREBUILT_INC}/libplist"
  )

  target_compile_definitions(sideloadnative PRIVATE HAVE_LIBPLIST=1)
  target_link_libraries(sideloadnative libplist-2.0 log android z)

# =============================================================================
#  Mode 3 — Fully bundled fallback
# =============================================================================
else()

  add_library(sideloadnative SHARED
    jni_bridge.c
    usbmux.c
    lockdown.c
    pairing.c
    afc.c
    install_proxy.c
    plist_util.c
  )

  target_link_libraries(sideloadnative log android z)

endif()

# ── Cờ compiler chung ─────────────────────────────────────────────────────────
target_compile_options(sideloadnative PRIVATE
  -Wall -Wextra -Wno-unused-parameter
  -ffunction-sections -fdata-sections
  -fvisibility=hidden
  -fPIC
)

set_target_properties(sideloadnative PROPERTIES
  C_STANDARD 11
  LINK_FLAGS "-Wl,--gc-sections -Wl,--strip-all"
)
