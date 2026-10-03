#!/usr/bin/env python3
"""GPU helper smoke test without a screen: plays the core's side of the
protocol against `facet-gpu --offscreen` (Mesa's software rasterizer works)
and checks the composed picture: a plugin surface shows through a hole in the
canvas, scaled into place, and the canvas covers the rest.

    scripts/ci/gpu-smoke.py build/facet-gpu
"""
import json
import mmap
import os
import socket
import struct
import subprocess
import sys
import tempfile

W, H = 320, 200


def send(sock, msg, fd=None):
    data = (json.dumps(dict(msg, **({"fd": True} if fd is not None else {}))) + "\n").encode()
    if fd is None:
        sock.sendall(data)
    else:
        sock.sendmsg([data], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, struct.pack("i", fd))])


def expect(sock, buf, want):
    while True:
        while b"\n" in buf[0]:
            line, buf[0] = buf[0].split(b"\n", 1)
            msg = json.loads(line)
            if msg["t"] == "error":
                sys.exit(f"gpu smoke test failed: helper error: {msg.get('message')}")
            if msg["t"] == want:
                return msg
        chunk = sock.recv(65536)
        if not chunk:
            sys.exit(f"gpu smoke test failed: helper closed the connection waiting for {want}")
        buf[0] += chunk


def main():
    helper = sys.argv[1]
    work = tempfile.mkdtemp()
    dump = os.path.join(work, "frame.ppm")
    core, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    proc = subprocess.Popen([helper, "--fd", str(child.fileno()), "--offscreen", f"{W}x{H}", "--dump", dump],
                            pass_fds=[child.fileno()])
    child.close()
    buf = [b""]
    ready = expect(core, buf, "ready")
    print("ready:", ready)

    # Canvas: dark grey with a transparent hole at (100, 50, 120, 80).
    cfd = os.memfd_create("canvas")
    os.ftruncate(cfd, W * H * 4)
    canvas = mmap.mmap(cfd, W * H * 4)
    grey = struct.pack("<I", 0xFF202020)
    for y in range(H):
        row = bytearray(grey * W)
        if 50 <= y < 130:
            row[100 * 4:220 * 4] = b"\0" * (120 * 4)
        canvas[y * W * 4:(y + 1) * W * 4] = bytes(row)
    send(core, {"t": "canvas", "w": W, "h": H}, cfd)

    # Surface: 60 x 40, two buffers; buffer 1 is pure red (XRGB, alpha byte 0).
    sw, sh = 60, 40
    sfd = os.memfd_create("surface")
    os.ftruncate(sfd, sw * sh * 4 * 2)
    surf = mmap.mmap(sfd, sw * sh * 4 * 2)
    surf[sw * sh * 4:] = struct.pack("<I", 0x00FF0000) * (sw * sh)
    send(core, {"t": "surface", "key": "test/s", "w": sw, "h": sh, "stride": sw * 4, "buffers": 2}, sfd)

    send(core, {"t": "frame", "seq": 1, "canvas": [[0, 0, W, H]],
                "layers": [{"key": "test/s", "buffer": 1, "dst": [100, 50, 120, 80], "clip": [0, 0, W, H]}]})
    expect(core, buf, "uploaded")
    expect(core, buf, "shown")
    send(core, {"t": "quit"})
    proc.wait(timeout=10)

    data = open(dump, "rb").read()
    header, px = data.split(b"\n", 3)[:3], data.split(b"\n", 3)[3]

    def pixel(x, y):
        i = (y * W + x) * 3
        return tuple(px[i:i + 3])

    checks = {"canvas": (pixel(10, 10), (0x20, 0x20, 0x20)), "surface": (pixel(160, 90), (0xFF, 0, 0)),
              "edge outside the hole": (pixel(99, 90), (0x20, 0x20, 0x20))}
    bad = [f"{k}: got {got}, want {want}" for k, (got, want) in checks.items() if got != want]
    if bad:
        sys.exit("gpu smoke test failed: " + "; ".join(bad))
    print("ok: GPU helper composed the frame")


if __name__ == "__main__":
    main()
