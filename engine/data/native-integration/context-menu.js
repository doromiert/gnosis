// Executed by the native backend in a private CDP isolated world, not as a
// page script. No native command binding is exposed to webpage JavaScript.
(() => {
  const send = globalThis.__jupiterContextMenu;
  delete globalThis.__jupiterContextMenu;
  if (typeof send !== 'function' || typeof globalThis.__gnosisContextMenuAction === 'function') return;
  let sequence = 0;
  let target = null;
  let targetToken = '';
  // Keep the actual hit element in this isolated world. A menu command cannot
  // substitute a selector/URL or accidentally operate on a newer context menu.
  Object.defineProperty(globalThis, '__gnosisContextMenuAction', {value: async (token, action) => {
    const media = token === targetToken ? target?.deref() : null;
    if (!(media instanceof HTMLMediaElement) || !media.isConnected) return false;
    switch (action) {
      case 'play': await media.play(); break;
      case 'pause': media.pause(); break;
      case 'mute': media.muted = true; break;
      case 'unmute': media.muted = false; break;
      case 'loop': media.loop = true; break;
      case 'unloop': media.loop = false; break;
      case 'show-controls': media.controls = true; break;
      case 'hide-controls': media.controls = false; break;
      case 'picture-in-picture':
        if (!(media instanceof HTMLVideoElement)) return false;
        if (document.pictureInPictureElement === media) await document.exitPictureInPicture();
        else await media.requestPictureInPicture();
        break;
      case 'fullscreen':
        if (!(media instanceof HTMLVideoElement)) return false;
        if (document.fullscreenElement === media) await document.exitFullscreen();
        else await media.requestFullscreen();
        break;
      default: return false;
    }
    return true;
  }});
  window.addEventListener('contextmenu', event => {
    if (!event.isTrusted || event.defaultPrevented) return;
    const path = event.composedPath();
    const element = path.find(node => node instanceof Element);
    if (!element) return;
    const link = path.find(node => node instanceof HTMLAnchorElement || node instanceof HTMLAreaElement);
    const image = path.find(node => node instanceof HTMLImageElement);
    const media = path.find(node => node instanceof HTMLMediaElement);
    const input = element.closest('input,textarea');
    const editable = element.isContentEditable || !!(input && !input.disabled && !input.readOnly &&
      (input instanceof HTMLTextAreaElement || /^(text|search|url|email|tel|password|number)$/.test(input.type)));
    const password = input instanceof HTMLInputElement && input.type === 'password';
    let selection = password ? '' : String(getSelection());
    if (!password && input && typeof input.selectionStart === 'number')
      selection = input.value.slice(input.selectionStart, input.selectionEnd);
    const rect = element.getBoundingClientRect();
    target = media ? new WeakRef(media) : null;
    targetToken = String(++sequence);
    const video = media instanceof HTMLVideoElement;
    event.preventDefault();
    send(JSON.stringify({link: link?.href || '', image: image?.currentSrc || image?.src || '',
      target: targetToken,
      media: media ? {kind: video ? 'video' : 'audio', src: media.currentSrc || media.src || '',
        paused: media.paused, muted: media.muted, loop: media.loop, controls: media.controls,
        error: !!media.error,
        pictureInPicture: video && document.pictureInPictureEnabled && !media.disablePictureInPicture &&
          typeof media.requestPictureInPicture === 'function',
        inPictureInPicture: document.pictureInPictureElement === media,
        fullscreen: video && document.fullscreenEnabled && typeof media.requestFullscreen === 'function',
        inFullscreen: document.fullscreenElement === media} : null,
      selection: selection.slice(0, 16384), editable, password,
      pointer: event.button === 2, x: Number.isFinite(event.clientX) ? event.clientX : rect.x,
      y: Number.isFinite(event.clientY) ? event.clientY : rect.y}));
  });
})();
