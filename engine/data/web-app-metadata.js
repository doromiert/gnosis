// SPDX-License-Identifier: GPL-3.0-or-later
// Epiphany metadata discovery, evaluated on demand in an isolated world.
({
getAppleMobileWebAppCapable()
{
    for (const meta of document.getElementsByTagName('meta')) {
        // https://developer.apple.com/library/archive/documentation/AppleApplications/Reference/SafariHTMLRef/Articles/MetaTags.html
        if (meta.name === 'apple-mobile-web-app-capable' && meta.getAttribute('content') === 'yes')
            return true;
    }

    return false;
},

getWebAppManifestURL()
{
    const manifest = document.head.querySelector('link[rel=manifest]');
    return manifest ? manifest.href : null;
},

getWebAppTitle()
{
    for (const meta of document.getElementsByTagName('meta')) {
        // https://developer.mozilla.org/en-US/docs/Web/HTML/Element/meta/name#standard_metadata_names_defined_in_the_html_specification
        if (meta.name === 'application-name')
            return meta.content;

        // https://developer.apple.com/library/archive/documentation/AppleApplications/Reference/SafariWebContent/ConfiguringWebApplications/ConfiguringWebApplications.html
        if (meta.name === 'apple-mobile-web-app-title')
            return meta.content;

        // https://ogp.me/
        // og:site_name is read from the property attribute (standard), but is
        // commonly seen on the web in the name attribute. Both are supported.
        if (meta.getAttribute('property') === 'og:site_name' || meta.name === 'og:site_name')
            return meta.content;
    }

    // document title
    // Prefer HTML tag over dynamic updates
    const titles = document.head.getElementsByTagName('title');
    if (titles.length > 0) {
      const title = titles[titles.length - 1];

      if (title && title.innerText)
          return title.innerText;
    }
    if (document.title)
        return document.title;

    // set_default_application_title will fallback to the formatted hostname
    return null;
},

getWebAppIcon()
{
    const baseURL = location.href;
    let htmlIconURL = null;
    let msIconURL = null;
    let largestIconSize = 0;
    let iconColor = null;
    const links = document.getElementsByTagName('link');
    const metas = document.getElementsByTagName('meta');

    for (const link of links) {
        if (link.rel === 'icon' || link.rel === 'shortcut icon' || link.rel === 'icon shortcut' || link.rel === 'shortcut-icon' || link.rel === 'apple-touch-icon' || link.rel === 'apple-touch-icon-precomposed') {
            const sizes = link.getAttribute('sizes');

            if (!sizes) {
                if (largestIconSize === 0 && (!htmlIconURL || link.rel === 'apple-touch-icon' || link.rel === 'apple-touch-icon-precomposed'))
                  htmlIconURL = link.href;
                continue;
            }

            if (sizes === 'any') {
                // "any" means a vector, and thus it will always be the largest icon.
                htmlIconURL = link.href;
                break;
            }

            const sizesList = sizes.split(' ');
            for (let size of sizesList) {
                size = size.toLowerCase().split('x');

                // Only accept square icons.
                if (size.length !== 2 || size[0] !== size[1])
                    continue;

                // Only accept icons of 96 px (smallest GNOME HIG app icon) or larger.
                // It's better to defer to other icon discovery methods if smaller
                // icons are returned here.
                if (size[0] >= 92 && size[0] > largestIconSize) {
                    htmlIconURL = link.href;
                    largestIconSize = size[0];
                }
            }
        }
    }

    if (largestIconSize !== 0 && htmlIconURL)
        return { 'url' : new URL(htmlIconURL, baseURL).href, 'color' : null };

    for (const meta of metas) {
        if (meta.name === 'msapplication-TileImage')
            msIconURL = meta.content;
        else if (meta.name === 'msapplication-TileColor')
            iconColor = meta.content;
    }

    // msapplication icon.
    if (msIconURL)
        return { 'url' : new URL(msIconURL, baseURL).href, 'color' : iconColor };

    // html icon without known size
    if (htmlIconURL)
        return { 'url' : new URL(htmlIconURL, baseURL).href, 'color' : null };

    // Last ditch effort: just fallback to the default favicon location.
    return { 'url' : new URL('/favicon.ico', baseURL).href, 'color' : null };
}
})
