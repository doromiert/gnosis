#!/usr/bin/env python3
"""Exercise the pinned native FindTabHelper bridge through private inherited IPC.

Pass a Brave launcher containing the current native UI shim; this does not use
public debugging ports, inject a webpage search implementation, or disable the
Chromium sandbox. All document content is a local fixture.
"""
import argparse
import http.server
import json
import os
from pathlib import Path
import resource
import select
import signal
import socket
import subprocess
import tempfile
import threading
import time
import urllib.parse

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--browser', required=True, type=Path)
parser.add_argument('--log', type=Path, default=Path('/tmp/gnosis-native-find-browser.log'))
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

class Fixture(http.server.BaseHTTPRequestHandler):
    documents = {}
    def log_message(self, *_args):
        pass
    def do_GET(self):
        content = self.documents.get(self.path,b'<!doctype html><p>CrossFrameNeedle</p>')
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(content)))
        self.end_headers()
        self.wfile.write(content)

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
threading.Thread(target=server.serve_forever, daemon=True).start()
read_command, write_command = os.pipe()
read_result, write_result = os.pipe()
native, native_child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)

def setup_child():
    os.dup2(read_command, 3)
    os.dup2(write_result, 4)
    os.dup2(native_child.fileno(), 5)

with tempfile.TemporaryDirectory(prefix='gnosis-native-find-') as profile, args.log.open('w') as browser_log:
    process = subprocess.Popen([
        str(args.browser.resolve()), '--headless=new', '--ozone-platform=wayland',
        '--site-per-process', '--no-first-run', '--no-default-browser-check',
        '--remote-debugging-pipe', '--user-data-dir=' + profile, 'about:blank',
    ], env=dict(os.environ, JUPITER_NATIVE_UI_FD='5'),
        pass_fds=tuple(set((read_command, write_command, read_result, write_result, native_child.fileno(), 3, 4, 5))),
        preexec_fn=setup_child, start_new_session=True, stdout=subprocess.DEVNULL, stderr=browser_log)
    os.close(read_command)
    os.close(write_result)
    native_child.close()
    buffer = b''
    sequence = 0
    replies, native_replies = {}, {}

    def receive_cdp():
        global buffer
        while b'\0' not in buffer:
            chunk = os.read(read_result, 65536)
            if not chunk:
                raise EOFError('Browser closed its private protocol')
            buffer += chunk
        raw, buffer = buffer.split(b'\0', 1)
        message = json.loads(raw)
        if 'id' in message:
            replies[message['id']] = message

    def receive(timeout=10):
        if b'\0' in buffer:
            receive_cdp()
            return
        ready, _, _ = select.select([read_result, native], [], [], timeout)
        if not ready:
            raise TimeoutError('Native find/protocol did not respond')
        if read_result in ready:
            receive_cdp()
        if native in ready:
            raw = native.recv(65536)
            if not raw:
                raise EOFError('Browser closed the native UI channel')
            message = json.loads(raw)
            if message.get('kind') == 'find-result':
                native_replies[message['id']] = message

    def call(method, params=None, session=None):
        global sequence
        sequence += 1
        ident = sequence
        message = {'id': ident, 'method': method, 'params': params or {}}
        if session:
            message['sessionId'] = session
        os.write(write_command, json.dumps(message).encode() + b'\0')
        deadline = time.monotonic() + 15
        while ident not in replies:
            if time.monotonic() >= deadline:
                raise TimeoutError('Private protocol reply')
            receive(max(.01, deadline - time.monotonic()))
        reply = replies.pop(ident)
        if 'error' in reply:
            raise RuntimeError(reply['error'])
        return reply.get('result', {})

    def send_find(target, ident, text='', forward=True, sensitive=False, match=True, stop=False):
        native.send(json.dumps({
            'kind': 'find-stop' if stop else 'find', 'id': ident, 'target': target,
            'text': text, 'forward': forward, 'caseSensitive': sensitive, 'findMatch': match,
        }).encode())

    def result(ident):
        deadline = time.monotonic() + 10
        while ident not in native_replies:
            if time.monotonic() >= deadline:
                raise TimeoutError('Native find reply')
            receive(max(.01, deadline - time.monotonic()))
        message = native_replies.pop(ident)
        assert message['id'] == ident
        return message

    def search(target, ident, text, **options):
        send_find(target, ident, text, **options)
        message = result(ident)
        assert message['target'] == target, message
        assert not message['error'], message
        assert 0 <= message['ordinal'] <= message['count'], message
        return message

    def attach(target):
        session = call('Target.attachToTarget', {'targetId': target, 'flatten': True})['sessionId']
        call('Page.enable', session=session)
        call('Runtime.enable', session=session)
        return session

    def navigate(session, html):
        path='/document/'+str(len(Fixture.documents))
        Fixture.documents[path]=html.encode()
        url=f'http://127.0.0.1:{server.server_port}{path}'
        call('Page.navigate', {'url':url}, session)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            loaded = call('Runtime.evaluate', {'expression': 'location.href==='+json.dumps(url)+' && document.readyState==="complete"', 'returnByValue': True}, session)
            if loaded.get('result', {}).get('value') is True:
                return
            time.sleep(.05)
        raise TimeoutError('Fixture load')

    try:
        deadline = time.monotonic() + 10
        while True:
            pages = [target['targetId'] for target in call('Target.getTargets')['targetInfos'] if target['type'] == 'page']
            if pages:
                break
            if time.monotonic() >= deadline:
                raise TimeoutError('No native page target')
            time.sleep(.05)
        first = pages[0]
        session = attach(first)
        navigate(session, "<!doctype html><p>Alpha alpha ALPHA</p><p>long native unicode café поиск 🌍</p>"
                 "<p hidden>InvisibleNeedle</p><iframe srcdoc='<p>alpha</p>'></iframe>"
                 f"<iframe src='http://localhost:{server.server_port}/frame'></iframe>")
        initial = search(first, 1, 'alpha')
        assert initial['count'] == 4, initial
        second = search(first, 2, 'alpha')
        assert second['count'] == 4 and second['ordinal'] != initial['ordinal'], (initial, second)
        previous = search(first, 3, 'alpha', forward=False)
        assert previous['ordinal'] == initial['ordinal'], previous
        cycle = [search(first, ident, 'alpha')['ordinal'] for ident in range(10, 14)]
        assert len(set(cycle)) == 4 and cycle[-1] == initial['ordinal'], cycle
        assert search(first, 20, 'Alpha', sensitive=True)['count'] == 1
        for ident, text in enumerate(['поиск', 'café', '🌍', 'long native unicode café поиск 🌍'], 21):
            assert search(first, ident, text)['count'] == 1
        assert search(first, 30, 'no such visible string')['count'] == 0
        assert search(first, 31, 'InvisibleNeedle')['count'] == 0
        assert search(first, 32, 'CrossFrameNeedle')['count'] == 1
        frames = [target for target in call('Target.getTargets')['targetInfos'] if target['type'] == 'iframe']
        assert frames, 'Cross-origin iframe did not enter a separate renderer target'
        print('FIND_NATIVE_MATCH_PASS counts, ordinals, wrapping, case, Unicode, hidden text, same/cross-process frames', flush=True)

        assert search(first, 40, 'alpha', match=False)['count'] == 4
        before = time.monotonic()
        assert search(first, 41, 'alpha', match=False)['count'] == 4
        assert time.monotonic() - before < 2, 'Repeated native count-only request stalled'
        assert search(first, 42, 'x' * 5000)['count'] == 0
        send_find(first, 43, 'x' * 8193)
        assert result(43)['error'], 'Oversized search accepted'
        assert search(first, 44, 'alpha')['count'] == 4
        assert search(first, 45, '')['count'] == 0
        print('FIND_COUNT_AND_BOUNDS_PASS repeated count-only, IPC >4KiB, limit rejection, empty query', flush=True)

        send_find(first, 50, 'alpha')
        send_find(first, 51, 'поиск')
        replaced, latest = result(50), result(51)
        assert replaced['error'] or replaced['count'] == 4, replaced
        assert latest['count'] == 1 and not latest['error'], latest
        send_find(first, 52, 'alpha')
        send_find(first, 53, stop=True)
        canceled = result(52)
        assert canceled['error'] or canceled['count'] == 4, canceled
        assert search(first, 54, 'alpha')['count'] == 4
        print('FIND_REPLACE_STOP_PASS overlapping query replacement and immediate stop/restart', flush=True)

        other = call('Target.createTarget', {'url': 'about:blank'})['targetId']
        other_session = attach(other)
        navigate(other_session, '<!doctype html><p>OnlyOther OnlyOther</p>')
        send_find(first, 60, 'alpha')
        send_find(other, 61, 'OnlyOther')
        one, two = result(60), result(61)
        assert one['target'] == first and one['count'] == 4 and not one['error'], one
        assert two['target'] == other and two['count'] == 2 and not two['error'], two
        assert search(first, 62, 'OnlyOther')['count'] == 0
        send_find(other, 63, 'OnlyOther')
        call('Target.closeTarget', {'targetId': other})
        send_find(other, 64, stop=True)
        closed = result(63)
        assert closed['error'] or closed['count'] == 2, closed
        send_find(other, 65, 'OnlyOther')
        assert result(65)['error'], 'Closed target still searchable'
        send_find('0' * 32, 66, 'alpha')
        assert result(66)['error'], 'Invented target accepted'
        assert search(first, 67, 'alpha')['count'] == 4
        print('FIND_TARGET_LIFETIME_PASS independent tabs, closed/invented target rejection, surviving tab remains usable', flush=True)
        print('NATIVE_FIND_PASS', flush=True)
    finally:
        try:
            call('Browser.close')
        except (EOFError, TimeoutError, OSError):
            pass
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
        os.close(write_command)
        os.close(read_result)
        native.close()
        server.shutdown()
