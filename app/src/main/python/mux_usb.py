"""mux_usb.py — ĐÃ PORT SANG NATIVE C (v8).

Giao thức usbmux Apple (USB bulk transfer, TCP-over-USB) hiện được triển khai
hoàn toàn bằng C native trong libsideloadnative.so:
  - app/src/main/cpp/usbmux.c   — usbmux protocol (SETUP, SYN/ACK, data)
  - app/src/main/cpp/lockdown.c — lockdown plist protocol
  - app/src/main/cpp/pairing.c  — device pairing flow
  - app/src/main/cpp/afc.c      — AFC file transfer
  - app/src/main/cpp/jni_bridge.c — JNI entry points

File này được GIỮ LẠI chỉ để không phá vỡ các import cũ.
Mọi lớp/hàm ở đây đều là stub — không bao giờ được gọi trong runtime bình thường.
Thao tác USB thực sự đi qua DeviceNative.kt → NativeBridge.kt → JNI C.

Nếu bạn thấy import từ file này trong code Python, hãy thay bằng:
  from com.superalpha.sideload.bridge import DeviceNative
"""


class MuxError(Exception):
    """Stub — USB errors bây giờ được xử lý ở native C layer."""
    pass


class MuxRstError(MuxError):
    """Stub — RST errors bây giờ được xử lý ở native C layer."""
    pass


# Các hàm API công khai — stub, không bao giờ được gọi trong runtime bình thường.
# Nếu bị gọi, sẽ raise lỗi có thông báo rõ ràng thay vì crash lặng lẽ.

def get_device():
    raise MuxError(
        "[mux_usb] get_device() đã được port sang native C. "
        "Dùng DeviceNative.connectAndPair() từ Kotlin hoặc device_link.pair_device() từ Python."
    )


def reset_device():
    """Stub — gọi DeviceNative.reset() qua device_link.reset_mux_device()."""
    try:
        from com.superalpha.sideload.bridge import DeviceNative
        DeviceNative.reset()
    except Exception as e:
        print(f"[mux_usb] reset_device stub: {e}")
