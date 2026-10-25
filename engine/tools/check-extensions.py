#!/usr/bin/env python3
"""Exercise full uBlock Origin MV2 in the actual CEF extension runtime."""
import argparse
import resource
import functools
import hashlib
import http.server
import json
import os
import signal
import shutil
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import urllib.parse
import uuid

# Assertion failures in disposable Chromium tests must not write multi-GB cores.
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=Path)
parser.add_argument("--ublock", type=Path, default=os.environ.get("UBLOCK_DIR"))
parser.add_argument("--fixture", choices=("mv2", "mv3"), help="Run the MV2 or MV3 API fixture instead of uBlock")
parser.add_argument("--lifecycle", action="store_true", help="Also close an executeScript destination while the real MV2 background awaits its result")
parser.add_argument("--network-probes", action="store_true", help="Verify full uBlock HEAD/GET blocking and redirects against independent local server logs")
parser.add_argument("--public-adblock-probe", action="store_true", help="Diagnose the live public adblock test with unchanged default uBlock filters and native network tracing")
args = parser.parse_args()
if args.lifecycle and args.fixture != "mv2":
    parser.error("--lifecycle requires --fixture mv2")
if args.network_probes and args.fixture:
    parser.error("--network-probes requires full uBlock, not an API fixture")
if args.public_adblock_probe and (args.fixture or args.network_probes):
    parser.error("--public-adblock-probe is a separate full-uBlock diagnostic")
if not args.fixture and (not args.ublock or not (args.ublock / "manifest.json").is_file()):
    parser.error("Set UBLOCK_DIR or --ublock to the official unpacked full uBlock Origin")
engine = Path(__file__).resolve().parents[1]
artifacts = Path(tempfile.mkdtemp(prefix="jupiter-extension-check-", dir="/tmp"))
print("Artifacts:", artifacts, flush=True)
display_file = artifacts / "display"
with display_file.open("w") as display_out:
    display = subprocess.Popen(["Xvfb", "-displayfd", str(display_out.fileno()), "-screen", "0", "1000x750x24", "-nolisten", "tcp", "-ac"], pass_fds=(display_out.fileno(),), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
requests = []
request_lock = threading.Lock()
run_nonce = uuid.uuid4().hex

class ProbeHandler(http.server.SimpleHTTPRequestHandler):
    def probe(self):
        parsed = urllib.parse.urlsplit(self.path)
        if not args.network_probes or not parsed.path.startswith("/network-probe/"):
            return False
        entry = {"origin": self.server.probe_origin, "method": self.command,
                 "path": parsed.path, "nonce": urllib.parse.parse_qs(parsed.query).get("nonce", [""])[0]}
        with request_lock:
            requests.append(entry)
            with (artifacts / "network-requests.jsonl").open("a") as log:
                log.write(json.dumps(entry) + "\n")
        self.send_response(404 if parsed.path.endswith("allowed-404") else 200)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", "0")
        self.end_headers()
        return True

    def do_HEAD(self):
        if not self.probe():
            super().do_HEAD()

    def do_GET(self):
        if not self.probe():
            super().do_GET()

handler = functools.partial(ProbeHandler, directory=str(engine / "tests/fixtures"))
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
server.probe_origin = "same"
threading.Thread(target=server.serve_forever, daemon=True).start()
cross_server = None
if args.network_probes:
    cross_server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    cross_server.probe_origin = "cross"
    threading.Thread(target=cross_server.serve_forever, daemon=True).start()
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
    network_config = {"origin": f"http://127.0.0.1:{cross_server.server_port}", "nonce": run_nonce} if cross_server else None
    setup.write_text((engine / "tests/fixtures/configure-ublock.js").read_text()
                     .replace("JUPITER_FIXTURE_PORT", str(server.server_port))
                     .replace("JUPITER_NETWORK_CONFIG", json.dumps(network_config)))
    if args.public_adblock_probe:
        setup.write_text((engine / "tests/fixtures/prepare-public-adblock.js").read_text())
    env = dict(os.environ, DISPLAY=":" + display_file.read_text().strip(), JUPITER_PROBE_EXTENSION=extension + "," + str(engine / "data/tab-bridge"), JUPITER_PROBE_URL=f"chrome-extension://{ext_id}/1p-filters.html", JUPITER_PROBE_SCRIPT=str(setup))
    env.pop("WAYLAND_DISPLAY", None)
    if args.fixture:
        env["JUPITER_PROBE_URL"] = f"http://127.0.0.1:{server.server_port}/extensions.html"
        env.pop("JUPITER_PROBE_SCRIPT", None)
    if args.lifecycle:
        env["JUPITER_PROBE_LIFECYCLE"] = "1"
    if args.network_probes:
        env["JUPITER_PROBE_NETWORK"] = "1"
    if args.public_adblock_probe:
        env["JUPITER_PUBLIC_ADBLOCK_SCRIPT"] = str(engine / "tests/fixtures/public-adblock.js")
    with (artifacts / "ublock.log").open("w") as log:
        process = subprocess.Popen([str(args.binary.resolve())], env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            status = process.wait(timeout=180 if args.public_adblock_probe else 45)
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
    output = (artifacts / "ublock.log").read_text()
    markers = ["EXTENSION_RUNTIME", "EXTENSION_BLOCKING", "EXTENSION_HOST_ISOLATION"] if args.fixture else ["UBO_NETWORK", "UBO_COSMETIC", "UBO_SCRIPTLET", "UBO_FRAME"]
    if args.public_adblock_probe:
        markers = ["PUBLIC_ADBLOCK"]
        public = {}
        for name in ("FETCH", "REPORT", "DEFAULTS"):
            prefix = f"PUBLIC_ADBLOCK_{name}:"
            public[name.lower()] = [json.loads(line.removeprefix(prefix)) for line in output.splitlines()
                                    if line.startswith(prefix)]
        events = []
        for line in output.splitlines():
            if line.startswith("PUBLIC_ADBLOCK_CDP:"):
                method, _, params = line.removeprefix("PUBLIC_ADBLOCK_CDP:").partition(":")
                events.append({"method": method, "params": json.loads(params)})
        (artifacts / "public-adblock-results.json").write_text(json.dumps(public, indent=2) + "\n")
        (artifacts / "public-adblock-network.jsonl").write_text("".join(json.dumps(event) + "\n" for event in events))
        if (len(public["defaults"]) != 1 or {r["stage"] for r in public["report"]} != {"initial", "retest"}
                or "PUBLIC_ADBLOCK_RETEST_CLICK" not in output
                or any(f"PUBLIC_ADBLOCK_CDP_SETUP:{id}:PASS:" not in output for id in (1000000, 1000001, 1000002))):
            raise SystemExit("Public probe did not collect defaults, native tracing and both completed runs; see " + str(artifacts))
        for report in public["report"]:
            stage, score = report["stage"], report["report"]["abt"]
            host_fetches = [r for r in public["fetch"] if r["stage"] == stage and "/fakepage.html" in r["url"]]
            print(f"PUBLIC_ADBLOCK_{stage.upper()}: {score['blocked']}/{score['total']} reported blocked; "
                  f"HEAD fetches {sum(r['outcome'] == 'fulfilled' for r in host_fetches)} fulfilled, "
                  f"{sum(r['outcome'] == 'rejected' for r in host_fetches)} rejected")
        redirected = [e for e in events if e["method"] == "Network.requestWillBeSent" and "redirectResponse" in e["params"]]
        failed = [e for e in events if e["method"] == "Network.loadingFailed"]
        print(f"PUBLIC_ADBLOCK_NATIVE_TRACE: {len(redirected)} redirects, {len(failed)} loading failures; see {artifacts}")
    if args.lifecycle:
        markers += ["EXTENSION_DESTINATION_CLOSE", "EXTENSION_TRANSPORT_OWNERSHIP"]
    if args.network_probes:
        markers += ["UBO_NETWORK_PROBES"]
        reports = [line.removeprefix("UBO_NETWORK_RESULTS:") for line in output.splitlines()
                   if line.startswith("UBO_NETWORK_RESULTS:")]
        if len(reports) != 1:
            raise SystemExit("Missing or duplicate network report; see " + str(artifacts))
        results = json.loads(reports[0])
        (artifacts / "network-results.json").write_text(json.dumps(results, indent=2) + "\n")
        expected = {(origin, method, kind, f"{run_nonce}-{origin}-{method}-{kind}-{repeat}")
                    for origin in ("same", "cross") for method in ("HEAD", "GET")
                    for kind in ("allowed-200", "allowed-404", "hard-block", "redirect-empty")
                    for repeat in range(4)}
        actual = {(r["origin"], r["method"], r["kind"], r["nonce"]) for r in results}
        controls = {(origin, method, "/network-probe/" + kind, nonce)
                    for origin, method, kind, nonce in expected if kind.startswith("allowed-")}
        with request_lock:
            received = [(r["origin"], r["method"], r["path"], r["nonce"]) for r in requests]
        if len(results) != len(expected) or actual != expected or len(received) != len(controls) or set(received) != controls:
            raise SystemExit("Independent server request accounting failed; see " + str(artifacts))
        print("UBO_NETWORK_SERVER:PASS (32 controls arrived; 32 blocked/redirected requests absent)")
    for marker in markers:
        print(f"{marker}:" + ("PASS" if f"{marker}:PASS" in output and f"{marker}:FAIL" not in output else "FAIL"))
    if (status or "UBO_SETUP_ERROR" in output or "Check failed:" in output or
            "Diagnostic process failed" in output or
            any(f"{marker}:PASS" not in output or f"{marker}:FAIL" in output for marker in markers)):
        raise SystemExit("Extension compatibility check failed; see " + str(artifacts / "ublock.log"))
finally:
    server.shutdown()
    server.server_close()
    if cross_server:
        cross_server.shutdown()
        cross_server.server_close()
    display.terminate()
    display.wait(timeout=5)
