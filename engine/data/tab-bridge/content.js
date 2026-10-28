let attempts=0;
const timer=setInterval(()=>{
 chrome.runtime.sendMessage({},reply=>{
  if(chrome.runtime.lastError) return;
  if(Number.isInteger(reply?.tabId)) {console.info('GNOSIS_NATIVE_TAB_ID:'+reply.tabId);console.info('GNOSIS_NATIVE_FRAME:'+JSON.stringify(reply));clearInterval(timer);}
 });
 if(++attempts>=60)clearInterval(timer);
},250);
