#!/usr/bin/env python3
"""fake_iphone.py — giả lập PHÍA iPHONE của giao thức usbmux qua "USB".

Dùng để kiểm chứng usbmuxd_server.c + usb_fd_bridge.c trên máy host (không cần
iPhone thật). mock_libusb.c nối vào socket này; mỗi "USB transfer" là một frame
[u32 little-endian độ dài][dữ liệu]; frame độ dài 0 = ZLP.

Phía thiết bị làm đúng những gì usbmuxd upstream (device.c) mong đợi, và GHI LẠI
mọi vi phạm của phía host:
  - VERSION phải dùng header 8 byte, major = 2; SETUP header 16 byte, 0x07
  - tx_seq của host tăng đúng 1/gói từ 0 (SETUP); rx_seq của host phải là một
    giá trị rx_seq mà thiết bị ĐÃ gửi (ngữ nghĩa upstream: copy rx_seq, không
    phải tx_seq — tx_seq của thiết bị bắt đầu ở 0x5000 để phân biệt được)
  - gói dữ liệu chỉ mang cờ ACK (không PSH), seq liên tục, không vượt cửa sổ
  - gói mux ≤ USB_MTU (49152); gói có độ dài chia hết 512 phải kèm ZLP
  - không bao giờ gửi FIN (upstream đóng bằng RST)
Dịch vụ giả lập:
  62078 lockdownd (QueryType / GetValue)   5555 echo
  5556 gửi RST sau 1000 byte                5557 sink: [u64 len][data] → sha256
  5558 source: gửi 3 MiB dữ liệu có mẫu      5599 báo cáo vi phạm (JSON)
"""
import hashlib
import json
import os
import plistlib
import select
import socket
import struct
import sys

MUX_VERSION, MUX_CONTROL, MUX_SETUP, MUX_TCP = 0, 1, 2, 6
TH_FIN, TH_SYN, TH_RST, TH_PUSH, TH_ACK = 0x01, 0x02, 0x04, 0x08, 0x10
USB_MTU = 3 * 16384
DEV_WINDOW = 131072
UDID = os.environ.get("FAKE_UDID", "00008030-001A35E80C41802E")

violations = []
stats = {"host_packets": 0, "zlp_frames": 0, "data_bytes_in": 0, "data_bytes_out": 0,
         "connections": 0, "refused": 0, "max_mux_packet": 0}


def violate(msg):
    if len(violations) < 200:
        violations.append(msg)
    sys.stderr.write("[fake_iphone] VI PHẠM: %s\n" % msg)


class Conn:
    def __init__(self, sport, dport):
        self.sport, self.dport = sport, dport
        self.dev_seq = 0            # ISN của iPhone = 0 (như trace thật)
        self.host_next = 1          # host SYN tiêu 1 seq
        self.host_acked = 0         # host đã ack tới đâu (dữ liệu của thiết bị)
        self.host_win = 0
        self.state = "SYN_RCVD"
        self.out = bytearray()      # dữ liệu chờ gửi cho host
        self.rst_after_out = False
        self.inbuf = bytearray()
        self.rx_total = 0
        self.sink_len = None
        self.sink_hash = hashlib.sha256()
        self.sink_got = 0


class FakeIPhone:
    def __init__(self, sock):
        self.sock = sock
        self.rbuf = bytearray()
        self.mode = "init"
        self.host_expect_tx = None
        self.dev_tx = 0x5000
        self.dev_rx_sent = {0xFFFF}
        self.last_host_tx = 0xFFFF
        self.conns = {}
        self.expect_zlp = False

    # ── frame I/O ────────────────────────────────────────────────────────
    def send_frame(self, data):
        self.sock.sendall(struct.pack("<I", len(data)) + data)
        if len(data) > 0 and len(data) % 512 == 0:
            self.sock.sendall(struct.pack("<I", 0))      # iPhone thật gửi ZLP

    def send_mux(self, proto, payload, v1=False):
        if v1:
            hdr = struct.pack(">II", proto, 8 + len(payload))
        else:
            rx = self.last_host_tx
            hdr = struct.pack(">IIIHH", proto, 16 + len(payload), 0xFACEFACE, self.dev_tx, rx)
            self.dev_rx_sent.add(rx)
            self.dev_tx = (self.dev_tx + 1) & 0xFFFF
        self.send_frame(hdr + payload)

    def send_tcp(self, c, flags, data=b"", seq=None):
        th = struct.pack(">HHIIBBHHH", c.dport, c.sport, c.dev_seq if seq is None else seq,
                         c.host_next, 0x50, flags, DEV_WINDOW >> 8, 0, 0)
        self.send_mux(MUX_TCP, th + data)

    # ── xử lý frame từ host ─────────────────────────────────────────────
    def on_frame(self, data):
        if len(data) == 0:
            stats["zlp_frames"] += 1
            if not self.expect_zlp:
                violate("ZLP thừa (gói trước không chia hết 512)")
            self.expect_zlp = False
            return
        if self.expect_zlp:
            violate("thiếu ZLP sau gói có độ dài chia hết 512")
            self.expect_zlp = False
        if len(data) % 512 == 0:
            self.expect_zlp = True
        self.rbuf += data
        while len(self.rbuf) >= 8:
            proto, length = struct.unpack(">II", self.rbuf[:8])
            if length < 8 or length > 65536:
                violate("độ dài gói mux vô lý %d" % length)
                self.rbuf.clear()
                return
            if len(self.rbuf) < length:
                return
            pkt = bytes(self.rbuf[:length])
            del self.rbuf[:length]
            self.on_packet(proto, pkt)

    def on_packet(self, proto, pkt):
        stats["host_packets"] += 1
        stats["max_mux_packet"] = max(stats["max_mux_packet"], len(pkt))
        if len(pkt) > USB_MTU:
            violate("gói mux %d byte > USB_MTU" % len(pkt))
        if self.mode == "init":
            if proto != MUX_VERSION:
                violate("gói đầu tiên không phải VERSION (proto %d)" % proto)
                return
            if len(pkt) != 20:
                violate("VERSION phải dùng header 8 byte (len 20), nhận len %d" % len(pkt))
            major, minor, _pad = struct.unpack(">III", pkt[8:20])
            if major != 2:
                violate("VERSION major=%d (phải là 2)" % major)
            self.send_mux(MUX_VERSION, struct.pack(">III", 2, 0, 0), v1=True)
            self.mode = "wait_setup"
            return
        if proto == MUX_VERSION:
            # host gửi lại VERSION khi chưa thấy trả lời — hợp lệ, trả lời lại
            self.send_mux(MUX_VERSION, struct.pack(">III", 2, 0, 0), v1=True)
            return
        if len(pkt) < 16:
            violate("gói v2 ngắn hơn 16 byte")
            return
        _p, _l, magic, tx, rx = struct.unpack(">IIIHH", pkt[:16])
        if magic != 0xFEEDFACE:
            violate("magic 0x%08x" % magic)
        if self.mode == "wait_setup":
            if proto != MUX_SETUP or pkt[16:] != b"\x07" or tx != 0 or rx != 0xFFFF:
                violate("SETUP sai: proto=%d payload=%r tx=%d rx=0x%x" % (proto, pkt[16:], tx, rx))
            self.mode = "active"
            self.host_expect_tx = 1
            self.last_host_tx = tx
            return
        if tx != self.host_expect_tx:
            violate("tx_seq của host = %d, mong đợi %d" % (tx, self.host_expect_tx))
        self.host_expect_tx = (tx + 1) & 0xFFFF
        if rx not in self.dev_rx_sent:
            violate("rx_seq của host = 0x%04x không phải giá trị rx_seq thiết bị đã gửi "
                    "(sai ngữ nghĩa upstream: phải copy rx_seq, không copy tx_seq)" % rx)
        self.last_host_tx = tx
        if proto != MUX_TCP:
            violate("protocol lạ %d sau SETUP" % proto)
            return
        self.on_tcp(pkt[16:])

    def on_tcp(self, seg):
        sport, dport, seq, ack, off, flags, win, _s, _u = struct.unpack(">HHIIBBHHH", seg[:20])
        payload = seg[20:]
        if off != 0x50:
            violate("th_off=0x%02x" % off)
        key = (sport, dport)
        c = self.conns.get(key)
        if flags & TH_FIN:
            violate("host gửi FIN (upstream đóng kết nối bằng RST)")
        if flags == TH_SYN:
            if seq != 0 or ack != 0:
                violate("SYN seq/ack phải = 0")
            if win != (131072 >> 8):
                violate("cửa sổ host %d (upstream quảng bá 512 = 128KiB>>8)" % win)
            if c is not None:
                violate("SYN trùng cổng nguồn %d đang mở" % sport)
            if dport not in (62078, 5555, 5556, 5557, 5558, 5599):
                stats["refused"] += 1
                tmp = Conn(sport, dport)
                tmp.host_next = seq + 1
                self.send_tcp(tmp, TH_RST | TH_ACK, b"no listener")
                return
            c = Conn(sport, dport)
            c.host_win = win << 8
            self.conns[key] = c
            stats["connections"] += 1
            self.send_tcp(c, TH_SYN | TH_ACK)
            c.dev_seq = 1
            return
        if c is None:
            if not (flags & TH_RST):
                self.send_tcp(Conn(sport, dport), TH_RST)
            return
        if flags & TH_RST:
            del self.conns[key]
            return
        if flags & TH_PUSH:
            violate("gói dữ liệu mang cờ PSH (upstream chỉ dùng ACK)")
        if (flags & ~TH_PUSH) != TH_ACK:
            violate("cờ TCP lạ 0x%02x" % flags)
        c.host_win = win << 8
        if ack > c.dev_seq:
            violate("host ack %d vượt dữ liệu đã gửi %d" % (ack, c.dev_seq))
        c.host_acked = max(c.host_acked, ack)
        if c.state == "SYN_RCVD":
            if seq != 1 or ack != 1:
                violate("ACK bắt tay seq=%d ack=%d (mong 1/1)" % (seq, ack))
            c.state = "EST"
            self.on_established(c)
        if payload:
            if seq != c.host_next:
                violate("seq dữ liệu %d != mong đợi %d" % (seq, c.host_next))
            if len(payload) > DEV_WINDOW:
                violate("payload %d vượt cửa sổ thiết bị" % len(payload))
            c.host_next = seq + len(payload)
            stats["data_bytes_in"] += len(payload)
            self.send_tcp(c, TH_ACK)          # ACK ngay như iPhone
            self.on_data(c, payload)
        self.pump(c)

    # ── dịch vụ ──────────────────────────────────────────────────────────
    def on_established(self, c):
        if c.dport == 5558:
            c.out += bytes((i * 7 + 3) & 0xFF for i in range(3 * 1024 * 1024))
        elif c.dport == 5599:
            c.out += json.dumps({"violations": violations, "stats": stats}).encode()

    def on_data(self, c, data):
        c.rx_total += len(data)
        if c.dport == 5555:
            c.out += data
        elif c.dport == 5556:
            if c.rx_total >= 1000:
                self.send_tcp(c, TH_RST)
                del self.conns[(c.sport, c.dport)]
        elif c.dport == 5557:
            c.inbuf += data
            if c.sink_len is None and len(c.inbuf) >= 8:
                c.sink_len = struct.unpack(">Q", c.inbuf[:8])[0]
                del c.inbuf[:8]
            if c.sink_len is not None and c.inbuf:
                take = bytes(c.inbuf[: c.sink_len - c.sink_got])
                c.sink_hash.update(take)
                c.sink_got += len(take)
                del c.inbuf[: len(take)]
                if c.sink_got == c.sink_len:
                    c.out += c.sink_hash.digest()
        elif c.dport == 62078:
            c.inbuf += data
            while len(c.inbuf) >= 4:
                n = struct.unpack(">I", c.inbuf[:4])[0]
                if len(c.inbuf) < 4 + n:
                    break
                req = plistlib.loads(bytes(c.inbuf[4:4 + n]))
                del c.inbuf[:4 + n]
                c.out += self.lockdown_reply(req)

    def lockdown_reply(self, req):
        r = req.get("Request")
        if r == "QueryType":
            resp = {"Request": "QueryType", "Type": "com.apple.mobile.lockdown"}
        elif r == "GetValue":
            key = req.get("Key")
            values = {"UniqueDeviceID": UDID, "ProductVersion": "17.5.1",
                      "DeviceName": "iPhone giả lập", "ProductType": "iPhone12,1"}
            if key in values:
                resp = {"Request": "GetValue", "Key": key, "Value": values[key]}
            else:
                resp = {"Request": "GetValue", "Key": key or "", "Error": "MissingValue"}
        else:
            resp = {"Request": r or "", "Error": "InvalidService"}
        body = plistlib.dumps(resp, fmt=plistlib.FMT_XML)
        return struct.pack(">I", len(body)) + body

    def pump(self, c):
        """Gửi dữ liệu chờ, tôn trọng cửa sổ + ack của host. Dùng cả gói
        > 16 KiB để host phải gom gói qua nhiều transfer."""
        if (c.sport, c.dport) not in self.conns:
            return
        sizes = [40000, 1000, 16384 - 36, 7000, 20480 - 36]
        i = 0
        while c.out:
            inflight = c.dev_seq - c.host_acked
            room = c.host_win - inflight
            if room <= 0:
                return
            n = min(len(c.out), room, sizes[i % len(sizes)], USB_MTU - 36)
            i += 1
            chunk = bytes(c.out[:n])
            del c.out[:n]
            self.send_tcp(c, TH_ACK, chunk)
            c.dev_seq += n
            stats["data_bytes_out"] += n

    def run(self):
        while True:
            r, _, _ = select.select([self.sock], [], [], 1.0)
            if not r:
                continue
            chunk = self.sock.recv(1 << 20)
            if not chunk:
                return
            self.rbuf_frames = getattr(self, "rbuf_frames", bytearray()) + chunk
            buf = self.rbuf_frames
            while len(buf) >= 4:
                n = struct.unpack("<I", buf[:4])[0]
                if len(buf) < 4 + n:
                    break
                frame = bytes(buf[4:4 + n])
                del buf[:4 + n]
                self.on_frame(frame)
            for c in list(self.conns.values()):
                self.pump(c)


def main():
    path = sys.argv[1]
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(1)
    print("READY", flush=True)
    conn, _ = srv.accept()
    FakeIPhone(conn).run()


if __name__ == "__main__":
    main()
