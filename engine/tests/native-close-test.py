#!/usr/bin/env python3
"""Exercise native beforeunload preflight through private inherited IPC.

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
parser.add_argument('--log', type=Path, default=Path('/tmp/gnosis-native-close-browser.log'))
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

class Fixture(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass
    def do_GET(self):
        content = b'<!doctype html><p>CrossFrameNeedle</p>'
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

with tempfile.TemporaryDirectory(prefix='gnosis-native-close-') as profile, args.log.open('w') as browser_log:
    process = subprocess.Popen([
        str(args.browser.resolve()), '--headless=new', '--ozone-platform=wayland',
        '--site-per-process', '--no-first-run', '--no-default-browser-check',
        '--remote-debugging-pipe', '--user-data-dir=' + profile, 'about:blank',
    ], env=dict(os.environ, GNOSIS_NATIVE_UI_FD='5'),
        pass_fds=tuple(set((read_command, write_command, read_result, write_result, native_child.fileno(), 3, 4, 5))),
        preexec_fn=setup_child, start_new_session=True, stdout=subprocess.DEVNULL, stderr=browser_log)
    os.close(read_command)
    os.close(write_result)
    native_child.close()
    buffer = b''
    sequence = 0
    replies, native_replies = {}, {}
    events=[]

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
        else: events.append(message)

    def receive(timeout=10):
        if b'\0' in buffer:
            receive_cdp()
            return
        ready, _, _ = select.select([read_result, native], [], [], timeout)
        if not ready:
            return
        if read_result in ready:
            receive_cdp()
        if native in ready:
            raw = native.recv(65536)
            if not raw:
                raise EOFError('Browser closed the native UI channel')
            message = json.loads(raw)
            if message.get('kind') in ['find-result','beforeunload-result']:
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
        call('Page.navigate', {'url': 'data:text/html;charset=utf-8,' + urllib.parse.quote(html)}, session)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            loaded = call('Runtime.evaluate', {'expression': 'document.readyState', 'returnByValue': True}, session)
            if loaded.get('result', {}).get('value') == 'complete':
                return
            time.sleep(.05)
        raise TimeoutError('Fixture load')

    try:
        deadline = time.monotonic() + 10
        while True:
            pages = [x['targetId'] for x in call('Target.getTargets')['targetInfos'] if x['type']=='page']
            if pages: break
            if time.monotonic()>deadline: raise TimeoutError('page')
            time.sleep(.05)
        target=pages[0];session=attach(target)
        navigate(session,"<!doctype html><button style='width:200px;height:100px'>Edit draft</button><p>native beforeunload fixture</p>")
        events.clear()
        def check(ident,accept=None,cancel=False):
            native.send(json.dumps({'kind':'beforeunload-check','id':ident,'target':target}).encode())
            prompted=False;deadline=time.monotonic()+10
            while ident not in native_replies:
                if time.monotonic()>deadline:raise TimeoutError('beforeunload native result')
                receive(.2)
                for event in list(events):
                    if event.get('method')!='Page.javascriptDialogOpening':continue
                    events.remove(event);prompted=True
                    assert event['params']['type']=='beforeunload',event
                    if cancel:
                        native.send(json.dumps({'kind':'beforeunload-cancel','id':ident,'target':target}).encode())
                    call('Page.handleJavaScriptDialog',{'accept':bool(accept)},session)
                    if cancel:
                        time.sleep(.1)
                        assert call('Runtime.evaluate',{'expression':'location.href','returnByValue':True},session)['result']['value'].startswith('data:'), 'cancel guard destroyed page'
                        return None,prompted
            result=native_replies.pop(ident)
            assert result['target']==target and not result['error'],result
            return result,prompted
        first,prompt=check(1)
        assert first['proceed'] and not prompt,(first,prompt)
        call('Runtime.evaluate',{'expression':"window.nativeChecks=0;window.onbeforeunload=e=>{window.nativeChecks++;e.preventDefault();e.returnValue='draft';};true",'returnByValue':True},session)
        for kind in ['mousePressed','mouseReleased']:
            call('Input.dispatchMouseEvent',{'type':kind,'x':40,'y':40,'button':'left','clickCount':1},session)
        denied,prompt=check(2,False)
        assert prompt and not denied['proceed'],(denied,prompt)
        assert call('Runtime.evaluate',{'expression':'nativeChecks','returnByValue':True},session)['result']['value']==1
        allowed,prompt=check(3,True)
        assert prompt and allowed['proceed'],(allowed,prompt)
        assert call('Runtime.evaluate',{'expression':'nativeChecks','returnByValue':True},session)['result']['value']==2, 'accepted preflight unloaded page'
        _,prompt=check(4,True,cancel=True)
        assert prompt
        again,prompt=check(5,False)
        assert prompt and not again['proceed']
        call('Runtime.evaluate',{'expression':'window.onbeforeunload=null','returnByValue':True},session)
        final,prompt=check(6)
        assert final['proceed'] and not prompt
        print('NATIVE_CLOSE_PASS real handlers, Cancel/Leave decisions, non-destructive approval, canceled late ACK and repeated checks',flush=True)
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
