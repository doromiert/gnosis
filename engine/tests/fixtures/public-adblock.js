/* Observes the public page without replacing its requests or filter results. */
console.log('PUBLIC_ADBLOCK_OBSERVER_BOOT:' + JSON.stringify({href: location.href, readyState: document.readyState}));
if (top === window && location.hostname === 'adblock.turtlecute.org') {
  window.__jupiterPublicObserver = true;
  const stage = sessionStorage.getItem('jupiter-public-retest') ? 'retest' : 'initial';
  const storedCount = () => JSON.parse(localStorage.getItem('adb_tool') || '{}').results?.length || 0;
  const baseline = storedCount();
  const originalFetch = window.fetch;
  window.fetch = function (...args) {
    const promise = originalFetch.apply(this, args);
    const url = String(args[0]?.url || args[0]);
    const method = args[1]?.method || args[0]?.method || 'GET';
    promise.then(response => {
      console.log('PUBLIC_ADBLOCK_FETCH:' + JSON.stringify({stage, url, method, outcome: 'fulfilled',
        status: response.status, type: response.type, responseURL: response.url, redirected: response.redirected}));
    }, error => {
      console.log('PUBLIC_ADBLOCK_FETCH:' + JSON.stringify({stage, url, method, outcome: 'rejected', error: error.name}));
    });
    return promise;
  };
  let elapsed = 0;
  const timer = setInterval(() => {
    try {
      const results = JSON.parse(localStorage.getItem('adb_tool') || '{}').results || [];
      if (results.length > baseline) {
        clearInterval(timer);
        console.log('PUBLIC_ADBLOCK_REPORT:' + JSON.stringify({stage, report: results.at(-1)}));
        if (stage === 'initial') {
          const button = document.querySelector('#start_test');
          if (!button) throw new Error('Public Re-test button missing');
          sessionStorage.setItem('jupiter-public-retest', '1');
          console.log('PUBLIC_ADBLOCK_RETEST_CLICK');
          button.click();
        } else {
          console.log('PUBLIC_ADBLOCK:PASS');
        }
      } else if (++elapsed > 1200) {
        throw new Error('Public site did not save its completed result within 120 seconds');
      }
    } catch (error) {
      clearInterval(timer);
      console.error('PUBLIC_ADBLOCK_ERROR:' + error.stack);
      console.log('PUBLIC_ADBLOCK:FAIL');
    }
  }, 100);
}
