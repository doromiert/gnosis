// SPDX-License-Identifier: GPL-3.0-or-later
// Modal text editing stays inside the isolated renderer: field values never
// travel through the shell's command/binding channel.
(() => {
    const active = () => {let n=document.activeElement;while(n?.shadowRoot?.activeElement)n=n.shadowRoot.activeElement;return n;};
    Object.defineProperty(globalThis,'gnosisVimField',{value:Object.freeze({create({changed,send}){
        let target=null,entered=false,insert=false,visual=false,cursor=0,anchor=0,pending='',digits='',editing=false,hidden=false,register='';
        const node=()=>{const n=target?.deref();return n?.isConnected?n:null;};
        const model=()=>{const n=node();return n&&typeof n.selectionStart==='number'?{n,text:n.value,start:n.selectionStart,end:n.selectionEnd}:null;};
        const selection=()=>{const n=node(),s=getSelection();if(!n||!s)return null;if(!n.contains(s.anchorNode)){const r=document.createRange();r.selectNodeContents(n);r.collapse(true);s.removeAllRanges();s.addRange(r);}return s;};
        const segmenter=new Intl.Segmenter(undefined,{granularity:'grapheme'});
        const boundaries=text=>[...segmenter.segment(text)].map(s=>s.index).concat(text.length);
        const stepChar=(text,pos,direction)=>{const points=boundaries(text);return direction>0?(points.find(i=>i>pos)??text.length):(points.filter(i=>i<pos).at(-1)??0);};
        const snap=(text,pos)=>boundaries(text).filter(i=>i<=pos).at(-1)??0;
        const point=(pos,extend=false)=>{const m=model();if(!m)return;cursor=snap(m.text,Math.max(0,Math.min(m.text.length,pos)));if(extend)m.n.setSelectionRange(Math.min(anchor,cursor),Math.max(anchor,cursor),cursor<anchor?'backward':'forward');else m.n.setSelectionRange(cursor,stepChar(m.text,cursor,1));};
        const collapse=pos=>{const m=model();if(m){cursor=Math.max(0,Math.min(m.text.length,pos));m.n.setSelectionRange(cursor,cursor);}else selection()?.collapseToStart();};
        const bounds=()=>{const m=model();if(!m)return null;const start=m.text.lastIndexOf('\n',Math.max(-1,cursor-1))+1;let end=m.text.indexOf('\n',cursor);if(end<0)end=m.text.length;return{...m,lineStart:start,lineEnd:end};};
        const classify=c=>!c||/\s/.test(c)?0:/[\p{L}\p{N}_]/u.test(c)?1:2;
        const word=(text,pos,key)=>{if(key==='b'){pos=Math.max(0,pos-1);while(pos>0&&!classify(text[pos]))pos--;const k=classify(text[pos]);while(pos>0&&classify(text[pos-1])===k)pos--;return pos;}if(key==='e'){if(pos<text.length-1)pos++;while(pos<text.length&&!classify(text[pos]))pos++;const k=classify(text[pos]);while(pos<text.length-1&&classify(text[pos+1])===k)pos++;return pos;}const k=classify(text[pos]);while(pos<text.length&&classify(text[pos])===k)pos++;while(pos<text.length&&!classify(text[pos]))pos++;return pos;};
        const nativeEdit=(operation,value=null)=>{editing=true;try{document.execCommand(operation,false,value);}finally{editing=false;}const m=model();if(m){cursor=m.n.selectionStart;if(insert)collapse(cursor);else point(cursor);}changed();};
        const range=(start,end)=>{const m=model();if(m)m.n.setSelectionRange(Math.max(0,start),Math.min(m.text.length,end));};
        const move=(key,count)=>{
            const m=model();if(m){let pos=cursor;for(let i=0;i<count;i++){
                const start=m.text.lastIndexOf('\n',Math.max(-1,pos-1))+1;let end=m.text.indexOf('\n',pos);if(end<0)end=m.text.length;
                if(key==='h')pos=stepChar(m.text,pos,-1);else if(key==='l')pos=stepChar(m.text,pos,1);
                else if(key==='w'||key==='b'||key==='e')pos=word(m.text,pos,key);
                else if(key==='0'||key==='^')pos=start+(key==='^'?(m.text.slice(start,end).match(/^\s*/)?.[0].length||0):0);
                else if(key==='$')pos=Math.max(start,end-1);
                else if(key==='gg')pos=0;else if(key==='G')pos=m.text.length;
                else if(key==='j'){if(end===m.text.length)break;const ns=end+1,ne=m.text.indexOf('\n',ns);pos=Math.min(ns+pos-start,ne<0?m.text.length:ne);}
                else if(key==='k'){if(!start)break;const pe=start-1,ps=m.text.lastIndexOf('\n',pe-1)+1;pos=Math.min(ps+pos-start,pe);}
            }point(pos,visual);return;}
            if(hidden)return;const s=selection();if(!s)return;
            const direction=['h','b','k','0','^','gg'].includes(key)?'backward':'forward';
            const unit=['w','b','e'].includes(key)?'word':['j','k'].includes(key)?'line':['0','^','$'].includes(key)?'lineboundary':['gg','G'].includes(key)?'documentboundary':'character';
            for(let i=0;i<count;i++)s.modify(visual?'extend':'move',direction,unit);
        };
        const startInsert=kind=>{const b=bounds();if(b){let at=cursor;if(kind==='a')at=stepChar(b.text,at,1);if(kind==='A')at=b.lineEnd;if(kind==='I')at=b.lineStart;collapse(at);}else if(!hidden){if(kind==='a')selection()?.modify('move','forward','character');if(kind==='A')selection()?.modify('move','forward','lineboundary');if(kind==='I')selection()?.modify('move','backward','lineboundary');}
            insert=true;visual=false;pending=digits='';changed();};
        const remove=(kind,count=1,change=false)=>{
            const b=bounds();if(b){let start=cursor,end=cursor;
                if(visual){start=b.start;end=b.end;}
                else if(kind==='line'){start=b.lineStart;end=b.lineEnd<b.text.length?b.lineEnd+1:b.lineEnd;}
                else if(kind==='word'){for(let i=0;i<count;i++)end=word(b.text,end,'w');}
                else if(kind==='back-word'){start=word(b.text,cursor,'b');}
                else if(kind==='end')end=b.lineEnd;
                else if(kind==='back-char'){for(let i=0;i<count;i++)start=stepChar(b.text,start,-1);}
                else {for(let i=0;i<count;i++)end=stepChar(b.text,end,1);}
                register=b.text.slice(start,end);range(start,end);cursor=start;
            }else if(!hidden){const s=selection();if(!s)return;if(!visual){if(kind==='line')s.modify('move','backward','lineboundary');s.modify('extend',kind.startsWith('back-')?'backward':'forward',kind==='line'||kind==='end'?'lineboundary':kind.includes('word')?'word':'character');}}
            else return;
            if(!b)register=getSelection()?.toString()||'';
            nativeEdit('delete');visual=false;if(change)startInsert('i');
        };
        const normal=()=>{insert=false;visual=false;pending=digits='';const m=model();if(m){cursor=m.n.selectionStart;if(insert)collapse(cursor);else point(cursor);}changed();};
        const leave=()=>{const n=node();entered=false;insert=visual=false;pending=digits='';register='';if(!hidden)n?.blur();target=null;changed();};
        const command=operation=>{
            if(!entered||!node())return false;
            const m=model();if(insert&&m)cursor=m.start;pending=digits='';
            if(operation==='undo'||operation==='redo'){nativeEdit(operation);return true;}
            if(operation==='delete-word'){remove('back-word');return true;}
            if(operation==='backspace'){if(insert)remove('back-char');else move('h',1);return true;}
            if(operation==='scroll-down'||operation==='scroll-up'){node().scrollBy?.({top:node().clientHeight/2*(operation==='scroll-up'?-1:1),behavior:'instant'});return true;}
            return false;
        };
        addEventListener('beforeinput',event=>{if(entered&&!insert&&!editing&&!/^history(Undo|Redo)$/.test(event.inputType)&&event.isTrusted)event.preventDefault();},{capture:true});
        return Object.freeze({get active(){return entered;},get inserting(){return insert;},get editing(){return editing;},target:node,
            enter(n,opaque=false){target=new WeakRef(n);hidden=opaque;entered=true;insert=visual=false;pending=digits='';if(!opaque)n.focus({preventScroll:true});const m=model();cursor=m?.start||0;if(m)point(cursor);changed();},leave,command,
            key(event){
                if(!entered||!node()){if(entered)leave();return false;}
                const key=event.key;
                if(key==='Escape'){if(insert||visual||pending||digits)normal();else leave();return true;}
                if(insert)return false;
                if(key===':'){pending=digits='';return false;}
                if(event.ctrlKey||event.altKey||event.metaKey)return false;
                if(event.isComposing||key==='Dead'||key==='Process')return true;
                if(/^[1-9]$/.test(key)||(key==='0'&&digits)){digits=(digits+key).slice(0,3);return true;}
                const count=Math.min(999,parseInt(digits,10)||1);digits='';
                if(pending){const op=pending;pending='';if(op==='g'&&key==='g')move('gg',1);else if((op==='d'||op==='c')&&(key==='d'||key==='c'||key==='w'||key==='$'))remove(key==='w'?'word':key==='$'?'end':'line',count,op==='c');else if(op==='y'&&(key==='y'||key==='w')){const b=bounds();if(b){register=b.text.slice(key==='y'?b.lineStart:cursor,key==='y'?b.lineEnd:word(b.text,cursor,'w'));point(cursor);}}return true;}
                if('hljkwbe0^$G'.includes(key))move(key,count);
                else if(key==='g'||key==='d'||key==='c'||key==='y')pending=key;
                else if('iaIA'.includes(key))startInsert(key);
                else if(key==='x'||key==='X')remove(key==='x'?'char':'back-char',count);
                else if(key==='D'||key==='C')remove('end',count,key==='C');
                else if(key==='s')remove('char',count,true);
                else if(key==='v'){visual=!visual;anchor=cursor;point(cursor,visual);changed();}
                else if(key==='u')command('undo');
                else if(key==='p'&&register){const m=model();collapse(m?stepChar(m.text,cursor,1):cursor+1);nativeEdit('insertText',register);}
                else if(key==='Enter'){/* Enter is never an insert-mode shortcut. */}
                return true;
            }
        });
    }}),configurable:false,writable:false});
})();
