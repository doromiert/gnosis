// SPDX-License-Identifier: GPL-3.0-or-later
// Browser-owned isolated world: normal mode selects controls; i edits them.
(() => {
    const send = globalThis.__jupiterDocumentMetadata;
    if (typeof send !== 'function' || globalThis.gnosisVim) return;
    const selector = 'a[href],area[href],button,input:not([type=hidden]),textarea,select,summary,[role="button"],[role="link"],[role="textbox"],[role="combobox"],[contenteditable]:not([contenteditable="false"]),[tabindex]:not([tabindex="-1"])';
    const alphabet = 'asdfghjkl';
    let field = null;
    let enabled = false, selected = null, selectedEditable = false, selectedOpaque = false, newTab = false;
    let prefix = '', prefixAt = 0, hints = [], typed = '', overlay = null, root = null, frame = 0, selecting = false;
    let accent = '#3584e4';
    const action = name => send(JSON.stringify({kind:'vim-action', action:name}));
    const active = () => { let n=document.activeElement; while(n?.shadowRoot?.activeElement)n=n.shadowRoot.activeElement; return n; };
    const current = () => { const n=selected?.deref(); return n?.isConnected?n:null; };
    const editable = n => n instanceof Element && (n.isContentEditable || n.matches('input:not([type=button]):not([type=submit]):not([type=reset]):not([type=checkbox]):not([type=radio]):not([type=range]):not([type=color]):not([type=file]),textarea,select,[role="textbox"],[role="searchbox"],[role="combobox"],[role="spinbutton"],[role="application"]'));
    const opaque = n => n instanceof Element && n!==document.body && n!==document.documentElement && !n.matches(selector);
    const usable = n => n instanceof Element && n.isConnected && !n.disabled && n.getAttribute('aria-disabled')!=='true' && !n.closest('[inert],[hidden],[aria-hidden="true"]');
    const rendered = n => { if(!usable(n))return false; const r=n.getBoundingClientRect(),s=getComputedStyle(n); return r.width>0&&r.height>0&&s.visibility==='visible'&&s.display!=='none'&&s.opacity!=='0'; };
    const visible = n => { if(!rendered(n))return false; const r=n.getBoundingClientRect(); return r.bottom>0&&r.right>0&&r.top<innerHeight&&r.left<innerWidth; };
    const controls = viewport => {
        const result=[]; let visited=0;
        const visit = scope => { for(const n of scope.querySelectorAll('*')) {
            if(++visited>10000||result.length>=2000)return;
            if(n.matches(selector)&&(viewport?visible(n):rendered(n)))result.push(n);
            if(n.shadowRoot)visit(n.shadowRoot);
        }};
        visit(document); return result;
    };
    const clearLayer = () => { overlay?.remove(); overlay=root=null; };
    const layer = () => {
        clearLayer(); overlay=document.createElement('div');
        overlay.style.cssText='all:initial!important;position:fixed!important;inset:0!important;pointer-events:none!important;z-index:2147483647!important';
        root=overlay.attachShadow({mode:'closed'});
        const style=document.createElement('style');
        style.textContent='.hint,.mode{position:fixed;box-sizing:border-box;font:700 13px system-ui;color:#fff;background:#383838;border:1px solid #555;border-radius:6px;padding:3px 6px;box-shadow:0 2px 6px #0005}.mode{bottom:18px;right:18px;font-weight:500;padding:10px 16px;border-radius:12px}.selection{position:fixed;box-sizing:border-box;border:2px solid '+accent+';border-radius:8px;box-shadow:0 0 0 2px #fff8,0 2px 8px #0005}';
        root.append(style); document.documentElement.append(overlay);
    };
    const render = () => {
        frame=0; if(!enabled||hints.length)return;
        const n=current(); if(!field?.inserting&&(!n||!visible(n))){clearLayer();return;}
        layer();
        if(!field?.inserting&&n){const r=n.getBoundingClientRect(),ring=document.createElement('div');ring.className='selection';Object.assign(ring.style,{left:r.left-3+'px',top:r.top-3+'px',width:r.width+6+'px',height:r.height+6+'px'});root.append(ring);}
        const badge=document.createElement('span');badge.className='mode';badge.setAttribute('role','status');
        badge.textContent=field?.inserting?'Field insert mode · Esc for normal':field?.active?'Field normal mode · i to type · Esc to leave':selectedEditable?'Page normal mode · Enter to enter field':'Page normal mode · Enter to activate';root.append(badge);
    };
    const redraw = () => {if(!frame)frame=requestAnimationFrame(render);};
    const clearHints = () => {hints=[];typed='';clearLayer();};
    const select = (n, tab=false, hiddenInput=false) => {
        if(!rendered(n))return;
        selected=new WeakRef(n);selectedOpaque=hiddenInput;selectedEditable=editable(n)||hiddenInput;newTab=tab;
        selecting=true;
        if(selectedEditable){
            // Keep normal mode out of ordinary text fields. A closed shadow
            // host cannot refocus its hidden input, so retain that focus and
            // gate its keys/beforeinput until the user explicitly presses i.
            if(!hiddenInput)active()?.blur();
        }else n.focus({preventScroll:true});
        selecting=false;
        n.scrollIntoView({block:'nearest',inline:'nearest',behavior:'instant'});redraw();
    };
    const move = (key,count) => {
        const nodes=controls(false);if(!nodes.length)return;
        const rectangles=new Map(nodes.map(n=>[n,n.getBoundingClientRect()]));
        let n=current();if(!n||!rectangles.has(n)){select(nodes.find(visible)||nodes[0]);return;}
        for(let i=0;i<count;i++){
            const r=rectangles.get(n),x=r.left+r.width/2,y=r.top+r.height/2;let best=null,score=Infinity;
            for(const candidate of nodes){if(candidate===n)continue;const c=rectangles.get(candidate),dx=c.left+c.width/2-x,dy=c.top+c.height/2-y;
                const vertical=key==='j'||key==='k',primary=(vertical?dy:dx)*(key==='k'||key==='h'?-1:1);if(primary<=1)continue;
                const cross=Math.abs(vertical?dx:dy),beam=vertical?(c.left<r.right&&c.right>r.left):(c.top<r.bottom&&c.bottom>r.top);
                const distance=primary+cross*(beam?1:5);if(distance<score){score=distance;best=candidate;}}
            if(!best)break;n=best;
        }
        select(n);
    };
    const tabMove = backwards => {const nodes=controls(false);if(!nodes.length)return;const index=nodes.indexOf(current());select(nodes[(index+(backwards?nodes.length-1:1)+nodes.length)%nodes.length]);};
    const showHints = linksOnly => {
        clearHints();layer();const nodes=controls(true).filter(n=>!linksOnly||n.matches('a[href],area[href]')).slice(0,400);
        if(!nodes.length){render();return;}
        const width=Math.max(1,Math.ceil(Math.log(nodes.length)/Math.log(alphabet.length)));
        hints=nodes.map((n,index)=>{let code='';for(let i=0;i<width;i++){code=alphabet[index%alphabet.length]+code;index=Math.floor(index/alphabet.length);}const label=document.createElement('span'),r=n.getBoundingClientRect();label.className='hint';label.textContent=code.toUpperCase();label.style.left=Math.max(2,Math.min(innerWidth-width*12-16,r.left))+'px';label.style.top=Math.max(2,Math.min(innerHeight-30,r.top))+'px';root.append(label);return {node:n,code,label,newTab:linksOnly};});
    };
    const hintKey = key => {
        if(key==='Backspace')typed=typed.slice(0,-1);else if(key.length===1&&alphabet.includes(key.toLowerCase()))typed+=key.toLowerCase();else return;
        const matches=hints.filter(h=>h.code.startsWith(typed));if(!matches.length){clearHints();render();return;}
        if(matches.length===1&&matches[0].code===typed){const h=matches[0];clearHints();if(visible(h.node))select(h.node,h.newTab);return;}
        for(const h of hints){h.label.hidden=!h.code.startsWith(typed);h.label.textContent=h.code.slice(typed.length).toUpperCase();}
    };
    const activate = () => {
        const n=current();if(!n||!rendered(n))return;if(selectedEditable){field.enter(n,selectedOpaque);return;}
        if(newTab&&n.matches('a[href],area[href]')){try{const url=new URL(n.href,location.href);if(!['http:','https:'].includes(url.protocol))return;const a=document.createElement('a');a.href=url.href;a.target='_blank';a.rel='noopener';a.click();}catch{}return;}
        n.click();
    };
    const normal = () => {prefix='';clearHints();render();};
    const scroller = () => {
        let n=current();while(n instanceof Element&&n!==document.documentElement){const s=getComputedStyle(n);if(n.scrollHeight>n.clientHeight&&/(auto|scroll)/.test(s.overflowY))return n;n=n.parentElement||n.getRootNode()?.host;}return document.scrollingElement;
    };
    const eat = event => {event.preventDefault();event.stopImmediatePropagation();};
    addEventListener('focusin',event=>{
        if(!enabled||selecting)return;
        if(field?.active){if(event.composedPath().includes(field.target()))return;field.leave();}
        const n=event.composedPath().find(n=>n instanceof Element&&n.matches(selector))||active();
        if(usable(n)&&(n.matches(selector)||opaque(n)))select(n,false,opaque(n));
    },{capture:true});
    addEventListener('pointerdown',event=>{
        if(!enabled||!event.isTrusted)return;
        if(field?.active){if(event.composedPath().includes(field.target()))return;field.leave();}
        const n=event.composedPath().find(n=>n instanceof Element&&n.matches(selector));
        if(n&&usable(n)){select(n);if(editable(n))event.preventDefault();}
    },{capture:true});
    addEventListener('beforeinput',event=>{if(enabled&&!field?.active&&event.isTrusted)event.preventDefault();},{capture:true});
    addEventListener('scroll',()=>{if(hints.length)clearHints();redraw();},{capture:true,passive:true});
    addEventListener('resize',()=>{if(hints.length)clearHints();redraw();});
    addEventListener('blur',()=>{prefix='';clearHints();send(JSON.stringify({kind:'vim-blur'}));});
    addEventListener('focus',()=>{if(document.hasFocus()&&!['iframe','frame'].includes(active()?.localName))send(JSON.stringify({kind:'vim-focus',active:field?.active||false}));redraw();});
    addEventListener('pagehide',()=>{field?.leave();selected=null;prefix='';clearHints();});
    addEventListener('keydown',event=>{
        if(!enabled||!event.isTrusted)return;
        if(field?.active){if(field.key(event)){eat(event);return;}if(field.inserting||event.key!==':')return;}
        if(event.key==='Escape'){normal();eat(event);return;}
        if(event.ctrlKey||event.altKey||event.metaKey){prefix='';return;}
        if(event.isComposing||event.key==='Process'||event.key==='Dead'){eat(event);return;}
        if(hints.length){hintKey(event.key);eat(event);return;}
        if(performance.now()-prefixAt>1000)prefix='';
        const key=event.key,previous=prefix;
        if(/^[1-9]$/.test(key)||(key==='0'&&/^[1-9]/.test(prefix))){prefix=(prefix+key).slice(0,3);prefixAt=performance.now();eat(event);return;}
        const count=Math.min(999,parseInt(prefix,10)||1);prefix='';
        const commands={':':'vim.commands','/':'win.find',n:'win.find-next',N:'win.find-prev',o:'win.location',t:'win.new-tab',H:'toolbar.navigation-back',L:'toolbar.navigation-forward',r:'toolbar.reload',x:'tab.close',J:'win.vim-tab-next',K:'win.vim-tab-previous'};
        if('hjkl'.includes(key))move(key,count);
        else if(key==='d'||key==='u'){const target=scroller();if(target)target.scrollBy({top:(target.clientHeight||innerHeight)/2*count*(key==='u'?-1:1),behavior:'instant'});}
        else if(key==='g'){if(previous==='g'){const nodes=controls(false);if(nodes.length)select(nodes[0]);else scrollTo(0,0);}else{prefix='g';prefixAt=performance.now();}}
        else if(key==='G'){const nodes=controls(false);if(nodes.length)select(nodes.at(-1));else scrollTo(0,document.scrollingElement.scrollHeight);}
        else if(key==='i'){if(previous==='g'){const n=controls(false).find(editable);if(n){select(n);field.enter(n);}}}
        else if(key==='f'||key==='F')showHints(key==='F');
        else if(key==='Tab')tabMove(event.shiftKey);
        else if(key==='Enter'||key===' '){activate();}
        else if(commands[key]){if(!event.repeat)action(commands[key]);}
        // Unknown printable keys must not edit an opaque/closed-shadow input.
        else if(!editable(active())&&!opaque(active()))return;
        eat(event);
    },{capture:true});
    field=globalThis.gnosisVimField.create({changed:()=>{send(JSON.stringify({kind:'vim-field',active:field?.active||false}));redraw();},send});
    Object.defineProperty(globalThis,'gnosisVim',{value:Object.freeze({command:operation=>field.command(operation),setEnabled(value,color){
        const was=enabled;enabled=value===true;if(typeof color==='string')accent=color;
        if(!enabled){field?.leave();selected=null;prefix='';clearHints();}
        else if(!was){const n=active();if(usable(n)&&(n.matches(selector)||opaque(n)))select(n,false,opaque(n));}
        redraw();
    }}),configurable:false,writable:false});
    send(JSON.stringify({kind:'vim-ready'}));
})();
