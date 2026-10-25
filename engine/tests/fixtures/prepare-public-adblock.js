/* Read defaults in this disposable profile; never change uBlock's settings. */
(async () => {
  const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
  let lists;
  for (let i = 0; i < 150; i++) {
    if (globalThis.vAPI?.messaging)
      lists = await vAPI.messaging.send('dashboard', {what: 'getLists'});
    if (lists?.userFiltersPath) break;
    await wait(200);
  }
  if (!lists?.userFiltersPath) throw new Error('uBlock did not finish native startup');
  console.log('UBO_DASHBOARD_READY');
  const filters = await vAPI.messaging.send('dashboard', {what: 'readUserFilters'});
  console.log('PUBLIC_ADBLOCK_DEFAULTS:' + JSON.stringify({lists, filters}));
  if (filters?.content?.trim()) throw new Error('Fresh public probe unexpectedly has user filters');
  location.href = 'https://adblock.turtlecute.org/';
})().catch(error => console.error('UBO_SETUP_ERROR:' + error.stack));
