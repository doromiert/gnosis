chrome.runtime.onMessage.addListener((message,sender,reply)=>{
 (async()=>{
  const version=chrome.runtime.getManifest().manifest_version;
  await chrome.storage.local.set({fixture:version});
  const stored=await chrome.storage.local.get('fixture');
  let script,css;
  const values=await chrome.scripting.executeScript({target:{tabId:sender.tab.id,allFrames:true},func:()=>{document.documentElement.dataset.gnosisInjected='yes';return 7;}});
  await chrome.scripting.insertCSS({target:{tabId:sender.tab.id,allFrames:true},css:'.gnosis-ad{display:none!important}'});
  const target={tabId:sender.tab.id,allFrames:true};
  await chrome.scripting.executeScript({target,func:()=>{
   const probe=document.createElement('div');
   probe.id='gnosis-user-origin';
   probe.style.setProperty('color','rgb(11, 22, 33)','important');
   document.documentElement.append(probe);
  }});
  const userStyle={target,origin:'USER',css:'#gnosis-user-origin{color:rgb(44,55,66)!important}'};
  let userValues;
  try {
   await chrome.scripting.insertCSS(userStyle);
   userValues=await chrome.scripting.executeScript({target,func:()=>getComputedStyle(document.getElementById('gnosis-user-origin')).color});
  } finally {
   await chrome.scripting.removeCSS(userStyle);
  }
  const restoredValues=await chrome.scripting.executeScript({target,func:()=>{
   const probe=document.getElementById('gnosis-user-origin');
   const color=getComputedStyle(probe).color;
   probe.remove();
   return color;
  }});
  script=values?.length===1 && values[0].result===7;
  css=userValues?.length===1 && userValues[0].result==='rgb(44, 55, 66)' && restoredValues?.length===1 && restoredValues[0].result==='rgb(11, 22, 33)';
  if(!css) throw new Error('USER stylesheet must override author !important and removal must restore it');
  reply({background:true,version,tabId:sender.tab.id,storage:stored.fixture===version,script,css});
 })().catch(error=>reply({error:error.message}));
 return true;
});
