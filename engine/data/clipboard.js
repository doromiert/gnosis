((mainWorld, readText, read) => {
  const post = window.__gnosisClipboardWrite;
  const generation = Math.random().toString(36).slice(2);
  const pending = new Map(); let sequence = 0;
  const limit = 64 * 1024 * 1024;
  const encode = bytes => {
    let value = '';
    for (let i = 0; i < bytes.length; i += 16384) value += String.fromCharCode(...bytes.subarray(i, i + 16384));
    return btoa(value);
  };
  const write = items => new Promise((resolve, reject) => {
    const id = ++sequence;
    const timer = setTimeout(() => { pending.delete(id); reject(new DOMException('The native clipboard did not respond.', 'NotAllowedError')); }, 10000);
    pending.set(id, {resolve, reject, timer});
    post(JSON.stringify({generation, id, items}));
  });
  Object.defineProperty(window, '__gnosisClipboardResult', {value: (token, id, ok) => {
    if (token !== generation) return;
    const request = pending.get(id); if (!request) return;
    pending.delete(id); clearTimeout(request.timer);
    if (ok) request.resolve(); else request.reject(new DOMException('Could not write to the native clipboard.', 'NotAllowedError'));
  }});
  if (window.isSecureContext) {
    const clipboard = navigator.clipboard || {};
    if (!navigator.clipboard) Object.defineProperty(navigator, 'clipboard', {value:clipboard});
    const NativeBlob = Blob;
    // CEF's context callback precedes conditional global interface installation.
    let NativeClipboardItem = globalThis.ClipboardItem;
    const parse = JSON.parse, unbase64 = atob;
    const fromBytes = Uint8Array.from.bind(Uint8Array);
    const decoder = new TextDecoder();
    const decodeText = decoder.decode.bind(decoder);
    const createDocument = document.implementation.createHTMLDocument.bind(document.implementation);
    const getBody = Object.getOwnPropertyDescriptor(Document.prototype,'body').get;
    const setHTML = Element.prototype.setHTML;
    const getHTML = Object.getOwnPropertyDescriptor(Element.prototype,'innerHTML').get;
    const query = Element.prototype.querySelector;
    const NativeXMLSerializer = XMLSerializer, serialize = XMLSerializer.prototype.serializeToString;
    const invoke = Reflect.apply;
    let inert, serializer;
    const sanitize = (text, svg) => {
      if (!setHTML) throw new DOMException('Native HTML sanitization is unavailable.', 'NotSupportedError');
      if (!inert) {
        inert = invoke(getBody, createDocument(''), []);
        serializer = new NativeXMLSerializer();
      }
      // Safe setHTML always removes XSS, while this config retains inline formatting.
      invoke(setHTML, inert, [text, {sanitizer:{removeElements:['style'],removeAttributes:[]}}]);
      if (!svg) return invoke(getHTML, inert, []);
      const root = invoke(query, inert, ['svg']);
      if (!root) throw new DOMException('The clipboard SVG has no SVG document.', 'DataError');
      return invoke(serialize, serializer, [root]);
    };
    const sequence = value => {
      if (value === null || !['object','function'].includes(typeof value) || typeof value[Symbol.iterator] !== 'function')
        throw new TypeError('Clipboard formats must be a sequence.');
      return Array.from(value, item => {
        if (typeof item === 'symbol') throw new TypeError('Clipboard formats must be strings.');
        return String(item);
      });
    };
    Object.defineProperties(clipboard, {
      ...(read ? {read: {configurable:true, value: async (options = {}) => {
        if (options !== null && !['object','function'].includes(typeof options)) throw new TypeError('Clipboard options must be an object.');
        const types = options?.types;
        const unsanitized = options?.unsanitized;
        const normalized = {};
        if (types !== undefined) normalized.types = types === null ? null : sequence(types);
        if (unsanitized !== undefined) normalized.unsanitized = sequence(unsanitized);
        const rawHTML = normalized.unsanitized?.includes('text/html');
        const reply = await read(normalized);
        if (!reply.ok) throw new DOMException('Could not read the native clipboard.', reply.text || 'NotAllowedError');
        NativeClipboardItem ??= globalThis.ClipboardItem;
        const values = {};
        for (const item of parse(reply.text)) {
          let bytes = fromBytes(unbase64(item.data), c => c.charCodeAt(0));
          if ((item.type === 'text/html' && !rawHTML) || item.type === 'image/svg+xml')
            bytes = sanitize(decodeText(bytes), item.type === 'image/svg+xml');
          const type = item.type === 'application/x-figma' ? 'web application/x-figma' : item.type;
          values[type] = new NativeBlob([bytes], {type:item.type});
        }
        return Object.keys(values).length ? [new NativeClipboardItem(values)] : [];
      }}} : {}),
      ...(readText ? {readText: {configurable:true, value: () => readText().then(reply => {
        if (!reply.ok) throw new DOMException('Could not read the native clipboard.', 'NotAllowedError');
        return reply.text;
      })}} : {}),
      writeText: {configurable:true, value: text => {
        text = String(text);
        if (!document.hasFocus()) return Promise.reject(new DOMException('The document is not focused.', 'NotAllowedError'));
        if (new TextEncoder().encode(text).length > limit) return Promise.reject(new DOMException('Clipboard content is too large.', 'DataError'));
        return write([{type:'text/plain', text}]);
      }},
      write: {configurable:true, value: async values => {
        if (!document.hasFocus()) throw new DOMException('The document is not focused.', 'NotAllowedError');
        if (!values.length) throw new DOMException('No clipboard items.', 'DataError');
        const items = []; let size = 0;
        // Desktop clipboards hold one item with several alternative formats.
        for (const type of values[0].types) {
          const blob = await values[0].getType(type); size += blob.size;
          if (size > limit) throw new DOMException('Clipboard content is too large.', 'DataError');
          items.push({type:type.replace(/^web /,''), data:encode(new Uint8Array(await blob.arrayBuffer()))});
        }
        return write(items);
      }}
    });
  }
  const selected = () => {
    const el = document.activeElement;
    if (el?.matches('input:not([type="password"]),textarea') && Number.isInteger(el.selectionStart)) return [{type:'text/plain',text:el.value.slice(el.selectionStart,el.selectionEnd)}];
    const selection = window.getSelection(), text = selection?.toString() || '';
    if (!text) return [];
    const items = [{type:'text/plain',text}];
    if (selection.rangeCount) { const container = document.createElement('div'); container.append(selection.getRangeAt(0).cloneContents()); items.push({type:'text/html',text:container.innerHTML}); }
    return items;
  };
  const records = new WeakMap();
  const normalize = type => type === 'text' ? 'text/plain' : type === 'url' ? 'text/uri-list' : String(type).toLowerCase();
  const setData = DataTransfer.prototype.setData, clearData = DataTransfer.prototype.clearData, getData = DataTransfer.prototype.getData;
  const types = Object.getOwnPropertyDescriptor(DataTransfer.prototype,'types');
  if (types?.configurable) Object.defineProperty(DataTransfer.prototype,'types',{...types,get() { const record = records.get(this); return record ? Object.freeze([...record.items.keys()]) : types.get.call(this); }});
  DataTransfer.prototype.getData = function(type) { const record = records.get(this); return record ? (record.items.get(normalize(type)) || '') : getData.call(this,type); };
  DataTransfer.prototype.setData = function(type, value) {
    const record = records.get(this);
    if (record) { record.items.set(normalize(type),String(value)); return; }
    return setData.call(this,type,value);
  };
  DataTransfer.prototype.clearData = function(type) {
    const record = records.get(this);
    if (record) { if (arguments.length) record.items.delete(normalize(type)); else record.items.clear(); return; }
    return arguments.length ? clearData.call(this,type) : clearData.call(this);
  };
  for (const type of ['copy','cut']) window.addEventListener(type, event => {
    const record = {items:new Map(),fallback:mainWorld ? selected() : [],done:false}, transfer = event.clipboardData;
    if (transfer) records.set(transfer,record);
    const finish = useDefault => {
      if (record.done) return; record.done = true;
      const items = [...record.items].map(([type,text]) => ({type,text}));
      if (!items.length && useDefault) items.push(...record.fallback.filter(item => item.text));
      if (transfer) records.delete(transfer);
      if (items.length) write(items).then(() => {
        if (useDefault && type === 'cut' && record.fallback[0]?.text === selected()[0]?.text) document.execCommand('delete');
      }).catch(() => {});
    };
    // Install the final bubble listener during capture, after the page's
    // existing listeners. Let the website populate the data first, then suppress
    // Chromium's own write, which otherwise races GTK for clipboard ownership.
    const end = e => {
      if (e !== event) return;
      const useDefault = !event.defaultPrevented;
      if (mainWorld || record.items.size) event.preventDefault();
      finish(useDefault);
    };
    window.addEventListener(type,end);
    setTimeout(() => {
      window.removeEventListener(type,end);
      // Editors that stop propagation normally also prevent the default;
      // setData snapshots survive beyond the event's protected lifetime.
      finish(!event.defaultPrevented);
    },0);
  },true);
})
