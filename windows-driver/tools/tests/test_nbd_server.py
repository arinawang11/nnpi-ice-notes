#!/usr/bin/env python3
"""Protocol tests for nbd-server.py.

These use socketpair and a regular file as backing, so they run without a
Windows volume, a card, or the network. They cover both handshakes plus a
read/write/flush exchange.
"""
import importlib.util
import os
import socket
import struct
import tempfile
import threading
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]


def load_server():
    spec = importlib.util.spec_from_file_location(
        "nbd_server", TOOLS / "nbd-server.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


nbd = load_server()
SIZE = 4096


def recvn(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("closed")
        buf += chunk
    return buf


def run(target):
    errors = []

    def wrapper():
        try:
            target()
        except (EOFError, ConnectionError, OSError, ValueError, TimeoutError):
            pass
        except BaseException as exc:  # noqa: BLE001 - surfaced to the test
            errors.append(exc)

    thread = threading.Thread(target=wrapper, daemon=True)
    thread.start()
    return thread, errors


class NewstyleTests(unittest.TestCase):
    def setUp(self):
        self.client, self.server = socket.socketpair()
        self.client.settimeout(5)

    def tearDown(self):
        self.client.close()
        self.server.close()

    def greeting(self):
        magic = recvn(self.client, 8)
        ihaveopt = recvn(self.client, 8)
        flags = struct.unpack(">H", recvn(self.client, 2))[0]
        self.assertEqual(magic, b"NBDMAGIC")
        self.assertEqual(ihaveopt, b"IHAVEOPT")
        self.assertTrue(flags & nbd.HS_FIXED_NEWSTYLE)
        self.client.sendall(struct.pack(">I", flags & nbd.HS_NO_ZEROES))

    def option(self, opt, payload=b""):
        self.client.sendall(struct.pack(">QII", int.from_bytes(
            nbd.IHAVEOPT_MAGIC, "big"), opt, len(payload)) + payload)

    def test_export_name(self):
        thread, errors = run(
            lambda: nbd.newstyle_handshake(self.server, SIZE))
        self.greeting()
        self.option(nbd.OPT_EXPORT_NAME)
        reply = recvn(self.client, 10)
        size, flags = struct.unpack(">QH", reply)
        self.assertEqual((size, flags), (SIZE, nbd.TRANS_FLAGS))
        thread.join(5)
        self.assertFalse(errors)

    def test_info_then_go(self):
        thread, errors = run(
            lambda: nbd.newstyle_handshake(self.server, SIZE))
        self.greeting()

        self.option(nbd.OPT_INFO)
        magic, opt, rep = struct.unpack(">QII", recvn(self.client, 16))
        self.assertEqual((magic, opt, rep), (nbd.REP_MAGIC_OPT,
                                             nbd.OPT_INFO, nbd.REP_INFO))
        length = struct.unpack(">I", recvn(self.client, 4))[0]
        info = recvn(self.client, length)
        self.assertEqual(struct.unpack(">HQH", info),
                         (nbd.INFO_EXPORT, SIZE, nbd.TRANS_FLAGS))
        magic, opt, rep = struct.unpack(">QII", recvn(self.client, 16))
        self.assertEqual((magic, opt, rep), (nbd.REP_MAGIC_OPT,
                                             nbd.OPT_INFO, nbd.REP_ACK))
        self.assertEqual(struct.unpack(">I", recvn(self.client, 4))[0], 0)

        self.option(nbd.OPT_GO)
        # INFO payload, then ACK
        for expected in (nbd.REP_INFO, nbd.REP_ACK):
            magic, opt, rep = struct.unpack(">QII", recvn(self.client, 16))
            length = struct.unpack(">I", recvn(self.client, 4))[0]
            recvn(self.client, length)
            self.assertEqual((magic, opt, rep), (nbd.REP_MAGIC_OPT,
                                                 nbd.OPT_GO, expected))
        thread.join(5)
        self.assertFalse(errors)


class TransferTests(unittest.TestCase):
    def setUp(self):
        self.client, self.server = socket.socketpair()
        self.client.settimeout(5)
        fd, self.path = tempfile.mkstemp()
        os.write(fd, b"a" * SIZE)
        os.close(fd)

    def tearDown(self):
        self.client.close()
        self.server.close()
        os.unlink(self.path)

    def oldstyle_greeting(self):
        magic = recvn(self.client, 8)
        old = recvn(self.client, 8)
        size, flags = struct.unpack(">QI", recvn(self.client, 12))
        recvn(self.client, 124)
        self.assertEqual(magic, b"NBDMAGIC")
        self.assertEqual(old, nbd.OLDSTYLE_MAGIC)
        self.assertEqual((size, flags), (SIZE, 0))

    def request(self, typ, handle, offset, length, data=b""):
        self.client.sendall(struct.pack(">II8sQI", nbd.REQ_MAGIC, typ,
                                        handle, offset, length) + data)
        magic, error, echoed = struct.unpack(">II8s", recvn(self.client, 16))
        self.assertEqual((magic, error, echoed), (nbd.REP_MAGIC, 0, handle))

    def test_read_write_flush(self):
        thread, errors = run(
            lambda: nbd.serve_conn(self.server, "test", self.path,
                                   None, "oldstyle"))
        self.oldstyle_greeting()
        self.request(nbd.CMD_READ, b"r" * 8, 0, 16)
        self.assertEqual(recvn(self.client, 16), b"a" * 16)

        self.request(nbd.CMD_WRITE, b"w" * 8, 0, 4, b"bcde")
        self.request(nbd.CMD_FLUSH, b"f" * 8, 0, 0)
        self.client.close()

        thread.join(5)
        self.assertFalse(errors)
        with open(self.path, "rb") as f:
            self.assertEqual(f.read(4), b"bcde")

    def test_out_of_range(self):
        thread, errors = run(
            lambda: nbd.serve_conn(self.server, "test", self.path,
                                   None, "oldstyle"))
        self.oldstyle_greeting()
        self.client.sendall(struct.pack(">II8sQI", nbd.REQ_MAGIC,
                                        nbd.CMD_READ, b"x" * 8,
                                        SIZE - 4, 8))
        magic, error, _ = struct.unpack(">II8s", recvn(self.client, 16))
        self.assertEqual((magic, error), (nbd.REP_MAGIC, 22))
        self.client.close()
        thread.join(5)
        self.assertFalse(errors)


if __name__ == "__main__":
    unittest.main()
