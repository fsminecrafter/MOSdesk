(function(){
var G=window,def=function(o,n,f){Object.defineProperty(o,n,{value:f,writable:true,configurable:true});},
 acc=function(o,n,g,s){Object.defineProperty(o,n,{get:g,set:s,configurable:true});};
var NP=Node.prototype,EP=Element.prototype;
var K={ELEMENT_NODE:1,ATTRIBUTE_NODE:2,TEXT_NODE:3,CDATA_SECTION_NODE:4,PROCESSING_INSTRUCTION_NODE:7,COMMENT_NODE:8,
 DOCUMENT_NODE:9,DOCUMENT_TYPE_NODE:10,DOCUMENT_FRAGMENT_NODE:11,DOCUMENT_POSITION_DISCONNECTED:1,DOCUMENT_POSITION_PRECEDING:2,
 DOCUMENT_POSITION_FOLLOWING:4,DOCUMENT_POSITION_CONTAINS:8,DOCUMENT_POSITION_CONTAINED_BY:16,DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC:32};
for(var k in K){Object.defineProperty(Node,k,{value:K[k]});if(!(k in NP))Object.defineProperty(NP,k,{value:K[k]});}
def(NP,'getRootNode',function(){var n=this;while(n.parentNode)n=n.parentNode;return n;});
def(NP,'compareDocumentPosition',function(o){if(o===this)return 0;
 var chain=function(n){var a=[];while(n){a.unshift(n);n=n.parentNode;}return a;};var a=chain(this),b=chain(o);
 if(a[0]!==b[0])return 1|32|(a[0]<b[0]?2:4);var i=0;while(i<a.length&&i<b.length&&a[i]===b[i])i++;
 if(i===a.length)return 16|4;if(i===b.length)return 8|2;var p=a[i-1].childNodes;
 for(var j=0;j<p.length;j++){if(p[j]===a[i])return 4;if(p[j]===b[i])return 2;}return 4;});
def(NP,'isSameNode',function(o){return this===o;});
if(!NP.isEqualNode)def(NP,'isEqualNode',function(o){return !!o&&this.nodeType===o.nodeType&&(this.outerHTML!==undefined?this.outerHTML===o.outerHTML:this.textContent===o.textContent);});
/* <template>.content: a fragment that holds the template's children */
acc(EP,'content',function(){if(this.tagName!=='TEMPLATE')return this.getAttribute('content');
 var f=this.__mxContent;if(!f){f=document.createDocumentFragment();def(this,'__mxContent',f);}
 while(this.firstChild)f.appendChild(this.firstChild);return f;},function(v){if(this.tagName!=='TEMPLATE')this.setAttribute('content',v);});
/* shadow roots: the host renders its shadow content in place of its children */
var attach=EP.attachShadow;
def(EP,'attachShadow',function(o){if(this.__mxShadow)throw new DOMException("Shadow root cannot be created on a host which already hosts a shadow tree.","NotSupportedError");
 var r=attach?attach.call(this,o):this;def(this,'__mxShadow',r);def(this,'__mxShadowMode',o&&o.mode||'open');return r;});
acc(EP,'shadowRoot',function(){return this.__mxShadowMode==='open'?this.__mxShadow:null;});
acc(EP,'host',function(){return this.__mxShadow===this?this:undefined;});
acc(EP,'elements',function(){if(this.tagName!=='FORM'&&this.tagName!=='FIELDSET')return undefined;
 var l=this.querySelectorAll('input,select,textarea,button,fieldset,output,object');var me=this;
 Array.prototype.forEach.call(l,function(e){if(e.name&&!(e.name in l))l[e.name]=e;});l.namedItem=function(n){return l[n]||null;};return l;});
if(typeof G.DOMException!=='function'){var DE=function(m,n){this.message=m||'';this.name=n||'Error';this.code=0;};DE.prototype=Object.create(Error.prototype);DE.prototype.constructor=DE;G.DOMException=DE;}
/* CSS.escape */
if(G.CSS)def(CSS,'escape',function(v){var s=String(v),out='';for(var i=0;i<s.length;i++){var c=s.charCodeAt(i),ch=s.charAt(i);
 if(c===0){out+='�';continue;}
 if((c>=1&&c<=31)||c===127||(i===0&&c>=48&&c<=57)||(i===1&&c>=48&&c<=57&&s.charCodeAt(0)===45)){out+='\\'+c.toString(16)+' ';continue;}
 if(i===0&&c===45&&s.length===1){out+='\\'+ch;continue;}
 if(c>=128||c===45||c===95||(c>=48&&c<=57)||(c>=65&&c<=90)||(c>=97&&c<=122)){out+=ch;continue;}out+='\\'+ch;}return out;});
/* structuredClone with Map, Set, Date, RegExp, typed arrays and cycles */
def(G,'structuredClone',function(v){var seen=new Map();var c=function(x){
 if(x===null||typeof x!=='object'){if(typeof x==='function'||typeof x==='symbol')throw new DOMException(String(x)+' could not be cloned.','DataCloneError');return x;}
 if(seen.has(x))return seen.get(x);var r;
 if(x instanceof Date)r=new Date(x.getTime());else if(x instanceof RegExp)r=new RegExp(x.source,x.flags);
 else if(x instanceof Map){r=new Map();seen.set(x,r);x.forEach(function(val,key){r.set(c(key),c(val));});return r;}
 else if(x instanceof Set){r=new Set();seen.set(x,r);x.forEach(function(val){r.add(c(val));});return r;}
 else if(typeof ArrayBuffer!=='undefined'&&ArrayBuffer.isView&&ArrayBuffer.isView(x))r=x.slice();
 else if(Array.isArray(x)){r=[];seen.set(x,r);for(var i=0;i<x.length;i++)r[i]=c(x[i]);return r;}
 else if(x instanceof Error){r=new Error(x.message);r.name=x.name;}
 else if(x.nodeType)throw new DOMException('Node could not be cloned.','DataCloneError');
 else{r={};seen.set(x,r);for(var k in x)if(Object.prototype.hasOwnProperty.call(x,k))r[k]=c(x[k]);return r;}
 seen.set(x,r);return r;};return c(v);});
/* MutationObserver: the engine hands over raw tree changes (__mxMODeliver) */
var regs=[];
var MO=function(cb){if(typeof cb!=='function')throw new TypeError("Failed to construct 'MutationObserver': parameter 1 is not of type 'Function'.");this._cb=cb;this._q=[];};
def(MO.prototype,'observe',function(target,o){o=Object.assign({},o||{});
 if((o.attributeOldValue||o.attributeFilter)&&o.attributes===undefined)o.attributes=true;
 if(o.characterDataOldValue&&o.characterData===undefined)o.characterData=true;
 if(!o.childList&&!o.attributes&&!o.characterData)throw new TypeError("The options object must set at least one of 'attributes', 'characterData', or 'childList' to true.");
 for(var i=0;i<regs.length;i++)if(regs[i].mo===this&&regs[i].target===target){regs[i].o=o;return;}
 regs.push({mo:this,target:target,o:o});__mxMOEnable(true);});
def(MO.prototype,'disconnect',function(){var me=this;regs=regs.filter(function(r){return r.mo!==me;});this._q=[];if(!regs.length)__mxMOEnable(false);});
def(MO.prototype,'takeRecords',function(){var q=this._q;this._q=[];return q;});
var TYPES=['childList','childList','','attributes','characterData'];
def(G,'__mxMODeliver',function(recs){var touched=[];
 for(var i=0;i<recs.length;i++){var r=recs[i],type=TYPES[r[0]],target=r[1];
  for(var j=0;j<regs.length;j++){var g=regs[j],o=g.o;if(!o[type])continue;
   if(target!==g.target&&!(o.subtree&&g.target.contains&&g.target.contains(target)))continue;
   if(type==='attributes'&&o.attributeFilter&&o.attributeFilter.indexOf(r[3])<0)continue;
   var keepOld=(type==='attributes'&&o.attributeOldValue)||(type==='characterData'&&o.characterDataOldValue);
   g.mo._q.push({type:type,target:target,addedNodes:r[0]===0?[r[2]]:[],removedNodes:r[0]===1?[r[2]]:[],
    previousSibling:r[5],nextSibling:r[6],attributeName:r[3],attributeNamespace:null,oldValue:keepOld?r[4]:null});
   if(touched.indexOf(g.mo)<0)touched.push(g.mo);}}
 for(var t=0;t<touched.length;t++){var mo=touched[t],q=mo._q;mo._q=[];if(!q.length)continue;
  try{mo._cb.call(mo,q,mo);}catch(e){console.error(e);}}});
Object.defineProperty(G,'__mxMODeliver',{enumerable:false});Object.defineProperty(G,'__mxMOEnable',{enumerable:false});
G.MutationObserver=MO;G.WebKitMutationObserver=MO;
})();
