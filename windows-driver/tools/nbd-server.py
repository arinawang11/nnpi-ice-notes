#!/usr/bin/env python3
"""Minimal NBD server for the NNP-I card.

Two handshakes are implemented: oldstyle (the card's busybox client) and
fixed-newstyle (nbd-client >= 3.10). Requests use the standard kernel NBD
format in both cases.

The backing store is either a regular file or a raw Windows volume such as
\\\\.\\PHYSICALDRIVE4. A volume has no file length, so raw devices need
--size; the server also opens them through CreateFileW because plain
open() cannot address a device path.

Replies are sent by a separate thread so the main loop keeps consuming
requests while a large read reply is in flight. Without that, the card's
small C2H net ring fills, the host stops ACKing, and the link deadlocks.
"""
import argparse
import os
import queue
import socket
import struct
import sys
import threading
import time
import ctypes

if os.name == "nt":
    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _kernel32.CreateFileW.restype = ctypes.c_void_p
    _kernel32.CreateFileW.argtypes = (
        ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p)
    INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
else:
    _kernel32 = None
    INVALID_HANDLE_VALUE = -1

REQ_MAGIC = 0x25609513
REP_MAGIC = 0x67446698
CMD_READ, CMD_WRITE, CMD_DISC, CMD_FLUSH, CMD_TRIM = 0, 1, 2, 3, 4
OLDSTYLE_MAGIC = b"\x00\x00\x42\x02\x81\x86\x12\x53"

# Newstyle (fixed) handshake, required by nbd-client >= 3.10.
IHAVEOPT_MAGIC = b"IHAVEOPT"
REP_MAGIC_OPT = 0x0003E889045565A9  # NBD_REP_MAGIC on option replies
HS_FIXED_NEWSTYLE = 0x1
HS_NO_ZEROES = 0x2
OPT_EXPORT_NAME, OPT_ABORT, OPT_INFO, OPT_GO = 1, 2, 6, 7
REP_ACK, REP_INFO, REP_ERR_UNSUP = 1, 3, 0x80000001
INFO_EXPORT = 0
# HAS_FLAGS | SEND_FLUSH | SEND_TRIM, all implemented below.
# Bit 1 is READ_ONLY, bit 5 is SEND_TRIM -- do not confuse them.
TRANS_FLAGS = 0x1 | 0x4 | 0x20


def tune(conn):
    """Low-latency plus dead-peer detection on an NBD connection."""
    if conn.family not in (socket.AF_INET, socket.AF_INET6):
        # AF_UNIX socketpairs (used by the tests) reject TCP-level options.
        return
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    conn.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    if hasattr(socket, "SIO_KEEPALIVE_VALS"):
        try:
            conn.ioctl(socket.SIO_KEEPALIVE_VALS, (1, 10000, 3000))
        except OSError:
            pass


def recvn(conn, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise EOFError("peer closed")
        buf += chunk
    return bytes(buf)


def open_backing(backing, size_override):
    if backing.startswith("\\\\") or "PhysicalDrive" in backing:
        if _kernel32 is None:
            raise OSError("raw volume access requires Windows")
        g = 0x80000000 | 0x40000000
        h = _kernel32.CreateFileW(backing, g, 3, None, 3, 0, None)
        if h is None or h == INVALID_HANDLE_VALUE:
            raise ctypes.WinError(ctypes.get_last_error())
        import msvcrt
        # O_BINARY is required: without it the CRT may translate line endings.
        fd = msvcrt.open_osfhandle(h, os.O_RDWR | getattr(os, "O_BINARY", 0))
        f = os.fdopen(fd, "r+b", buffering=0)
        size = size_override or 16 << 30
        return f, size
    return open(backing, "r+b", buffering=0), (size_override or os.path.getsize(backing))


def newstyle_handshake(conn, size):
    """Fixed-newstyle negotiation; returns False if the client aborted."""
    conn.sendall(b"NBDMAGIC" + IHAVEOPT_MAGIC +
                 struct.pack(">H", HS_FIXED_NEWSTYLE | HS_NO_ZEROES))
    client_flags = struct.unpack(">I", recvn(conn, 4))[0]
    no_zeroes = bool(client_flags & HS_NO_ZEROES)
    while True:
        magic, opt, length = struct.unpack(">QII", recvn(conn, 16))
        if magic != int.from_bytes(IHAVEOPT_MAGIC, "big"):
            raise ValueError(f"bad option magic {magic:#x}")
        if length:
            recvn(conn, length)
        if opt == OPT_EXPORT_NAME:
            conn.sendall(struct.pack(">QH", size, TRANS_FLAGS) +
                         (b"" if no_zeroes else b"\0" * 124))
            return True
        if opt in (OPT_INFO, OPT_GO):
            info = struct.pack(">HQH", INFO_EXPORT, size, TRANS_FLAGS)
            conn.sendall(struct.pack(">QII", REP_MAGIC_OPT,
                                     opt, REP_INFO) +
                         struct.pack(">I", len(info)) + info)
            conn.sendall(struct.pack(">QII", REP_MAGIC_OPT,
                                     opt, REP_ACK) + struct.pack(">I", 0))
            if opt == OPT_GO:
                return True
            continue
        reply = REP_ACK if opt == OPT_ABORT else REP_ERR_UNSUP
        conn.sendall(struct.pack(">QII", REP_MAGIC_OPT,
                                 opt, reply) + struct.pack(">I", 0))
        if opt == OPT_ABORT:
            return False


def serve_conn(conn, peer, backing, size_override=None, proto="oldstyle",
               io_timeout=0):
    tune(conn)
    conn.settimeout(io_timeout or None)
    f, size = open_backing(backing, size_override)
    if proto == "newstyle":
        if not newstyle_handshake(conn, size):
            print(f"[nbd] {peer} aborted negotiation", flush=True)
            return
    else:
        conn.sendall(b"NBDMAGIC" + OLDSTYLE_MAGIC +
                     struct.pack(">QI", size, 0) + b"\0" * 124)
    print(f"[nbd] {peer} connected ({proto}), export {size} bytes", flush=True)

    out = queue.Queue(maxsize=64)
    send_err = []

    def sender():
        try:
            while True:
                item = out.get()
                if item is None:
                    return
                conn.sendall(item)
        except BaseException as exc:  # socket errors end the sender
            send_err.append(exc)

    th = threading.Thread(target=sender, daemon=True)
    th.start()

    def enqueue(item):
        if send_err:
            raise send_err[0]
        try:
            out.put(item, timeout=10)
        except queue.Full:
            raise TimeoutError("reply queue full")

    reads = writes = others = 0
    st = {"n": 0, "last": time.monotonic(), "gap": 0.0, "svc": 0.0}
    try:
        with f:
            while True:
                hdr = recvn(conn, 28)
                arrived = time.monotonic()
                st["gap"] += arrived - st["last"]
                st["last"] = arrived
                magic, typ, handle, offset, length = struct.unpack(">II8sQI", hdr)
                if magic != REQ_MAGIC:
                    raise ValueError(f"bad request magic {magic:#x}")
                if offset + length > size:
                    enqueue(struct.pack(">II8s", REP_MAGIC, 22, handle))
                    others += 1
                    continue
                if typ == CMD_READ:
                    f.seek(offset)
                    data = f.read(length)
                    if len(data) < length:
                        data += b"\0" * (length - len(data))
                    enqueue(struct.pack(">II8s", REP_MAGIC, 0, handle) + data)
                    reads += 1
                elif typ == CMD_WRITE:
                    data = recvn(conn, length)
                    f.seek(offset)
                    f.write(data)
                    enqueue(struct.pack(">II8s", REP_MAGIC, 0, handle))
                    writes += 1
                elif typ == CMD_FLUSH:
                    f.flush()
                    os.fsync(f.fileno())
                    enqueue(struct.pack(">II8s", REP_MAGIC, 0, handle))
                    others += 1
                elif typ == CMD_TRIM:
                    enqueue(struct.pack(">II8s", REP_MAGIC, 0, handle))
                    others += 1
                elif typ == CMD_DISC:
                    enqueue(struct.pack(">II8s", REP_MAGIC, 0, handle))
                    print("[nbd] disconnect requested", flush=True)
                    break
                else:
                    enqueue(struct.pack(">II8s", REP_MAGIC, 95, handle))
                    others += 1
                st["svc"] += time.monotonic() - arrived
                st["n"] += 1
                if st["n"] % 64 == 0:
                    print(f"[nbd] n={st['n']} "
                          f"avg_gap={st['gap']/st['n']*1000:.2f}ms "
                          f"avg_svc={st['svc']/st['n']*1000:.3f}ms", flush=True)
    finally:
        try:
            out.put(None, timeout=1)
        except queue.Full:
            pass
        th.join(timeout=2)
    print(f"[nbd] {peer} closed, reads={reads} writes={writes} other={others}",
          flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bind", default="192.168.115.1")
    ap.add_argument("--port", type=int, default=10810)
    ap.add_argument("--connect", default="",
                    help="connect out to HOST:PORT instead of listening")
    ap.add_argument("--retry", action="store_true",
                    help="keep reconnecting after a session ends")
    ap.add_argument("--backing", required=True)
    ap.add_argument("--size", type=int, default=0,
                    help="export size for raw devices (bytes)")
    ap.add_argument("--proto", choices=("oldstyle", "newstyle"),
                    default="oldstyle")
    ap.add_argument("--io-timeout", type=float, default=0,
                    help="seconds; 0 keeps the old blocking behaviour")
    args = ap.parse_args()

    if args.connect:
        while True:
            try:
                conn = socket.create_connection(
                    tuple([args.connect.rpartition(":")[0],
                           int(args.connect.rpartition(":")[2])]), 10)
                print(f"[nbd] connected to {args.connect} -> {args.backing}",
                      flush=True)
                try:
                    serve_conn(conn, args.connect, args.backing, args.size,
                               args.proto, args.io_timeout)
                except (EOFError, ConnectionError, ValueError, TimeoutError) as exc:
                    print(f"[nbd] {args.connect} ended: {exc}", flush=True)
                finally:
                    conn.close()
            except OSError as exc:
                print(f"[nbd] connect failed: {exc}", flush=True)
            if not args.retry:
                return 0
            time.sleep(2)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(4)
    print(f"[nbd] listening on {args.bind}:{args.port} -> {args.backing}",
          flush=True)
    while True:
        conn, peer = srv.accept()
        try:
            serve_conn(conn, peer, args.backing, args.size, args.proto,
                       args.io_timeout)
        except (EOFError, ConnectionError, ValueError, TimeoutError) as exc:
            print(f"[nbd] {peer} ended: {exc}", flush=True)
        finally:
            conn.close()


if __name__ == "__main__":
    sys.exit(main())
