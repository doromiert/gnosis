chrome.runtime.onMessage.addListener((message,sender,reply)=>{
 chrome.tabs.insertCSS(sender.tab.id,{code:'.jupiter-ad{display:none!important}'},()=>{
  reply({background:true,version:2,tabId:sender.tab.id,cssError:chrome.runtime.lastError?.message});
 });return true;
});
chrome.webRequest.onBeforeRequest.addListener(details=>({cancel:details.url.includes('blocked.gif')}),{urls:['http://127.0.0.1/*']},['blocking']);
