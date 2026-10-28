// SPDX-License-Identifier: GPL-3.0-or-later
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(`${__dirname}/../data/web-app-metadata.js`, 'utf8');
function metadata({metas = [], links = [], title = '', manifest = null} = {}) {
    const node = attrs => ({...attrs, getAttribute: key => attrs[key] ?? null});
    return vm.runInNewContext(source, {
        URL, location: {href: 'https://example.test/app/'},
        document: {
            title,
            getElementsByTagName: tag => (tag === 'meta' ? metas : links).map(node),
            head: {
                querySelector: () => manifest ? {href: manifest} : null,
                getElementsByTagName: () => title ? [{innerText: title}] : [],
            },
        },
    });
}
let data = metadata();
assert.equal(data.getWebAppManifestURL(), null);
assert.equal(data.getWebAppTitle(), null);
assert.equal(data.getAppleMobileWebAppCapable(), false);
assert.equal(data.getWebAppIcon().url, 'https://example.test/favicon.ico');
data = metadata({title: 'Document', manifest: 'https://example.test/app.webmanifest', metas: [
    {name: 'application-name', content: 'Installed App'},
    {name: 'apple-mobile-web-app-capable', content: 'yes'},
], links: [
    {rel: 'icon', href: '/small.png', sizes: '32x32'},
    {rel: 'apple-touch-icon', href: '/large.png', sizes: '192x192'},
]});
assert.equal(data.getWebAppManifestURL(), 'https://example.test/app.webmanifest');
assert.equal(data.getWebAppTitle(), 'Installed App');
assert.equal(data.getAppleMobileWebAppCapable(), true);
assert.equal(data.getWebAppIcon().url, 'https://example.test/large.png');
assert.equal(metadata({title: 'Document'}).getWebAppTitle(), 'Document');
data = metadata({metas: [
    {name: 'msapplication-TileImage', content: '/tile.png'},
    {name: 'msapplication-TileColor', content: '#123456'},
]});
assert.equal(data.getWebAppIcon().url, 'https://example.test/tile.png');
assert.equal(data.getWebAppIcon().color, '#123456');
console.log('WEB_APP_METADATA_PASS standalone isolated discovery with manifest and fallback metadata');
