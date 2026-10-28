'use strict';

(() => {
    if (window.location.href !== 'ephy-about:overview')
        return;

    class OverviewModel {
        #thumbnails = new Map();
        #urls = [];

        onurlschanged = null;
        onthumbnailchanged = null;
        ontitlechanged = null;

        getThumbnail(url) {
            return this.#thumbnails.get(url) ?? null;
        }

        setURLs(urls) {
            this.#urls = urls;
            this.#thumbnails.clear();
            for (const item of urls)
                this.#thumbnails.set(item.url, item.thumbnail ?? null);
            this.onurlschanged?.(urls);
        }

        setThumbnail(url, thumbnail) {
            this.#thumbnails.set(url, thumbnail);
            this.onthumbnailchanged?.(url, thumbnail);
        }

        setTitle(url, title) {
            const item = this.#urls.find(candidate => candidate.url === url);
            if (item)
                item.title = title;
            this.ontitlechanged?.(url, title);
        }
    }

    class Overview {
        #config;
        #initialized = false;
        #items = [];
        #model;
        #pendingThumbnailChanges = [];
        #pendingTitleChanges = [];
        #pendingURLs = null;

        constructor(model) {
            this.#model = model;
            this.#model.onurlschanged = this.#onURLsChanged.bind(this);
            this.#model.onthumbnailchanged = this.#onThumbnailChanged.bind(this);
            this.#model.ontitlechanged = this.#onTitleChanged.bind(this);
            document.addEventListener('DOMContentLoaded', this.#initialize.bind(this), false);
            document.addEventListener('keypress', this.#onKeyPress.bind(this), false);
            window.addEventListener('pageshow', () => {
                Gnosis.postMessage('overview', {action: 'ready'});
            }, false);
        }

        #initialize() {
            const overview = document.getElementById('overview');
            this.#config = {
                emptyIcon: overview.dataset.emptyIcon,
                emptySubtitle: overview.dataset.emptySubtitle,
                emptyTitle: overview.dataset.emptyTitle,
                pin: overview.dataset.pinLabel,
                remove: overview.dataset.removeLabel,
                unpin: overview.dataset.unpinLabel,
            };

            for (const anchor of document.querySelectorAll('#most-visited-grid > a.overview-item')) {
                if (!anchor.hasAttribute('href'))
                    continue;
                this.#attachItem(anchor);
            }

            this.#initialized = true;
            if (this.#pendingURLs) {
                const urls = this.#pendingURLs;
                this.#pendingURLs = null;
                this.#onURLsChanged(urls);
            }
            for (const change of this.#pendingThumbnailChanges)
                this.#onThumbnailChanged(change.url, change.thumbnail);
            this.#pendingThumbnailChanges = [];
            for (const change of this.#pendingTitleChanges)
                this.#onTitleChanged(change.url, change.title);
            this.#pendingTitleChanges = [];

            if (this.#items.length)
                this.#addPlaceholders();
        }

        #attachItem(anchor) {
            const item = new OverviewItem(anchor);
            anchor.querySelector('.overview-close-button').onclick = event => {
                this.#removeItem(anchor);
                event.preventDefault();
            };
            anchor.querySelector('.overview-pin-button').onclick = event => {
                this.#togglePinItem(anchor);
                event.preventDefault();
            };
            this.#items.push(item);
            return item;
        }

        #onKeyPress(event) {
            if (event.which !== 127)
                return;

            const item = document.activeElement;
            if (item.classList.contains('overview-item') && item.hasAttribute('href')) {
                this.#removeItem(item);
                event.preventDefault();
            }
        }

        #grid() {
            let grid = document.getElementById('most-visited-grid');
            if (grid)
                return grid;

            const overview = document.getElementById('overview');
            overview.replaceChildren();
            overview.classList.remove('overview-empty');
            grid = document.createElement('div');
            grid.id = 'most-visited-grid';
            overview.appendChild(grid);
            return grid;
        }

        #showEmptyState() {
            const overview = document.getElementById('overview');
            const image = document.createElement('img');
            const titleBox = document.createElement('div');
            const title = document.createElement('h1');
            const subtitleBox = document.createElement('div');
            const subtitle = document.createElement('p');

            image.src = this.#config.emptyIcon;
            image.alt = '';
            title.textContent = this.#config.emptyTitle;
            subtitle.textContent = this.#config.emptySubtitle;
            titleBox.appendChild(title);
            subtitleBox.appendChild(subtitle);
            overview.replaceChildren(image, titleBox, subtitleBox);
            overview.classList.add('overview-empty');
        }

        #addPlaceholders() {
            const grid = document.getElementById('most-visited-grid');
            if (!grid)
                return;

            const anchors = grid.getElementsByTagName('a');
            for (let i = anchors.length; i < 9; i++) {
                const anchor = document.createElement('a');
                anchor.className = 'overview-item';
                const thumbnail = document.createElement('span');
                thumbnail.className = 'overview-thumbnail';
                anchor.appendChild(thumbnail);
                const title = document.createElement('span');
                title.className = 'overview-title';
                anchor.appendChild(title);
                grid.appendChild(anchor);
            }
        }

        #removePlaceholders() {
            for (const anchor of document.querySelectorAll('#most-visited-grid > a:not([href])'))
                anchor.remove();
        }

        #removeItem(anchor) {
            anchor.classList.add('overview-removed');
            // The CSS animation takes 0.25s; retain upstream's removal margin.
            setTimeout(() => {
                const url = anchor.href;
                anchor.remove();
                this.#items = this.#items.filter(item => item.url() !== url);
                this.#addPlaceholders();
                Gnosis.postMessage('overview', {action: 'remove', url});
            }, 500);
        }

        #togglePinItem(anchor) {
            Gnosis.postMessage('overview', {
                action: 'pin',
                url: anchor.href,
                pinned: !anchor.classList.contains('overview-item-pinned'),
            });
        }

        #createItem() {
            const anchor = document.createElement('a');
            anchor.className = 'overview-item';
            const closeButton = document.createElement('div');
            closeButton.className = 'overview-close-button';
            closeButton.title = this.#config.remove;
            anchor.appendChild(closeButton);
            const pinButton = document.createElement('div');
            pinButton.className = 'overview-pin-button';
            anchor.appendChild(pinButton);
            const thumbnail = document.createElement('span');
            thumbnail.className = 'overview-thumbnail';
            anchor.appendChild(thumbnail);
            const title = document.createElement('span');
            title.className = 'overview-title';
            anchor.appendChild(title);
            this.#grid().appendChild(anchor);
            return this.#attachItem(anchor);
        }

        #onURLsChanged(urls) {
            if (!this.#initialized) {
                this.#pendingURLs = urls;
                return;
            }
            if (urls.length === 0) {
                this.#items = [];
                this.#showEmptyState();
                return;
            }

            this.#grid();
            this.#removePlaceholders();
            for (let i = 0; i < urls.length; i++) {
                const data = urls[i];
                const item = this.#items[i] ?? this.#createItem();
                item.setURL(data.url);
                item.setTitle(data.title);
                item.setThumbnail(this.#model.getThumbnail(data.url));
                item.setPinned(data.pinned, this.#config.pin, this.#config.unpin);
            }
            while (this.#items.length > urls.length)
                this.#items.pop().detachFromParent();
            this.#addPlaceholders();
        }

        #onThumbnailChanged(url, thumbnail) {
            if (!this.#initialized) {
                this.#pendingThumbnailChanges.push({url, thumbnail});
                return;
            }
            this.#items.find(item => item.url() === url)?.setThumbnail(thumbnail);
        }

        #onTitleChanged(url, title) {
            if (!this.#initialized) {
                this.#pendingTitleChanges.push({url, title});
                return;
            }
            this.#items.find(item => item.url() === url)?.setTitle(title);
        }
    }

    class OverviewItem {
        #item;
        #thumbnail;
        #title;

        constructor(item) {
            this.#item = item;
            this.#thumbnail = item.querySelector('.overview-thumbnail');
            this.#title = item.querySelector('.overview-title');
        }

        url() {
            return this.#item.href;
        }

        setURL(url) {
            this.#item.href = url;
        }

        setTitle(title) {
            this.#item.title = title;
            this.#title.textContent = title;
        }

        setThumbnail(thumbnail) {
            if (thumbnail) {
                this.#thumbnail.style.backgroundImage = `url(${JSON.stringify(thumbnail)})`;
                this.#thumbnail.style.backgroundSize = '100%';
                this.#thumbnail.style.backgroundPosition = 'top';
            } else {
                this.#thumbnail.style.backgroundImage = '';
                this.#thumbnail.style.backgroundSize = 'auto';
                this.#thumbnail.style.backgroundPosition = 'center';
            }
        }

        setPinned(pinned, pinLabel, unpinLabel) {
            this.#item.classList.toggle('overview-item-pinned', pinned);
            this.#item.querySelector('.overview-pin-button').title = pinned ? unpinLabel : pinLabel;
        }

        detachFromParent() {
            this.#item.remove();
        }
    }

    const model = new OverviewModel();
    new Overview(model);
    Gnosis.addEventListener('overview', event => {
        const data = event.detail;
        if (data?.type === 'urls' && Array.isArray(data.urls))
            model.setURLs(data.urls);
        else if (data?.type === 'title')
            model.setTitle(data.url, data.title);
        else if (data?.type === 'thumbnail')
            model.setThumbnail(data.url, data.thumbnail);
    });
})();
