cmake_minimum_required(VERSION 3.22)
project(sideloadnative C)

# =============================================================================
# Native stack built from upstream source by scripts/build_all.sh.
#
# The build output is deliberately kept outside app/src/main/cpp so the
# repository never carries opaque prebuilt archives. Gradle passes
# -DNATIVE_DEPS_ROOT=<repo>/.native-deps and this file selects the current ABI.
# The resulting static libraries are linked into libsideloadnative.so.
#
# Runtime path (Android non-root):
#   UsbDeviceConnection fd -> libusb_wrap_sys_device()
#   -> in-process usbmuxd transport -> libusbmuxd/libimobiledevice
# =============================================================================

if(NOT DEFINED NATIVE_DEPS_ROOT OR NATIVE_DEPS_ROOT STREQUAL "")
  message(FATAL_ERROR
    "NATIVE_DEPS_ROOT is required. Run scripts/build_all.sh before Gradle."
  )
endif()

set(NATIVE_DEPS_DIR "${NATIVE_DEPS_ROOT}/${ANDROID_ABI}")
set(NATIVE_LIB_DIR "${NATIVE_DEPS_DIR}/lib")
set(NATIVE_INC_DIR "${NATIVE_DEPS_DIR}/include")

set(NATIVE_LIBRARIES
  imobiledevice-1.0
  usbmuxd-2.0
  imobiledevice-glue-1.0
  plist-2.0
  usb-1.0
  ssl
  crypto
)

foreach(LIB IN LISTS NATIVE_LIBRARIES)
  if(NOT EXISTS "${NATIVE_LIB_DIR}/lib${LIB}.a")
    message(FATAL_ERROR
      "Missing source-built library: ${NATIVE_LIB_DIR}/lib${LIB}.a"
    )
  endif()
  add_library("native_${LIB}" STATIC IMPORTED GLOBAL)
  set_target_properties("native_${LIB}" PROPERTIES
    IMPORTED_LOCATION "${NATIVE_LIB_DIR}/lib${LIB}.a"
  )
endforeach()

foreach(HEADER_DIR IN ITEMS
  "${NATIVE_INC_DIR}"
  "${NATIVE_INC_DIR}/libimobiledevice"
  "${NATIVE_INC_DIR}/libimobiledevice-glue"
  "${NATIVE_INC_DIR}/plist"
  "${NATIVE_INC_DIR}/libusb-1.0"
)
  if(NOT EXISTS "${HEADER_DIR}")
    message(FATAL_ERROR "Missing source-built headers: ${HEADER_DIR}")
  endif()
endforeach()

add_library(sideloadnative SHARED
  jni_bridge_imd.c
  usb_fd_bridge.c
  android_usbmuxd_fix.c
  usbmuxd_server.c
)

# libimobiledevice calls the normal libusbmuxd client API. On Android the
# app's in-process non-root transport must service those calls using the USB
# fd supplied by UsbDeviceConnection, so redirect the two entry points without
# introducing duplicate symbols into the static archives.
target_link_options(sideloadnative PRIVATE
  "-Wl,--wrap=usbmuxd_get_device"
  "-Wl,--wrap=usbmuxd_connect"
)

target_include_directories(sideloadnative PRIVATE
  "${NATIVE_INC_DIR}"
  "${NATIVE_INC_DIR}/libimobiledevice"
  "${NATIVE_INC_DIR}/libimobiledevice-glue"
  "${NATIVE_INC_DIR}/plist"
  "${NATIVE_INC_DIR}/libusb-1.0"
)

target_compile_definitions(sideloadnative PRIVATE
  HAVE_LIBIMOBILEDEVICE=1
)

target_compile_options(sideloadnative PRIVATE
  -Wall -Wextra -Wno-unused-parameter
  -ffunction-sections -fdata-sections
  -fvisibility=hidden
  -fPIC
)

target_link_libraries(sideloadnative
  # High-level to low-level order is required for static archives.
  native_imobiledevice-1.0
  native_usbmuxd-2.0
  native_imobiledevice-glue-1.0
  native_plist-2.0
  native_usb-1.0
  native_ssl
  native_crypto
  log
  android
  z
)

set_target_properties(sideloadnative PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON
  LINK_FLAGS "-Wl,--gc-sections -Wl,--strip-all"
)
