(() => {
 if(!globalThis.chrome?.tabs || chrome.tabs.__jupiter) return;
 const pending=new Map();let sequence=0;
 Object.defineProperty(globalThis,'__jupiterExtensionReply',{value:(id,result,error)=>{
  const request=pending.get(id);if(!request)return;pending.delete(id);
  if(error)request.reject(new Error(error));else request.resolve(result);
 }});
 const call=(op,args,callback)=>{
  const id=++sequence;
  const promise=new Promise((resolve,reject)=>{
   const timer=setTimeout(()=>{pending.delete(id);reject(new Error('The browser did not respond.'));},15000);
   pending.set(id,{resolve:value=>{clearTimeout(timer);resolve(value);},reject:error=>{clearTimeout(timer);reject(error);}});
   __jupiterExtensionRequest(JSON.stringify({id,op,args}));
  });
  if(typeof callback==='function') {promise.then(value=>callback(value),()=>callback());return;}
  return promise;
 };
 const tabs=chrome.tabs;
 Object.defineProperty(tabs,'__jupiter',{value:true});
 for(const op of ['insertCSS','removeCSS','executeScript']) tabs[op]=(tabId,details,callback)=>call(op,{tabId,details},callback);
 // The GTK shell owns the tab inventory; native queries also include popup web contents.
 tabs.query=(queryInfo,callback)=>call('query',queryInfo,callback);
 tabs.create=(properties,callback)=>call('create',{properties},callback);
 const get=tabs.get.bind(tabs);
 tabs.get=(tabId,callback)=>{
  const promise=call('get',{tabId}).then(tab=>tab||get(tabId));
  if(typeof callback==='function'){promise.then(callback,()=>callback());return;}return promise;
 };
 const update=tabs.update.bind(tabs);
 tabs.update=(tabId,properties,callback)=>{
  if(typeof tabId!=='number')return update(tabId,properties);
  const promise=call('update',{tabId,properties}).then(tab=>tab||update(tabId,properties));
  if(typeof callback==='function'){promise.then(callback,()=>callback());return;}return promise;
 };
})()
