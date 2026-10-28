#!/usr/bin/env python3
"""Check the pinned native theme with sandboxing and no injected scrollbar styles.

Requires ImageMagick on PATH. Captures are kept for visual review.
"""
import argparse,base64,json,os,select,subprocess,tempfile,time,urllib.parse,sys
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--browser',required=True,type=Path)
parser.add_argument('--output-dir',type=Path,default=Path('/tmp/gnosis-native-scrollbars-check'))
args=parser.parse_args();args.output_dir.mkdir(parents=True,exist_ok=True)
browser=str(args.browser.resolve())
with tempfile.TemporaryDirectory(prefix='gnosis-native-scrollbar-') as root:
 a,b=os.pipe();c,d=os.pipe()
 def setup():os.dup2(a,3);os.dup2(d,4)
 p=subprocess.Popen([browser,'--headless=new','--no-first-run','--no-default-browser-check','--enable-features=OverlayScrollbar:mode/overlay','--remote-debugging-pipe','--user-data-dir='+root,'--window-size=900,700','about:blank'],pass_fds=tuple(set((a,b,c,d,3,4))),preexec_fn=setup,stdout=subprocess.DEVNULL,stderr=(args.output_dir/'browser.log').open('w'),start_new_session=True)
 os.close(a);os.close(d);buf=b'';seq=0;replies={}
 def call(method,params={},session=None):
  global seq,buf
  seq+=1;ident=seq;msg=dict(id=seq,method=method,params=params)
  if session:msg['sessionId']=session
  os.write(b,json.dumps(msg).encode()+b'\0');end=time.monotonic()+15
  while ident not in replies:
   while b'\0' not in buf:
    if not select.select([c],[],[],max(0,end-time.monotonic()))[0]:raise TimeoutError(method)
    raw=os.read(c,65536)
    if not raw:raise EOFError((p.poll(),method))
    buf+=raw
   raw,buf=buf.split(b'\0',1);v=json.loads(raw)
   if 'id' in v:replies[v['id']]=v
  v=replies.pop(ident)
  if 'error' in v:raise RuntimeError(v)
  return v.get('result',{})
 try:
  end=time.monotonic()+15
  while True:
   pages=[t for t in call('Target.getTargets')['targetInfos'] if t['type']=='page']
   if pages:target=pages[0];break
   assert time.monotonic()<end;time.sleep(.05)
  s=call('Target.attachToTarget',dict(targetId=target['targetId'],flatten=True))['sessionId']
  call('Page.enable',{},s);call('Runtime.enable',{},s)
  def ev(js):return call('Runtime.evaluate',dict(expression=js,returnByValue=True),s)['result'].get('value')
  def mouse(typ,x,y,**kw):call('Input.dispatchMouseEvent',dict(type=typ,x=x,y=y,**kw),s)
  def shot(name):
   path=args.output_dir/(name+'.png')
   path.write_bytes(base64.b64decode(call('Page.captureScreenshot',{},s)['data']))
   return path
  def track_width(path,x,y):
   raw=subprocess.check_output(['magick',str(path),'-crop',f'24x1+{x}+{y}','-depth','8','RGB:-'])
   pixels=[tuple(raw[i:i+3]) for i in range(0,len(raw),3)]
   # Unstyled panels have the same dark background; a 10%-white track is ~54.
   marked=[i for i,pixel in enumerate(pixels) if 49<=pixel[0]<=65 and max(pixel)-min(pixel)<8]
   assert marked and marked==list(range(min(marked),max(marked)+1)),pixels
   return len(marked)
  html='<!doctype html><style>body{margin:0;background:#202124;color:white;font:18px sans-serif}section{height:180px;width:240px;overflow:auto;background:#202124;position:fixed;top:80px}section div{height:1800px;width:1500px}#normal{left:40px}#thin{left:340px;scrollbar-width:thin}#custom{left:640px;scrollbar-color:rgb(255,0,180) transparent}</style><h1>Native scrollbars</h1><section id=normal><div>Normal</div></section><section id=thin><div>Thin</div></section><section id=custom><div>Author color</div></section>'+('<p>Main content</p>'*200)
  call('Page.navigate',dict(url='data:text/html,'+urllib.parse.quote(html)),s)
  deadline=time.monotonic()+10
  while not ev("Boolean(document.querySelector('#thin'))"):
   assert time.monotonic()<deadline;time.sleep(.05)
  size=ev('({w:innerWidth,h:innerHeight,client:document.documentElement.clientWidth})');print(size,flush=True)
  assert size['w']==size['client'],size
  ev('scrollTo(0,100)');time.sleep(.2);shot('idle')
  mouse('mouseMoved',size['w']-12,25);time.sleep(.35)
  mouse('mousePressed',size['w']-12,25,button='left',buttons=1,clickCount=1)
  mouse('mouseMoved',size['w']-12,220,button='left',buttons=1)
  mouse('mouseReleased',size['w']-12,220,button='left',buttons=0,clickCount=1)
  time.sleep(.1);offset=ev('scrollY');print('native thumb drag:',offset,flush=True);assert offset>500
  mouse('mouseMoved',size['w']-12,440);time.sleep(.35)
  assert track_width(shot('root'),size['w']-24,440)==8
  mouse('mouseMoved',150,150);mouse('mouseWheel',150,150,deltaX=0,deltaY=80);time.sleep(.15)
  mouse('mouseMoved',268,150);time.sleep(.35)
  normal=track_width(shot('normal'),256,210)
  mouse('mouseMoved',450,150);mouse('mouseWheel',450,150,deltaX=0,deltaY=80);time.sleep(.15)
  mouse('mouseMoved',568,150);time.sleep(.35)
  thin=track_width(shot('thin'),556,210)
  assert normal==thin==8,(normal,thin)
  # Horizontal, nested scrollbar hit testing and native thumb dragging.
  mouse('mouseMoved',70,248);time.sleep(.35)
  mouse('mousePressed',70,248,button='left',buttons=1,clickCount=1)
  mouse('mouseMoved',190,248,button='left',buttons=1)
  mouse('mouseReleased',190,248,button='left',buttons=0,clickCount=1)
  assert ev("document.querySelector('#normal').scrollLeft")>0
  shot('horizontal')
  mouse('mouseMoved',750,150);mouse('mouseWheel',750,150,deltaX=0,deltaY=80);time.sleep(.15)
  mouse('mouseMoved',868,100);time.sleep(.35)
  custom=shot('author-color')
  raw=subprocess.check_output(['magick',str(custom),'-crop','24x160+856+80','-depth','8','RGB:-'])
  assert any(raw[i]>150 and raw[i+1]<60 and raw[i+2]>90 for i in range(0,len(raw),3))
  call('Emulation.setEmulatedMedia',dict(features=[dict(name='forced-colors',value='active')]),s)
  ev('scrollTo(0,600)');mouse('mouseMoved',800,400);mouse('mouseWheel',800,400,deltaX=0,deltaY=40);time.sleep(.15)
  mouse('mouseMoved',size['w']-12,70);time.sleep(.35)
  forced=shot('forced-colors')
  raw=subprocess.check_output(['magick',str(forced),'-crop',f'8x500+{size["w"]-16}+10','-depth','8','RGB:-'])
  assert any(raw[i:i+3]==b'\0\0\0' for i in range(0,len(raw),3)), 'Forced-color thumb must remain visible'
  # Remaining GTK colors can be installed transiently, even under strict CSP.
  call('Emulation.setEmulatedMedia',dict(features=[]),s)
  ev("let meta=document.createElement('meta');meta.httpEquiv='Content-Security-Policy';meta.content=\"default-src 'none';style-src 'none'\";document.head.append(meta)")
  call('DOM.enable',{},s);call('CSS.enable',{},s)
  frame=call('Page.getFrameTree',{},s)['frameTree']['frame']['id']
  sheet=call('CSS.createStyleSheet',dict(frameId=frame,force=True),s)['styleSheetId']
  call('CSS.setStyleSheetText',dict(styleSheetId=sheet,text='h1{color:rgb(123,45,67)!important}'),s)
  call('CSS.disable',{},s);call('DOM.disable',{},s)
  assert ev("getComputedStyle(document.querySelector('h1')).color")=='rgb(123, 45, 67)'
  call('DOM.enable',{},s);call('CSS.enable',{},s)
  call('CSS.setStyleSheetText',dict(styleSheetId=sheet,text='h1{color:rgb(98,76,54)!important}'),s)
  call('CSS.disable',{},s);call('DOM.disable',{},s)
  assert ev("getComputedStyle(document.querySelector('h1')).color")=='rgb(98, 76, 54)'
  print('PASS: 8px root/normal/thin tracks, real thumb dragging on both axes, author colors, forced colors, transient strict-CSP styles.',flush=True)

 finally:
  try:call('Browser.close')
  except Exception:pass
  try:p.wait(timeout=5)
  except subprocess.TimeoutExpired:p.terminate();p.wait(timeout=5)
  os.close(b);os.close(c)
