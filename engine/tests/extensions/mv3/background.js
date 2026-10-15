chrome.runtime.onMessage.addListener((message,sender,reply)=>{reply({background:true,version:chrome.runtime.getManifest().manifest_version,tabId:sender.tab?.id});});
