// SPDX-License-Identifier: GPL-3.0-or-later
// Private GTK-shell adapter. No external messaging listener and no credential
// values leave the upstream worker/content-script boundary.
globalThis.jupiterKeePassXC = Object.freeze({
    resolvePasskey(id, provider) {
        globalThis.gnosisPasskeyRequests?.get(id)?.(provider);
    },
    async invoke(tabId, operation, argument = '') {
        if (!page.settings) throw new Error('KeePassXC is starting. Try again shortly.');
        const tab = await browser.tabs.get(tabId);
        if (tab.incognito) throw new Error('KeePassXC is not enabled in private tabs.');
        const website = /^https?:\/\//.test(tab.url || '');
        const sendToPage = async message => {
            const target = {tabId: tab.id, frameIds: [0]};
            try {
                return await browser.tabs.sendMessage(tab.id, message, {frameId: 0});
            } catch (error) {
                if (!website || !/Receiving end does not exist/.test(String(error))) throw error;
                // An unpacked integration can finish loading after this document.
                // Recover only its own manifest-declared scripts in this tab's
                // top frame; Chrome still enforces the extension's host grants.
                const [probe] = await browser.scripting.executeScript({
                    target, func: () => typeof kpxc !== 'undefined'
                });
                if (!probe?.result) {
                    const currentURL = new URL((await browser.tabs.get(tab.id)).url);
                    const matches = pattern => {
                        if (pattern === '<all_urls>') return /^https?:$/.test(currentURL.protocol);
                        const parts = /^(\*|https?):\/\/([^/]+)(\/.*)$/.exec(pattern);
                        if (!parts) return false;
                        const [, scheme, host, path] = parts;
                        if (scheme !== '*' && currentURL.protocol !== scheme + ':') return false;
                        if (host !== '*' && currentURL.hostname !== host &&
                            !(host.startsWith('*.') && (currentURL.hostname === host.slice(2) ||
                                currentURL.hostname.endsWith(host.slice(1))))) return false;
                        const expression = path.split('*').map(part => part.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')).join('.*');
                        return new RegExp('^' + expression + '$').test(currentURL.pathname + currentURL.search);
                    };
                    for (const script of browser.runtime.getManifest().content_scripts || [])
                        if (script.js?.length && script.matches?.some(matches) &&
                            !script.exclude_matches?.some(matches))
                            await browser.scripting.executeScript({target, files: script.js});
                }
                for (let attempt = 0; attempt < 20; ++attempt) {
                    await new Promise(resolve => setTimeout(resolve, 50));
                    try { return await browser.tabs.sendMessage(tab.id, message, {frameId: 0}); }
                    catch (failure) {
                        if (!/Receiving end does not exist/.test(String(failure)) || attempt === 19) throw failure;
                    }
                }
            }
        };
        if (!tabs.getTabFromId(tab.id)) await tabs.createTabEntry(tab.id);
        if (operation === 'reconnect') await kpxcEvent.onReconnect(tab);
        else if (operation === 'unlock') {
            if (!keepass.isConnected) await kpxcEvent.onReconnect(tab);
            await keepass.getDatabaseHash(tab, [false, true]);
        } else if (operation === 'associate') {
            if (!keepass.isConnected) await kpxcEvent.onReconnect(tab);
            await keepass.getDatabaseHash(tab, [false, true]);
            // Unlocking is asynchronous in KeePassXC. Finish the user's pairing
            // request after its password window succeeds, then let KeePassXC
            // itself ask for database association authorization.
            for (let attempt = 0; keepass.isDatabaseClosed && attempt < 240; ++attempt)
                await new Promise(resolve => setTimeout(resolve, 250));
            if (keepass.isDatabaseClosed) throw new Error('Unlock the database in KeePassXC, then connect again.');
            await keepass.associate(tab);
        } else if (operation === 'lock') await keepass.lockDatabase(tab);
        else if (operation === 'passkeys' || operation === 'auto-unlock') {
            if (typeof argument !== 'boolean') throw new Error('Invalid setting');
            page.settings[operation === 'passkeys' ? 'passkeys' : 'jupiterAutoUnlock'] = argument;
            await browser.storage.local.set({ settings: page.settings });
            if (website) await sendToPage({ action: 'redetect_fields' }).catch(() => {});
        } else if (operation === 'redetect') {
            if (!website) throw new Error('Open a website to detect login fields.');
            await sendToPage({ action: 'redetect_fields' });
        } else if (operation === 'entries') {
            if (!website) throw new Error('Open a website to fill a login.');
            await sendToPage({ action: 'retrieve_credentials_forced' });
            let entries = await page.getLoginList(tab);
            // Upstream's content script publishes its formatted login list in
            // a separate IPC after retrieval. Let that message finish first.
            for (let attempt = 0; !entries.length && attempt < 10; ++attempt) {
                await new Promise(resolve => setTimeout(resolve, 25));
                entries = await page.getLoginList(tab);
            }
            return { entries: entries.slice(0, 200).map(entry => ({
                id: String(entry.uuid || ''), label: String(entry.text || '').slice(0, 1024)
            })) };
        } else if (operation === 'fill') {
            if (!website) throw new Error('Open a website to fill a login.');
            const entries = await page.getLoginList(tab);
            const index = entries.findIndex(entry => entry.uuid === argument);
            if (index < 0) throw new Error('That login is no longer available. Refresh the list.');
            await sendToPage({
                action: 'fill_user_pass_with_specific_login', id: index, uuid: entries[index].uuid
            });
        } else if (operation === 'totp') {
            if (!website) throw new Error('Select a one-time-code field on a website first.');
            await sendToPage({ action: 'fill_totp' });
        } else if (operation !== 'status') throw new Error('Unknown KeePassXC operation');
        return {
            connected: !!keepass.isConnected, available: !!keepass.isKeePassXCAvailable,
            locked: !!keepass.isDatabaseClosed, associated: !!keepass.isAssociated(),
            version: String(keepass.currentKeePassXC || ''),
            passkeys: !!page.settings.passkeys, autoUnlock: page.settings.jupiterAutoUnlock !== false,
            website, redetected: operation === 'redetect'
        };
    }
});

// Only the attached, verified extension worker has this CDP binding. Credential
// options and responses stay inside upstream; GTK receives provider metadata.
globalThis.gnosisPasskeyRequests ??= new Map();
kpxcEvent.messageHandlers.gnosis_passkeys_cancel = tab => {
    for (const finish of globalThis.gnosisPasskeyRequests.values())
        if (finish.tabId === tab.id) finish('cancel');
};
for (const [action, method] of [['passkeys_register', 'passkeysRegister'], ['passkeys_get', 'passkeysGet']]) {
    kpxcEvent.messageHandlers[action] = async (tab, args = []) => {
        if (typeof globalThis.__gnosisPasskeyProvider !== 'function') return keepass[method](tab, args);
        const current = await browser.tabs.get(tab.id);
        if (current.incognito || !/^https?:\/\//.test(current.url || '') || args.length < 2 ||
            !page.settings.passkeys || globalThis.gnosisPasskeyRequests.size >= 8)
            return {gnosisProvider: 'cancel'};
        const id = crypto.randomUUID();
        const timeout = Math.min(120000, Math.max(15000, Number(args[0]?.timeout) || 60000));
        const provider = await new Promise(resolve => {
            const timer = setTimeout(() => finish('cancel'), timeout);
            const finish = choice => {
                clearTimeout(timer);
                globalThis.gnosisPasskeyRequests.delete(id);
                try { globalThis.__gnosisPasskeyProvider(JSON.stringify({id, tabId: tab.id, url: current.url, closed: true})); } catch {}
                resolve(['keepassxc', 'device'].includes(choice) ? choice : 'cancel');
            };
            finish.tabId = tab.id;
            globalThis.gnosisPasskeyRequests.set(id, finish);
            try {
                globalThis.__gnosisPasskeyProvider(JSON.stringify({id, tabId: tab.id, url: current.url,
                    create: action === 'passkeys_register', timeout}));
            } catch { finish('cancel'); }
        });
        if (provider !== 'keepassxc') return {gnosisProvider: provider};
        try {
            if (!keepass.isConnected) await kpxcEvent.onReconnect(current);
            await keepass.getDatabaseHash(current, [false, true]);
            const deadline = Date.now() + timeout;
            while (keepass.isDatabaseClosed && Date.now() < deadline)
                await new Promise(resolve => setTimeout(resolve, 250));
            if (keepass.isDatabaseClosed) return {gnosisProvider: 'cancel'};
            if (!keepass.isAssociated()) await keepass.associate(current);
            if (!keepass.isAssociated()) return {gnosisProvider: 'cancel'};
            if ((await browser.tabs.get(tab.id)).url !== current.url) return {gnosisProvider: 'cancel'};
            // Never retry this request with a different provider without consent.
            return {...await keepass[method](current, args), gnosisProvider: 'keepassxc'};
        } catch { return {gnosisProvider: 'cancel'}; }
    };
}
