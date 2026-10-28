#!/usr/bin/env python3
"""Exercise context-menu media actions in a real sandboxed Brave isolated world."""
import argparse
import base64
import io
import json
import os
from pathlib import Path
import resource
import select
import signal
import subprocess
import tempfile
import time
import urllib.parse
import wave


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', required=True, help='Pinned Brave launcher')
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    source = (Path(__file__).resolve().parents[1] / 'data/native-integration/context-menu.js').read_text()
    audio = io.BytesIO()
    with wave.open(audio, 'wb') as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(8000)
        wav.writeframes(b'\0\0' * 8000 * 10)
    audio_url = 'data:audio/wav;base64,' + base64.b64encode(audio.getvalue()).decode()
    html = '<!doctype html><title>Context media fixture</title><audio id="audio" style="position:absolute;left:20px;top:20px;width:300px;height:60px" controls src="' + audio_url + '"></audio><video id="video" style="position:absolute;left:20px;top:120px;width:300px;height:180px"></video>'
    child_read, parent_write = os.pipe()
    parent_read, child_write = os.pipe()

    def child_setup():
        os.dup2(child_read, 3)
        os.dup2(child_write, 4)

    with tempfile.TemporaryDirectory(prefix='gnosis-context-media-') as profile:
        env = os.environ.copy()
        env.pop('GNOSIS_NATIVE_UI_FD', None)
        env.pop('GNOSIS_NATIVE_UI_SHIM', None)
        with open(profile + '/browser.log', 'w+') as log:
            process = subprocess.Popen([args.browser, '--headless=new', '--mute-audio', '--no-first-run',
                '--no-default-browser-check', '--remote-debugging-pipe', '--user-data-dir=' + profile,
                'about:blank'], env=env, pass_fds=(child_read, parent_write, parent_read, child_write),
                preexec_fn=child_setup, start_new_session=True, stdout=subprocess.DEVNULL, stderr=log)
            os.close(child_read)
            os.close(child_write)
            buffer = b''
            sequence = 0
            replies = {}
            events = []

            def receive(timeout=15):
                nonlocal buffer
                until = time.monotonic() + timeout
                while b'\0' not in buffer:
                    remaining = until - time.monotonic()
                    if remaining <= 0 or not select.select([parent_read], [], [], remaining)[0]:
                        raise TimeoutError('Browser protocol timed out')
                    chunk = os.read(parent_read, 65536)
                    if not chunk:
                        raise EOFError('Browser protocol closed')
                    buffer += chunk
                raw, buffer = buffer.split(b'\0', 1)
                value = json.loads(raw)
                if 'id' in value:
                    replies[value['id']] = value
                else:
                    events.append(value)
                return value

            def call(method, params=None, session=None):
                nonlocal sequence
                sequence += 1
                ident = sequence
                message = {'id': ident, 'method': method, 'params': params or {}}
                if session:
                    message['sessionId'] = session
                os.write(parent_write, json.dumps(message).encode() + b'\0')
                while ident not in replies:
                    receive()
                result = replies.pop(ident)
                if 'error' in result:
                    raise RuntimeError(result['error'])
                return result.get('result', {})

            try:
                print('BROWSER', call('Browser.getVersion')['product'], flush=True)
                target = next(t['targetId'] for t in call('Target.getTargets')['targetInfos'] if t['type'] == 'page')
                session = call('Target.attachToTarget', {'targetId': target, 'flatten': True})['sessionId']
                call('Page.enable', session=session)
                call('Runtime.enable', session=session)
                call('Runtime.addBinding', {'name': '__gnosisContextMenu', 'executionContextName': 'GnosisContextMenu'}, session)
                call('Page.addScriptToEvaluateOnNewDocument', {'worldName': 'GnosisContextMenu', 'source': source}, session)
                call('Page.navigate', {'url': 'data:text/html;charset=utf-8,' + urllib.parse.quote(html)}, session)

                def evaluate(expression, context=None):
                    params = {'expression': expression, 'returnByValue': True, 'awaitPromise': True, 'userGesture': True}
                    if context:
                        params['uniqueContextId'] = context
                    result = call('Runtime.evaluate', params, session)
                    if 'exceptionDetails' in result:
                        raise RuntimeError(result['exceptionDetails'])
                    return result['result'].get('value')

                deadline = time.monotonic() + 15
                while not evaluate("!!document.getElementById('audio') && document.getElementById('audio').readyState >= 2"):
                    assert time.monotonic() < deadline, 'Audio fixture did not load'
                    time.sleep(.05)
                assert evaluate("typeof globalThis.__gnosisContextMenu + ':' + typeof globalThis.__gnosisContextMenuAction") == 'undefined:undefined'
                before = len([e for e in events if e.get('method') == 'Runtime.bindingCalled'])
                evaluate("document.getElementById('audio').dispatchEvent(new MouseEvent('contextmenu',{bubbles:true,button:2,clientX:30,clientY:30}))")
                assert len([e for e in events if e.get('method') == 'Runtime.bindingCalled']) == before

                def context_at(x, y):
                    offset = len(events)
                    for kind, buttons in [('mousePressed', 2), ('mouseReleased', 0)]:
                        call('Input.dispatchMouseEvent', {'type': kind, 'button': 'right', 'buttons': buttons,
                            'x': x, 'y': y, 'clickCount': 1}, session)
                    deadline = time.monotonic() + 10
                    while True:
                        for event in events[offset:]:
                            if event.get('method') != 'Runtime.bindingCalled' or event['params'].get('name') != '__gnosisContextMenu':
                                continue
                            value = json.loads(event['params']['payload'])
                            number = event['params']['executionContextId']
                            context = next(e['params']['context']['uniqueId'] for e in events
                                if e.get('method') == 'Runtime.executionContextCreated' and e['params']['context']['id'] == number)
                            return value, context
                        assert time.monotonic() < deadline, 'Trusted context event was not received'
                        receive()

                hit, context = context_at(100, 35)
                assert hit['media']['kind'] == 'audio', hit
                assert hit['media']['paused'] is True

                def action(token, operation, world=context):
                    return evaluate('globalThis.__gnosisContextMenuAction(' + json.dumps(token) + ',' + json.dumps(operation) + ')', world)

                assert action('wrong', 'play') is False
                assert action(hit['target'], 'loop') is True
                assert action(hit['target'], 'play') is True
                assert evaluate("!document.getElementById('audio').paused && document.getElementById('audio').loop") is True
                assert action(hit['target'], 'mute') is True
                assert evaluate("document.getElementById('audio').muted") is True
                assert action(hit['target'], 'hide-controls') is True
                assert evaluate("document.getElementById('audio').controls") is False
                assert action(hit['target'], 'pause') is True
                assert evaluate("document.getElementById('audio').paused") is True
                video, next_context = context_at(60, 150)
                assert next_context == context and video['media']['kind'] == 'video', video
                assert action(hit['target'], 'play') is False, 'Superseded audio menu must not control audio'
                assert action(video['target'], 'show-controls') is True
                assert evaluate("document.getElementById('video').controls") is True
                evaluate("document.getElementById('video').remove()")
                assert action(video['target'], 'mute') is False, 'Removed video target must be rejected'
                call('Page.navigate', {'url': 'data:text/html,<p>Replacement document</p>'}, session)
                deadline = time.monotonic() + 10
                while evaluate('document.body.textContent') != 'Replacement document':
                    assert time.monotonic() < deadline
                    time.sleep(.05)
                try:
                    action(video['target'], 'pause')
                except RuntimeError:
                    pass
                else:
                    raise AssertionError('Destroyed document context unexpectedly accepted a command')
                print('CONTEXT_MEDIA_NATIVE_PASS trusted hit, hidden binding, native audio playback, media state, stale target/document rejection', flush=True)
            except BaseException:
                log.flush()
                log.seek(0)
                print(log.read()[-3000:])
                raise
            finally:
                try:
                    call('Browser.close')
                except Exception:
                    pass
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                os.close(parent_write)
                os.close(parent_read)


if __name__ == '__main__':
    main()
