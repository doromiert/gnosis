/* Runs inside the real uBlock Origin dashboard, in a disposable profile. */
(async () => {
  const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
  for (let i = 0; i < 30 && !globalThis.vAPI?.messaging; i++) await wait(200);
  await wait(6000);
  await vAPI.messaging.send('dashboard', {
    what: 'writeUserFilters', enabled: true, trusted: false,
    content: '||127.0.0.1^*/blocked.gif\n127.0.0.1##.jupiter-ad\n127.0.0.1##.jupiter-frame-ad\n127.0.0.1##+js(set-constant, jupiterSentinel, true)\n'
  });
  await vAPI.messaging.send('dashboard', {what: 'reloadAllFilters'});
  console.log('UBO_FILTERS_READY');
  await wait(1000);
  location.href = 'http://127.0.0.1:JUPITER_FIXTURE_PORT/ublock.html';
})().catch(error => console.error('UBO_SETUP_ERROR:' + error.stack));
