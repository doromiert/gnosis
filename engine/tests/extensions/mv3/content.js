let attempts=0,pending=false;
const timer=setInterval(()=>{
  if(pending)return;
  if (++attempts>20) clearInterval(timer);
  pending=true;
  chrome.runtime.sendMessage({probe:true},reply=>{
    pending=false;
    document.documentElement.dataset.jupiterExtension=JSON.stringify(reply||{error:chrome.runtime.lastError?.message});
    if (reply?.script && reply?.storage && reply?.css) clearInterval(timer);
  });
},500);
