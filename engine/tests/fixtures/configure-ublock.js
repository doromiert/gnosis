/* Runs inside the real uBlock Origin dashboard, in a disposable profile. */
(async () => {
  const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
  let lists;
  for (let i = 0; i < 100; i++) {
    // First install reloads uBO; disconnected messaging resolves undefined.
    if (globalThis.vAPI?.messaging)
      lists = await vAPI.messaging.send('dashboard', {what:'getLists'});
    if (lists?.userFiltersPath) break;
    await wait(200);
  }
  // getLists waits for tab initialization and real network observers to start.
  if (!lists?.userFiltersPath) throw new Error('uBlock did not finish native startup');
  console.log('UBO_DASHBOARD_READY');
  const filters = '||127.0.0.1^*/blocked.gif\n127.0.0.1##.jupiter-ad\n127.0.0.1##.jupiter-frame-ad\n127.0.0.1##+js(set-constant, jupiterSentinel, true)';
  await vAPI.messaging.send('dashboard', {
    what: 'writeUserFilters', enabled: true, trusted: false,
    content: filters
  });
  const saved = await vAPI.messaging.send('dashboard', {what:'readUserFilters'});
  if (!saved?.enabled || saved.content !== filters) throw new Error('uBlock did not persist the exact enabled fixture filters: '+JSON.stringify(saved));
  await vAPI.messaging.send('dashboard', {what: 'reloadAllFilters'});
  console.log('UBO_FILTERS_READY');
  await wait(1000);
  location.href = 'http://127.0.0.1:JUPITER_FIXTURE_PORT/ublock.html';
})().catch(error => console.error('UBO_SETUP_ERROR:' + error.stack));
