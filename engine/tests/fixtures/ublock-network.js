/* Same fetch shape as public host tests; server logs are the blocking oracle. */
(async () => {
  const params = new URL(location).searchParams;
  const origins = {same: location.origin, cross: params.get('networkOrigin')};
  const run = params.get('networkNonce');
  const tasks = [];
  for (const origin of ['same', 'cross']) {
    for (const method of ['HEAD', 'GET']) {
      for (const kind of ['allowed-200', 'allowed-404', 'hard-block', 'redirect-empty']) {
        for (let repeat = 0; repeat < 4; repeat++) {
          tasks.push({origin, method, kind, nonce: `${run}-${origin}-${method}-${kind}-${repeat}`});
        }
      }
    }
  }
  const results = [];
  let next = 0;
  await Promise.all(Array.from({length: 50}, async () => {
    while (next < tasks.length) {
      const task = tasks[next++];
      const controller = new AbortController();
      const timer = setTimeout(() => controller.abort(), 8000);
      const result = {...task};
      try {
        const response = await fetch(`${origins[task.origin]}/network-probe/${task.kind}?nonce=${task.nonce}`, {
          method: task.method, mode: 'no-cors', cache: 'no-store', signal: controller.signal
        });
        Object.assign(result, {outcome: 'fulfilled', status: response.status, type: response.type, url: response.url});
      } catch (error) {
        Object.assign(result, {outcome: 'rejected', error: error.name});
      } finally {
        clearTimeout(timer);
      }
      results.push(result);
    }
  }));
  console.log('UBO_NETWORK_RESULTS:' + JSON.stringify(results));
  const ok = results.every(result => {
    if (result.kind.startsWith('allowed-')) {
      const status = result.origin === 'cross' ? 0 : Number(result.kind.slice(-3));
      return result.outcome === 'fulfilled' && result.status === status;
    }
    if (result.error === 'AbortError') return false;
    // A redirect may fulfill locally; independent server accounting must still pass.
    return result.kind === 'redirect-empty' || result.outcome === 'rejected';
  });
  const redirected = results.filter(result => result.kind === 'redirect-empty');
  console.log(`UBO_NETWORK_REDIRECT_FULFILLED:${redirected.filter(r => r.outcome === 'fulfilled').length}/${redirected.length}`);
  console.log('UBO_NETWORK_PROBES:' + (ok ? 'PASS' : 'FAIL'));
})().catch(error => {
  console.error('UBO_NETWORK_PROBE_ERROR:' + error.stack);
  console.log('UBO_NETWORK_PROBES:FAIL');
});
