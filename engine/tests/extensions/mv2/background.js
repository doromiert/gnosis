chrome.runtime.onMessage.addListener((message,sender,reply)=>{
 (async()=>{
  const version=chrome.runtime.getManifest().manifest_version;
  await chrome.storage.local.set({fixture:version});
  const stored=await chrome.storage.local.get('fixture');
  let script,css;
  const values=await chrome.tabs.executeScript(sender.tab.id,{allFrames:true,code:"document.documentElement.dataset.gnosisInjected='yes'; 7"});
  await chrome.tabs.insertCSS(sender.tab.id,{allFrames:true,code:'.gnosis-ad{display:none!important}'});
  script=values?.length===1 && values[0]===7;css=true;
  reply({background:true,version,tabId:sender.tab.id,storage:stored.fixture===version,script,css});
 })().catch(error=>reply({error:error.message}));
 return true;
});
chrome.webRequest.onBeforeRequest.addListener(details=>({cancel:details.url.includes('blocked.gif')}),{urls:['http://127.0.0.1/*']},['blocking']);
