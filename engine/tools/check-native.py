#!/usr/bin/env python3
"""Run the actual GTK/CEF view against a local page and favicon, with an isolated display/profile."""
import argparse
import base64
import hashlib
import re
import urllib.parse
import resource
import signal
import struct
import zlib
import html
import http.server
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time

# Assertion failures in disposable Chromium tests must not write multi-GB cores.
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=Path)
parser.add_argument("--desktop", action="store_true", help="Use the current desktop display and GPU")
portal_mode = parser.add_mutually_exclusive_group()
portal_mode.add_argument("--portals", action="store_true", help="Test real file portals using the GTK portal backend")
portal_mode.add_argument("--open-uri", action="store_true", help="Test the actual browser's external-link dialog and real OpenURI portal")
parser.add_argument("--timeout", type=int, help="Deadline for workflows that reopen the browser several times")
parser.add_argument("browser_args", nargs=argparse.REMAINDER)
args = parser.parse_args()

class Fixture(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path != '/settings-report':
            self.send_error(404)
            return
        report = json.loads(self.rfile.read(min(int(self.headers.get('Content-Length', '0')), 8192)))
        report.update(httpUserAgent=self.headers.get('User-Agent', ''),
                      httpLanguage=self.headers.get('Accept-Language', ''))
        with self.server.settings_lock:
            self.server.settings_reports.append(report)
            del self.server.settings_reports[:-100]
        self.send_response(204)
        self.end_headers()

    def do_GET(self):
        auth_url = urllib.parse.urlsplit(self.path)
        if auth_url.path in ('/auth/basic', '/auth/digest', '/auth/proxy'):
            proxy = auth_url.path == '/auth/proxy'
            digest = auth_url.path == '/auth/digest'
            realm = urllib.parse.parse_qs(auth_url.query).get('realm', ['Gnosis authentication'])[0]
            # Fixed fixture credentials; observations deliberately omit credential values.
            username, password, nonce = 'gnosis-user', 'fixture-password', 'gnosis-native-auth-nonce'
            authorization = self.headers.get('Proxy-Authorization' if proxy else 'Authorization', '')
            valid = authorization == 'Basic ' + base64.b64encode(f'{username}:{password}'.encode()).decode()
            if digest:
                fields = {}
                for match in re.finditer(r'(\w+)=(?:"([^"]*)"|([^,\s]+))', authorization[7:]):
                    fields[match[1]] = match[2] if match[2] is not None else match[3]
                md5 = lambda text: hashlib.md5(text.encode()).hexdigest()
                ha1 = md5(f'{username}:{realm}:{password}')
                ha2 = md5(f'GET:{fields.get("uri", "")}')
                expected = md5(f'{ha1}:{nonce}:{fields.get("nc", "")}:{fields.get("cnonce", "")}:{fields.get("qop", "")}:{ha2}')
                valid = (authorization.startswith('Digest ') and fields.get('username') == username and
                         fields.get('realm') == realm and fields.get('nonce') == nonce and
                         fields.get('qop') == 'auth' and fields.get('uri') == self.path and
                         fields.get('response') == expected)
            with self.server.settings_lock:
                self.server.settings_reports.append({'httpAuth': auth_url.path, 'realm': realm,
                    'authorized': valid, 'authorizationScheme': authorization.split(' ', 1)[0]})
            body = (f'<!doctype html><title>Authenticated {realm}</title><h1 id="authenticated">'
                    f'Authenticated {html.escape(realm)}</h1>').encode() if valid else b'Authentication required'
            self.send_response(200 if valid else 407 if proxy else 401)
            if not valid:
                challenge = f'Digest realm="{realm}", nonce="{nonce}", algorithm=MD5, qop="auth"' if digest else f'Basic realm="{realm}"'
                self.send_header('Proxy-Authenticate' if proxy else 'WWW-Authenticate', challenge)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path.startswith('/open-uri-fixture?'):
            body = Path(__file__).resolve().parents[1].joinpath('tests/fixtures/open-uri.html').read_bytes()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path.startswith('/clipboard-read-resource'):
            with self.server.settings_lock:
                self.server.settings_reports.append({'clipboardReadFetch': self.path})
            self.send_response(204)
            self.end_headers()
            return
        if self.path == '/page-retry-enable':
            with self.server.settings_lock:
                self.server.page_retry_ready = True
            self.send_response(204)
            self.end_headers()
            return
        if self.path == '/page-retry-fixture':
            with self.server.settings_lock:
                ready = getattr(self.server, 'page_retry_ready', False)
                self.server.settings_reports.append({'pageRetryAttempt': True, 'ready': ready})
            if not ready:
                self.close_connection = True
                return
            body = b'''<!doctype html><title>Recovered through native Retry</title><h1>Recovered</h1><script>
              fetch('/settings-report',{method:'POST',body:JSON.stringify({pageRetryRecovered:true,internalAPI:typeof Gnosis})});
            </script>'''
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path.startswith('/permission-fixture') or self.path == '/permission-child':
            body = '''<!doctype html><title>Native permissions</title>
              <button id=notify style="position:fixed;left:20px;top:20px;width:400px;height:80px">Notifications</button>
              <button id=clipboard style="position:fixed;left:20px;top:120px;width:400px;height:80px">Read clipboard</button>
              <script>
              window.outcome='idle';
              window.childReady=false;
              window.permissionStates={};
              for(const name of ['notifications','geolocation','microphone','camera','clipboard-read'])
                navigator.permissions.query({name}).then(status=>{
                  permissionStates[name]=status.state;
                  status.onchange=()=>permissionStates[name]=status.state;
                }).catch(error=>permissionStates[name]=error.name);
              window.ask=async constraints=>{
                window.outcome='pending';
                try {const stream=await navigator.mediaDevices.getUserMedia(constraints);
                  stream.getTracks().forEach(t=>t.stop());window.outcome='allowed';
                }catch(error){window.outcome=error.name;}
                if(parent!==window) parent.postMessage({outcome},'*');
              };
              notify.onclick=()=>{window.outcome='pending';Notification.requestPermission().then(value=>window.outcome=value)};
              clipboard.onclick=async event=>{window.outcome='pending';try{
                await navigator.clipboard.writeText('GTK clipboard from native dialog');
                if(event.shiftKey){
                  const items=await navigator.clipboard.read({types:['text/plain']});
                  window.clipboardValue=await (await items[0].getType('text/plain')).text();
                }else window.clipboardValue=await navigator.clipboard.readText();
                window.outcome='read';
              }catch(error){window.outcome=error.name}};
              setInterval(()=>fetch('/settings-report',{method:'POST',body:JSON.stringify({permissionPage:location.host,testId:location.search,outcome,clipboardValue:window.clipboardValue,notificationState:Notification.permission,permissionStates})}).catch(()=>{}),250);
              addEventListener('message',event=>{
                if(parent!==window && event.source===parent && event.data==='microphone') ask({audio:true});
                if(parent===window && event.data && event.data.outcome) window.outcome=event.data.outcome;
                if(parent===window && event.source===document.querySelector('iframe')?.contentWindow && event.data?.childReady) window.childReady=true;
              });
              if(parent!==window) parent.postMessage({childReady:true},'*');
              </script>'''
            if self.path.startswith('/permission-fixture'):
                body += '<iframe allow="microphone *; camera *" src="http://localhost:'+str(self.server.server_port)+'/permission-child"></iframe>'
            body = body.encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.send_header('Permissions-Policy', 'microphone=(*), camera=(*)')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == '/obsolete-filter-fixture':
            with self.server.settings_lock:
                self.server.settings_reports.append({'legacyFilter': True})
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', '2')
            self.end_headers()
            self.wfile.write(b'[]')
            return
        if self.path.startswith('/session-fixture?'):
            tab = self.path.split('?', 1)[1]
            body = ('<!doctype html><title>Saved tab '+tab+'</title><h1>Restored '+tab+'</h1>'
                    '<script>fetch("/settings-report",{method:"POST",body:JSON.stringify({tab:location.search})})</script>').encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path.startswith('/script-ui-fixture'):
            if 'slow=1' in self.path:
                with self.server.settings_lock:
                    self.server.settings_reports.append({'readerSlowRequested': True})
                time.sleep(20)
            paragraph = ('Browser integration needs actual data from the rendering engine, '
                         'including selected text, article metadata, and nested values. '
                         'This paragraph provides enough readable text to exercise the real '
                         'article parser while keeping the surrounding application controls separate. ')
            body = ('''<!doctype html><title>Native result article</title>
              <style>body{font:18px sans-serif;margin:20px}article{padding-top:200px;max-width:720px}
              button,input{position:fixed;left:20px;width:400px;height:60px}button{top:20px}input{top:100px}</style>
              <button id=selectText>Select phrase</button><input id=proof placeholder="Paste proof">
              <article><h1>Native result article</h1><p><span id=phrase>Gnosis native results</span></p><p id=live></p>''' +
                    '<p>Reader session cookie: '+html.escape(self.headers.get('Cookie', 'none'))+'</p>'+
                    ''.join('<p>'+paragraph+'</p>' for _ in range(12)) + '''</article><script>
              document.cookie='reader_scope='+(new URLSearchParams(location.search).get('scope')||'regular')+';SameSite=Lax;Path=/';
              selectText.onclick=()=>{live.textContent='Live DOM edit preserved by reader extraction';const range=document.createRange();range.selectNodeContents(phrase);
                const selection=getSelection();selection.removeAllRanges();selection.addRange(range)};
              setInterval(()=>fetch('/settings-report',{method:'POST',body:JSON.stringify({scriptUI:true,text:proof.value})}).catch(()=>{}),250);
              </script>''').encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path.startswith('/script-fixture'):
            body = ('<!doctype html><title>Script worlds</title><h1>Native script fixture</h1>'
                    '<iframe src="http://localhost:'+str(self.server.server_port)+'/child"></iframe>').encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.send_header('Content-Security-Policy', "default-src 'self'; script-src 'none'; frame-src http://localhost:"+str(self.server.server_port))
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == '/cookies-ui-fixture':
            body = b'''<!doctype html><title>Gnosis cookies fixture</title><p id=cookies>Waiting for cookies</p><script>
              document.cookie='pageCookie='+location.hostname+'; SameSite=Lax; Path=/';
              localStorage.setItem('keepStorage','preserved');
              setInterval(()=>fetch('/echo-cookie').then(r=>r.text()).then(httpCookies=>{
                cookies.textContent=httpCookies;return fetch('/settings-report',{method:'POST',body:JSON.stringify({
                  domain:location.hostname,httpCookies,jsCookies:document.cookie,storage:localStorage.getItem('keepStorage')})});
              }).catch(()=>{}),250);
            </script>'''
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.send_header('Set-Cookie', 'httpCookie='+self.headers['Host'].split(':')[0]+'; HttpOnly; SameSite=Lax; Path=/')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == '/cache-ui-fixture':
            body = b'''<!doctype html><title>Gnosis cache fixture</title><p id=cache>Waiting for cache</p><script>
              setInterval(()=>fetch('/cache-fixture').then(r=>r.text()).then(text=>{
                cache.textContent=text;return fetch('/settings-report',{method:'POST',body:JSON.stringify({cache:text})});
              }).catch(()=>{}),250);
            </script>'''
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == '/cache-fixture':
            with self.server.settings_lock:
                self.server.cache_requests += 1
                body = ('cached response '+str(self.server.cache_requests)).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain')
            self.send_header('Cache-Control', 'public, max-age=3600')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == '/set-cookie':
            self.send_response(200)
            self.send_header('Set-Cookie', 'networkCookie=allowed; SameSite=Lax; Path=/')
            self.send_header('Content-Type', 'text/plain')
            self.send_header('Content-Length', '3')
            self.end_headers()
            self.wfile.write(b'set')
            return
        if self.path == '/settings-reports':
            with self.server.settings_lock:
                body = json.dumps(self.server.settings_reports).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == '/echo-headers':
            body = json.dumps({'userAgent': self.headers.get('User-Agent', ''),
                               'language': self.headers.get('Accept-Language', '')}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path.startswith('/settings-fixture'):
            headers = json.dumps({'userAgent': self.headers.get('User-Agent', ''),
                                  'language': self.headers.get('Accept-Language', '')}).replace('<', '\\u003c')
            reporter = ''
            if self.path.endswith('?report'):
                reporter = '''setInterval(()=>fetch('/settings-report',{method:'POST',body:JSON.stringify({
                  agent:navigator.userAgent,language:navigator.language,
                  size:getComputedStyle(sample).fontSize,family:getComputedStyle(sample).fontFamily,
                  initialHeaders,webgl:!!document.createElement('canvas').getContext('webgl')
                })}).catch(()=>{}),200);'''
            body = ('<!doctype html><title>Settings fixture</title><p id=sample>Native preferences</p>'
                    '<pre id=fixed>Monospace</pre><input spellcheck=true>'
                    '<script>window.initialHeaders=' + headers + ';' + reporter + '</script>').encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path.startswith(('/response-hold?', '/response-hold-final?')):
            query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
            key = query.get('case', [''])[0]
            if not re.fullmatch(r'[a-z0-9-]{1,80}', key):
                self.send_error(400)
                return
            marker = self.server.response_lifetimes / key
            if self.path.startswith('/response-hold?'):
                marker.with_suffix('.started').write_text(self.path)
                deadline = time.monotonic() + 15
                while not marker.with_suffix('.release').exists() and time.monotonic() < deadline:
                    time.sleep(.01)
                if not marker.with_suffix('.release').exists():
                    marker.with_suffix('.finished').write_text('unreleased')
                    self.send_error(504)
                    return
                if query.get('redirect') == ['1']:
                    self.send_response(302)
                    self.send_header('Location', '/response-hold-final?case=' + key)
                    self.send_header('Content-Length', '0')
                    self.end_headers()
                    marker.with_suffix('.redirected').write_text('302')
                    return
            body = b'<!doctype html><title>Released resource response</title><p>Actual delayed HTTP body</p>'
            sent = False
            try:
                self.send_response(200)
                self.send_header('Content-Type', 'text/html; charset=utf-8')
                self.send_header('Content-Length', str(len(body)))
                self.send_header('Cache-Control', 'no-store')
                self.end_headers()
                self.wfile.write(body)
                self.wfile.flush()
                sent = True
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                marker.with_suffix('.finished').write_text('sent' if sent else 'cancelled')
            return
        if self.path.startswith('/response-'):
            if self.path == '/response-redirect':
                self.send_response(302)
                self.send_header('Location', '/response-document')
                self.end_headers()
                return
            body = b'<!doctype html><title>Response metadata</title><h1>Original response</h1><iframe src="/response-child"></iframe>'
            if self.path == '/response-child':
                body = b'<p>Subframe response</p>'
            self.send_response(404 if self.path == '/response-missing' else 200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Content-Disposition', "inline; filename*=UTF-8''report%20%C3%A9.html")
            self.send_header('X-Gnosis-Fixture', 'actual-response')
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path.startswith('/download'):
            if self.path == '/download-pending':
                time.sleep(10)
            if self.path == '/download-redirect':
                self.send_response(302)
                self.send_header('Location', '/download')
                self.end_headers()
                return
            if self.path == '/download-fail':
                self.send_error(404)
                return
            body = self.headers.get('Cookie', '').encode() if self.path == '/download-cookie' else b'CEF download payload\x00with binary bytes'
            if self.path == '/download-headers':
                body = json.dumps({'userAgent': self.headers.get('User-Agent', ''),
                                   'language': self.headers.get('Accept-Language', '')}).encode()
            if self.path == '/download-slow':
                body = b'x' * (512 * 1024)
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Disposition', 'attachment; filename="cef-fixture.bin"')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            try:
                for start in range(0, len(body), 8192):
                    self.wfile.write(body[start:start + 8192])
                    self.wfile.flush()
                    if self.path == '/download-slow':
                        time.sleep(.03)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        if self.path == "/echo-cookie":
            body = self.headers.get("Cookie", "").encode()
            mime = "text/plain"
        elif self.path == "/favicon.png":
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

with tempfile.TemporaryDirectory(prefix="gnosis-native-check-") as scratch:
    root = Path(scratch)
    display_file = root / "display"
    with display_file.open("w") as display_out:
        display = subprocess.Popen(["Xvfb", "-displayfd", str(display_out.fileno()), "-screen", "0", "1000x750x24", "-nolisten", "tcp", "-ac"], pass_fds=(display_out.fileno(),))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
    server.response_lifetimes = root / 'response-lifetimes'
    server.response_lifetimes.mkdir()
    server.settings_reports = []
    server.cache_requests = 0
    server.settings_lock = threading.Lock()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        deadline = time.monotonic() + 5
        while not display_file.read_text().strip():
            if display.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("Xvfb failed to start")
            time.sleep(.05)
        bus = root / "bus.conf"
        bus.write_text('<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth><policy context="default"><allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/></policy></busconfig>')
        env = dict(os.environ, DISPLAY=":" + display_file.read_text().strip(), GDK_BACKEND="x11", GTK_A11Y="none", GIO_USE_VFS="local", GTK_USE_PORTAL="0", GNOSIS_SOFTWARE_RENDERING="1", GNOSIS_TEST_URL=f"http://127.0.0.1:{server.server_port}/")
        if args.desktop:
            for key in ("DISPLAY", "WAYLAND_DISPLAY", "GDK_BACKEND", "GNOSIS_SOFTWARE_RENDERING"):
                env.pop(key, None)
                if key in os.environ:
                    env[key] = os.environ[key]
        else:
            env.pop("WAYLAND_DISPLAY", None)
        env['GNOSIS_RESPONSE_FIXTURE_DIR'] = str(server.response_lifetimes)
        command = [str(args.binary.resolve()), *args.browser_args]
        if args.portals or args.open_uri:
            env.update(GTK_USE_PORTAL="1", GDK_DEBUG="portals", GNOSIS_TEST_SCRATCH=str(root),
                       XDG_CONFIG_HOME=str(root / "config"), XDG_CACHE_HOME=str(root / "cache"),
                       XDG_DATA_HOME=str(root / "data"))
            wrapper = "check-open-uri.py" if args.open_uri else "check-portals.py"
            command = ["python3", str(Path(__file__).with_name(wrapper).resolve()), *command]
        process = subprocess.Popen(["dbus-run-session", "--config-file=" + str(bus), "--", *command], env=env, start_new_session=True)
        try:
            status = process.wait(timeout=args.timeout if args.timeout else (240 if args.open_uri else 90 if args.portals else 45))
            if status:
                raise subprocess.CalledProcessError(status, process.args)
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
    finally:
        server.shutdown()
        server.server_close()
        display.terminate()
        try:
            display.wait(timeout=5)
        except subprocess.TimeoutExpired:
            display.kill()
            display.wait()
