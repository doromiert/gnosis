let attempts=0;
const timer=setInterval(()=>{
  if (++attempts>20) clearInterval(timer);
  chrome.runtime.sendMessage({probe:true},reply=>{
    document.documentElement.dataset.jupiterExtension=JSON.stringify(reply||{error:chrome.runtime.lastError?.message});
    if (reply?.script && reply?.storage && reply?.css) clearInterval(timer);
  });
},500);
