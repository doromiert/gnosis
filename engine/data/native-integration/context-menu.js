// Executed by the native backend in a private CDP isolated world, not as a
// page script. No native command binding is exposed to webpage JavaScript.
(() => {
  const send = globalThis.__jupiterContextMenu;
  delete globalThis.__jupiterContextMenu;
  window.addEventListener('contextmenu', event => {
    if (!event.isTrusted || event.defaultPrevented) return;
    const path = event.composedPath();
    const element = path.find(node => node instanceof Element);
    if (!element) return;
    const link = path.find(node => node instanceof HTMLAnchorElement || node instanceof HTMLAreaElement);
    const image = path.find(node => node instanceof HTMLImageElement);
    const input = element.closest('input,textarea');
    const editable = element.isContentEditable || !!(input && !input.disabled && !input.readOnly &&
      (input instanceof HTMLTextAreaElement || /^(text|search|url|email|tel|password|number)$/.test(input.type)));
    const password = input instanceof HTMLInputElement && input.type === 'password';
    let selection = password ? '' : String(getSelection());
    if (!password && input && typeof input.selectionStart === 'number')
      selection = input.value.slice(input.selectionStart, input.selectionEnd);
    const rect = element.getBoundingClientRect();
    event.preventDefault();
    send(JSON.stringify({link: link?.href || '', image: image?.currentSrc || image?.src || '',
      selection: selection.slice(0, 16384), editable, password,
      pointer: event.button === 2, x: event.clientX || rect.x, y: event.clientY || rect.y}));
  });
})();
