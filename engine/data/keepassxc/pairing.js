// SPDX-License-Identifier: GPL-3.0-or-later
// The verified native worker shares database associations, never login entries.
globalThis.gnosisKeePassXCPairing ??= (() => {
    let publisher = false, imports = Promise.resolve();
    const valid = (hash, entry) => /^[a-f0-9]{32,128}$/i.test(hash) && entry &&
        typeof entry.id === 'string' && entry.id.length > 0 && entry.id.length <= 4096 &&
        typeof entry.key === 'string' && /^[A-Za-z0-9+/]{43}=$/.test(entry.key);
    const associations = ring => Object.fromEntries(Object.entries(ring || {})
        .filter(([hash, entry]) => valid(hash, entry)).slice(0, 256)
        .sort(([a], [b]) => a.localeCompare(b))
        .map(([hash, entry]) => [hash, {hash, id: entry.id, key: entry.key}]));
    const same = (a, b) => !!a && !!b && a.id === b.id && a.key === b.key;
    const publish = async () => {
        if (!publisher || typeof globalThis.__gnosisKeePassXCPairingChanged !== 'function') return;
        const {keyRing} = await browser.storage.local.get('keyRing');
        globalThis.__gnosisKeePassXCPairingChanged(JSON.stringify(associations(keyRing)));
    };
    browser.storage.onChanged.addListener((changes, area) => {
        if (area === 'local' && Object.hasOwn(changes, 'keyRing')) void publish();
    });
    const apply = async shared => {
        if (publisher) throw new Error('The main profile owns its KeePassXC associations');
        const incoming = associations(shared);
        if (JSON.stringify(incoming) !== JSON.stringify(shared))
            throw new Error('Invalid shared KeePassXC associations');
        const stored = await browser.storage.local.get(['keyRing', 'gnosisInheritedAssociations']);
        const ring = {...stored.keyRing}, previous = stored.gnosisInheritedAssociations || {}, next = {};
        // Restore app-specific pairings when an inherited association is removed.
        for (const [hash, record] of Object.entries(previous)) {
            if (Object.hasOwn(incoming, hash) || !same(ring[hash], record?.shared)) continue;
            if (valid(hash, record?.local)) ring[hash] = record.local;
            else delete ring[hash];
        }
        for (const [hash, entry] of Object.entries(incoming)) {
            // Upstream can migrate a database hash while retaining its association.
            const old = previous[hash] || Object.values(previous).find(record => same(ring[hash], record?.shared));
            const local = old && same(ring[hash], old.shared) ? old.local : ring[hash];
            next[hash] = {shared: entry};
            if (valid(hash, local)) next[hash].local = {...local, hash};
            if (!same(ring[hash], entry)) ring[hash] = {...entry, created: Date.now(), lastUsed: Date.now()};
        }
        if (JSON.stringify(ring) === JSON.stringify(stored.keyRing || {}) &&
            JSON.stringify(next) === JSON.stringify(previous)) return false;
        const priorRing = keepass.keyRing;
        keepass.keyRing = ring;
        keepass.associated.value = false;
        keepass.associated.hash = null;
        page.clearAllLogins();
        try {
            await browser.storage.local.set({keyRing: ring, gnosisInheritedAssociations: next});
        } catch (error) {
            if (keepass.keyRing === ring) keepass.keyRing = priorRing;
            throw error;
        }
        return true;
    };
    return Object.freeze({
        async publish() {publisher = true; await publish();},
        apply(shared) {
            imports = imports.catch(() => {}).then(() => apply(shared));
            return imports;
        }
    });
})();
