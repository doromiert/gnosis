#!/usr/bin/env python3
"""Verify full uBlock identity/storage through a renamed profile's directory redirect."""
import os,subprocess,tempfile,json,select,time,socket,shutil
from pathlib import Path
import argparse
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--browser',type=Path,required=True)
parser.add_argument('--ublock',type=Path,required=True)
args=parser.parse_args()
browser=str(args.browser.resolve())
class Browser:
 def __init__(self,profile):
  a,self.input=os.pipe();self.output,d=os.pipe();self.parent,self.child=socket.socketpair(socket.AF_UNIX,socket.SOCK_SEQPACKET)
  def setup():os.dup2(a,3);os.dup2(d,4);os.dup2(self.child.fileno(),5)
  self.p=subprocess.Popen([browser,'--headless=new','--no-first-run','--no-default-browser-check','--enable-unsafe-extension-debugging','--remote-debugging-pipe','--user-data-dir='+str(profile),'about:blank'],env=dict(os.environ,GNOSIS_NATIVE_UI_FD='5'),pass_fds=tuple(set((a,self.input,self.output,d,3,4,5,self.child.fileno()))),preexec_fn=setup,stdout=subprocess.DEVNULL,stderr=open('/tmp/gnosis-rebrand-extension-check.log','a'))
  os.close(a);os.close(d);self.buf=b'';self.seq=0
 def call(self,method,params={},session=None):
  self.seq+=1;message=dict(id=self.seq,method=method,params=params)
  if session:message['sessionId']=session
  os.write(self.input,json.dumps(message).encode()+b'\0');end=time.monotonic()+15
  while True:
   while b'\0' not in self.buf:
    assert select.select([self.output],[],[],max(0,end-time.monotonic()))[0],method
    data=os.read(self.output,65536);assert data,method;self.buf+=data
   raw,self.buf=self.buf.split(b'\0',1);v=json.loads(raw)
   if v.get('id')==self.seq:
    assert 'error' not in v,v;return v.get('result',{})
 def background(self,id):
  end=time.monotonic()+15
  while True:
   candidates=[t for t in self.call('Target.getTargets')['targetInfos'] if t['url'].startswith('chrome-extension://'+id+'/') and t['type'] in ['background_page','service_worker']]
   if candidates:
    session=self.call('Target.attachToTarget',dict(targetId=candidates[0]['targetId'],flatten=True))['sessionId']
    self.call('Runtime.enable',{},session);self.call('Runtime.runIfWaitingForDebugger',{},session)
    while not self.evaluate("Boolean(globalThis.chrome?.storage?.local)",session):
     assert time.monotonic()<end,'extension context unavailable';time.sleep(.1)
    return session
   assert time.monotonic()<end;time.sleep(.1)
 def evaluate(self,expression,session):
  result=self.call('Runtime.evaluate',dict(expression=expression,awaitPromise=True,returnByValue=True),session)
  assert 'exceptionDetails' not in result,result;return result['result'].get('value')
 def close(self):
  self.call('Browser.close');self.p.wait(timeout=10)
  os.close(self.input);os.close(self.output);self.parent.close();self.child.close()
with tempfile.TemporaryDirectory(prefix='gnosis-rebrand-extension-') as root:
 root=Path(root);old=root/'jupiter';new=root/'gnosis';profile=old/'brave';source=profile/'builtin-ublock'
 source.parent.mkdir(parents=True);shutil.copytree(args.ublock,source)
 p=Browser(profile)
 try:
  old_id=p.call('Extensions.loadUnpacked',dict(path=str(source)))['id'];s=p.background(old_id)
  p.evaluate("chrome.storage.local.set({gnosisRebrandFixture:'retained'}).then(()=>true)",s)
 finally:p.close()
 new.symlink_to('jupiter',target_is_directory=True)
 p=Browser(new/'brave')
 try:
  new_id=p.call('Extensions.loadUnpacked',dict(path=str(source)))['id']
  assert new_id==old_id,dict(old=old_id,new=new_id)
  assert p.evaluate("chrome.storage.local.get('gnosisRebrandFixture').then(x=>x.gnosisRebrandFixture)",p.background(new_id))=='retained'
  print('REBRAND_NATIVE_EXTENSION_PASS actual full uBlock ID and storage retained through the renamed profile path',flush=True)
 finally:p.close()
