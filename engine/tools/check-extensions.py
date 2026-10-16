#!/usr/bin/env python3
"""Exercise full uBlock Origin MV2 in the actual CEF extension runtime."""
import argparse
import resource
import functools
import hashlib
import http.server
import os
import signal
import shutil
from pathlib import Path
import subprocess
import tempfile
import threading
import time

# Assertion failures in disposable Chromium tests must not write multi-GB cores.
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=Path)
parser.add_argument("--ublock", type=Path, default=os.environ.get("UBLOCK_DIR"))
parser.add_argument("--fixture", choices=("mv2", "mv3"), help="Run the MV2 or MV3 API fixture instead of uBlock")
args = parser.parse_args()
if not args.fixture and (not args.ublock or not (args.ublock / "manifest.json").is_file()):
    parser.error("Set UBLOCK_DIR or --ublock to the official unpacked full uBlock Origin")
engine = Path(__file__).resolve().parents[1]
artifacts = Path(tempfile.mkdtemp(prefix="jupiter-extension-check-", dir="/tmp"))
print("Artifacts:", artifacts, flush=True)
display_file = artifacts / "display"
with display_file.open("w") as display_out:
    display = subprocess.Popen(["Xvfb", "-displayfd", str(display_out.fileno()), "-screen", "0", "1000x750x24", "-nolisten", "tcp", "-ac"], pass_fds=(display_out.fileno(),), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(engine / "tests/fixtures")))
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    deadline = time.monotonic() + 5
    while not display_file.read_text().strip():
        if display.poll() is not None or time.monotonic() > deadline:
            raise RuntimeError("Xvfb failed to start")
        time.sleep(.05)
    if args.fixture:
        fixture = artifacts / "extension"
        shutil.copytree(engine / "tests/extensions" / args.fixture, fixture, ignore=shutil.ignore_patterns("_metadata"))
        extension = str(fixture)
    else:
        extension = str(args.ublock.resolve())
    ext_id = "".join(chr(97 + int(char, 16)) for char in hashlib.sha256(extension.encode()).hexdigest()[:32])
    setup = artifacts / "configure.js"
    setup.write_text((engine / "tests/fixtures/configure-ublock.js").read_text().replace("JUPITER_FIXTURE_PORT", str(server.server_port)))
    env = dict(os.environ, DISPLAY=":" + display_file.read_text().strip(), JUPITER_PROBE_EXTENSION=extension + "," + str(engine / "data/tab-bridge"), JUPITER_PROBE_URL=f"chrome-extension://{ext_id}/1p-filters.html", JUPITER_PROBE_SCRIPT=str(setup))
    env.pop("WAYLAND_DISPLAY", None)
    if args.fixture:
        env["JUPITER_PROBE_URL"] = f"http://127.0.0.1:{server.server_port}/extensions.html"
        env.pop("JUPITER_PROBE_SCRIPT", None)
    with (artifacts / "ublock.log").open("w") as log:
        process = subprocess.Popen([str(args.binary.resolve())], env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            status = process.wait(timeout=45)
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
    output = (artifacts / "ublock.log").read_text()
    markers = ["EXTENSION_RUNTIME", "EXTENSION_BLOCKING", "EXTENSION_HOST_ISOLATION"] if args.fixture else ["UBO_NETWORK", "UBO_COSMETIC", "UBO_SCRIPTLET", "UBO_FRAME"]
    for marker in markers:
        print(f"{marker}:" + ("PASS" if f"{marker}:PASS" in output and f"{marker}:FAIL" not in output else "FAIL"))
    if status or "UBO_SETUP_ERROR" in output or any(f"{marker}:PASS" not in output or f"{marker}:FAIL" in output for marker in markers):
        raise SystemExit("Extension compatibility check failed; see " + str(artifacts / "ublock.log"))
finally:
    server.shutdown()
    server.server_close()
    display.terminate()
    display.wait(timeout=5)
