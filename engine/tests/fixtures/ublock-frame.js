setTimeout(()=>parent.postMessage('UBO_FRAME:'+(getComputedStyle(document.querySelector('.jupiter-frame-ad')).display==='none'?'PASS':'FAIL'),location.origin),4500);
