#!/usr/bin/env python3
"""Run the actual GTK/CEF view against a local page and favicon, with an isolated display/profile."""
import argparse
import struct
import zlib
import http.server
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=Path)
args = parser.parse_args()

class Fixture(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/favicon.png":
            def chunk(tag, data):
                return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
            body = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 16, 16, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress((b"\x00" + b"\xff\x00\x00\xff" * 16) * 16)) + chunk(b"IEND", b"")
            mime = "image/png"
        else:
            body = b"<!doctype html><title>CEF native contract</title><link rel=icon href=/favicon.png><h1>Chromium in GTK</h1>"
            mime = "text/html"
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass

with tempfile.TemporaryDirectory(prefix="jupiter-native-check-") as scratch:
    root = Path(scratch)
    display_file = root / "display"
    with display_file.open("w") as display_out:
        display = subprocess.Popen(["Xvfb", "-displayfd", str(display_out.fileno()), "-screen", "0", "1000x750x24", "-nolisten", "tcp", "-ac"], pass_fds=(display_out.fileno(),))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        deadline = time.monotonic() + 5
        while not display_file.read_text().strip():
            if display.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("Xvfb failed to start")
            time.sleep(.05)
        bus = root / "bus.conf"
        bus.write_text('<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth><policy context="default"><allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/></policy></busconfig>')
        env = dict(os.environ, DISPLAY=":" + display_file.read_text().strip(), GDK_BACKEND="x11", GTK_A11Y="none", GIO_USE_VFS="local", GTK_USE_PORTAL="0", JUPITER_SOFTWARE_RENDERING="1", JUPITER_TEST_URL=f"http://127.0.0.1:{server.server_port}/")
        env.pop("WAYLAND_DISPLAY", None)
        subprocess.run(["dbus-run-session", "--config-file=" + str(bus), "--", str(args.binary.resolve())], env=env, check=True, timeout=45)
    finally:
        server.shutdown()
        server.server_close()
        display.terminate()
        display.wait(timeout=5)
