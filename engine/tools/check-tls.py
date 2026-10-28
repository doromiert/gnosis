#!/usr/bin/env python3
"""Serve disposable, untrusted TLS certificates for native Chromium integration checks."""
import http.server
import json
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

fixture = json.loads((Path(__file__).resolve().parents[1] / 'tests/fixtures/tls.json').read_text())
ui_workflow = sys.argv[1:2] == ['--ui']
command = sys.argv[2:] if ui_workflow else sys.argv[1:]


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.server.requests.append(self.path)
        if self.path.startswith('/redirect?'):
            target = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)['to'][0]
            self.send_response(302)
            self.send_header('Location', target)
            self.send_header('Access-Control-Allow-Origin', '*')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        body = ('<!doctype html><title>TLS fixture ' + self.server.label + '</title>'
                '<h1>Actual TLS response</h1>').encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class Server(http.server.ThreadingHTTPServer):
    context = None

    def get_request(self):
        connection, address = super().get_request()
        connection.settimeout(5)
        if self.context:
            try:
                if self.label == 'delayed':
                    self.started.touch()
                    deadline = time.monotonic() + 10
                    while not self.release.exists() and time.monotonic() < deadline:
                        time.sleep(.01)
                connection = self.context.wrap_socket(connection, server_side=True)
            except OSError:
                connection.close()
                raise
            finally:
                if self.label == 'delayed':
                    self.completed.touch()
        return connection, address


servers = []
try:
    with tempfile.TemporaryDirectory(prefix='gnosis-tls-fixtures-') as scratch:
        root = Path(scratch)
        key = root / 'server.key'
        key.write_text(fixture['key'])
        key.chmod(0o600)
        ca = root / 'ca.pem'
        ca.write_text(fixture['root'])
        urls = {}
        for name in [*fixture['certificates'], 'delayed', 'plain']:
            server = Server(('127.0.0.1', 0), Handler)
            server.label, server.requests = name, []
            server.started, server.release, server.completed = (root / name for name in ('started', 'release', 'completed'))
            servers.append(server)
            if name != 'plain':
                certificate = root / (name + '.pem')
                certificate.write_text(fixture['certificates']['expired' if name == 'delayed' else name] +
                                       (fixture['root'] if name != 'self-signed' else ''))
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                context.load_cert_chain(certificate, key)
                server.context = context
            threading.Thread(target=server.serve_forever, daemon=True).start()
            urls[name] = f"{'http' if name == 'plain' else 'https'}://127.0.0.1:{server.server_port}/"
        unused = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        urls['unavailable'] = f'http://127.0.0.1:{unused.server_port}/'
        unused.server_close()
        env = dict(os.environ, GNOSIS_TLS_FIXTURES=json.dumps(urls), GNOSIS_TLS_CA=str(ca),
                   GNOSIS_TLS_DELAY_STARTED=str(root / 'started'), GNOSIS_TLS_DELAY_RELEASE=str(root / 'release'),
                   GNOSIS_TLS_DELAY_COMPLETED=str(root / 'completed'))
        subprocess.run(command, env=env, check=True, timeout=90)
        assert any(s.label == 'valid' and s.requests for s in servers), 'Accepted TLS page never reached the server'
        if not ui_workflow:
            assert any('/private-accepted-resource' in s.requests for s in servers), 'Explicit private exception failed to load its own subresource'
        assert not any(any(path.startswith(('/subresource', '/private-download-denied')) for path in s.requests)
                       for s in servers), 'A private request inherited a regular certificate exception'
        print('Local TLS traffic confirms accepted main-frame loads and rejects invalid-certificate subresources: PASS', flush=True)
finally:
    for server in servers:
        server.shutdown()
        server.server_close()
