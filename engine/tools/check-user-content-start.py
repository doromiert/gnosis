#!/usr/bin/env python3
"""Real HTTP/internal CSP fixture for native renderer document-start checks."""
import argparse
import http.server
import os
from pathlib import Path
import resource
import subprocess
import tempfile
import threading

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
args = parser.parse_args()
CSP = "script-src 'nonce-jupiter-first'; object-src 'none'; base-uri 'none'; frame-src http:"

def document(kind, port=0, internal=False):
    meta = '<meta http-equiv="Content-Security-Policy" content="' + CSP + '">' if internal else ''
    body = '<!doctype html>' + meta + '<title>Native user-content start fixture</title>'
    # Actual CSP must deny this source while allowing the following nonce source.
    body += '<script>globalThis.__userContentUnauthorized = true;</script>'
    body += '''<script nonce="jupiter-first">
      const marker = globalThis.__jupiterUserContentStart;
      const report = {...(marker||{}),firstSawMarker:!!marker,nonceRan:true,
                      unauthorizedRan:globalThis.__userContentUnauthorized===true,href:location.href};
      globalThis.__userContentReports = {main: report};
      addEventListener('message', event => {
        if (!event.data?.userContentStartReport) return;
        for (const name of ['local','oopif','excluded'])
          if (event.source===document.getElementById(name)?.contentWindow)
            __userContentReports[name] = event.data.report;
        document.title='USER_CONTENT_START '+JSON.stringify(__userContentReports);
      });
      if (top!==window) parent.postMessage({userContentStartReport:true,report},'*');
      document.title='USER_CONTENT_START '+JSON.stringify(__userContentReports);
    </script>'''
    if kind == 'parent':
        body += '<iframe id="local" src="/user-content-start-child?local"></iframe>'
        body += '<iframe id="excluded" src="/user-content-start-denied"></iframe>'
        body += f'<iframe id="oopif" src="http://localhost:{port}/user-content-start-child?oopif"></iframe>'
    return body.encode()

class Fixture(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if not self.path.startswith('/user-content-start'):
            self.send_error(404)
            return
        kind = 'child' if self.path.startswith(('/user-content-start-child', '/user-content-start-denied')) else 'parent'
        body = document(kind, self.server.server_port)
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Security-Policy', CSP)
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *args):
        pass

with tempfile.TemporaryDirectory(prefix='jupiter-user-content-start-fixture-') as folder:
    internal = Path(folder) / 'internal.html'
    internal.write_bytes(document('internal', internal=True))
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    env = dict(os.environ, JUPITER_USER_CONTENT_START_URL=f'http://127.0.0.1:{server.server_port}/user-content-start',
               JUPITER_USER_CONTENT_START_INTERNAL_HTML=str(internal))
    try:
        result = subprocess.run([str(args.binary.resolve()), '--site-per-process'], env=env, timeout=130)
    finally:
        server.shutdown()
    raise SystemExit(result.returncode)
