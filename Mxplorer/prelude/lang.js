(function(){
var G=window,def=function(o,n,f){Object.defineProperty(o,n,{value:f,writable:true,configurable:true});};
var P=Promise,PP=P.prototype;
def(PP,'finally',function(f){var C=this;if(typeof f!=='function')return this.then(f,f);
 return this.then(function(v){return P.resolve(f()).then(function(){return v;});},
  function(e){return P.resolve(f()).then(function(){throw e;});});});
var each=function(list,fn){var a=Array.from(list);for(var i=0;i<a.length;i++)fn(a[i],i);return a.length;};
def(P,'all',function(list){return new P(function(res,rej){var out=[],left=0,n=each(list,function(x,i){left++;
 P.resolve(x).then(function(v){out[i]=v;if(--left===0)res(out);},rej);});out.length=n;if(!left)res(out);});});
def(P,'allSettled',function(list){return new P(function(res){var out=[],left=0,n=each(list,function(x,i){left++;
 P.resolve(x).then(function(v){out[i]={status:'fulfilled',value:v};if(--left===0)res(out);},
  function(e){out[i]={status:'rejected',reason:e};if(--left===0)res(out);});});out.length=n;if(!left)res(out);});});
def(P,'race',function(list){return new P(function(res,rej){each(list,function(x){P.resolve(x).then(res,rej);});});});
def(P,'any',function(list){return new P(function(res,rej){var errs=[],left=0,n=each(list,function(x,i){left++;
 P.resolve(x).then(res,function(e){errs[i]=e;if(--left===0){var a=new AggregateError(errs,'All promises were rejected');a.errors=errs;rej(a);}});});
 if(!n){var a=new AggregateError([],'All promises were rejected');a.errors=[];rej(a);}});});
def(P,'withResolvers',function(){var o={};o.promise=new P(function(a,b){o.resolve=a;o.reject=b;});return o;});
def(P,'try',function(f){var args=[].slice.call(arguments,1);return new P(function(r){r(f.apply(undefined,args));});});
var AP=Array.prototype;
def(AP,'toSorted',function(c){return this.slice().sort(c);});
def(AP,'toReversed',function(){return this.slice().reverse();});
def(AP,'toSpliced',function(){var a=this.slice();a.splice.apply(a,arguments);return a;});
def(AP,'with',function(i,v){var n=this.length;i=Math.trunc(+i)||0;if(i<0)i+=n;if(i<0||i>=n)throw new RangeError('Invalid index : '+i);var a=this.slice();a[i]=v;return a;});
def(AP,'copyWithin',function(t,s,e){var n=this.length,rel=function(x,d){x=x===undefined?d:Math.trunc(+x)||0;return x<0?Math.max(n+x,0):Math.min(x,n);};
 t=rel(t,0);s=rel(s,0);e=rel(e,n);var c=Math.min(e-s,n-t),tmp=this.slice(s,s+c);for(var i=0;i<c;i++)this[t+i]=tmp[i];return this;});
def(Array,'fromAsync',async function(src,map,self){var out=[],i=0;
 if(src&&typeof src[Symbol.asyncIterator]==='function'){for await(var v of src)out.push(map?await map.call(self,v,i++):v);}
 else{var a=Array.from(src);for(var k=0;k<a.length;k++){var w=await a[k];out.push(map?await map.call(self,w,k):w);}}return out;});
var groupBy=function(items,fn,map){var o=map?new Map():Object.create(null),i=0;each(items,function(x){var k=fn(x,i++);
 if(map){if(!o.has(k))o.set(k,[]);o.get(k).push(x);}else{if(!(k in o))o[k]=[];o[k].push(x);}});return o;};
def(Object,'groupBy',function(a,f){return groupBy(a,f,false);});
def(Map,'groupBy',function(a,f){return groupBy(a,f,true);});
def(Object,'getOwnPropertyDescriptors',function(o){var r={},k=Reflect.ownKeys(o);for(var i=0;i<k.length;i++){var d=Object.getOwnPropertyDescriptor(o,k[i]);if(d)r[k[i]]=d;}return r;});
def(String,'raw',function(s){var raw=s.raw,out='';for(var i=0;i<raw.length;i++){out+=raw[i];if(i+1<raw.length&&i+1<arguments.length)out+=arguments[i+1];}return out;});
def(String.prototype,'isWellFormed',function(){return true;});
def(String.prototype,'toWellFormed',function(){return String(this);});
/* symbols are strings inside the engine ("@@sym:desc:n"); give them a prototype */
var symDesc=function(s){s=String(s);var m=/^@@sym:([\s\S]*):\d+$/.exec(s);if(m)return m[1];m=/^@@for:([\s\S]*)$/.exec(s);if(m)return m[1];
 return s.slice(0,2)==='@@'?'Symbol.'+s.slice(2):undefined;};
if(!Symbol.prototype){var SyP={};def(SyP,'toString',function(){return 'Symbol('+(symDesc(this)||'')+')';});
 def(SyP,'valueOf',function(){return this;});Object.defineProperty(SyP,'description',{get:function(){return symDesc(this);},configurable:true});
 def(Symbol,'prototype',SyP);def(SyP,'constructor',Symbol);}
Object.defineProperty(String.prototype,'description',{get:function(){return symDesc(this);},configurable:true});
def(Symbol,'keyFor',function(s){var m=/^@@for:([\s\S]*)$/.exec(String(s));return m?m[1]:undefined;});
var SP=Set.prototype,setOf=function(x){return x instanceof Set?x:new Set(x.keys?Array.from(x.keys()):x);};
def(SP,'union',function(o){var r=new Set(this);setOf(o).forEach(function(v){r.add(v);});return r;});
def(SP,'intersection',function(o){var s=setOf(o),r=new Set();this.forEach(function(v){if(s.has(v))r.add(v);});return r;});
def(SP,'difference',function(o){var s=setOf(o),r=new Set();this.forEach(function(v){if(!s.has(v))r.add(v);});return r;});
def(SP,'symmetricDifference',function(o){var s=setOf(o),me=this,r=new Set();this.forEach(function(v){if(!s.has(v))r.add(v);});s.forEach(function(v){if(!me.has(v))r.add(v);});return r;});
def(SP,'isSubsetOf',function(o){var s=setOf(o),ok=true;this.forEach(function(v){if(!s.has(v))ok=false;});return ok;});
def(SP,'isSupersetOf',function(o){var me=this,ok=true;setOf(o).forEach(function(v){if(!me.has(v))ok=false;});return ok;});
def(SP,'isDisjointFrom',function(o){var s=setOf(o),ok=true;this.forEach(function(v){if(s.has(v))ok=false;});return ok;});
var parse=JSON.parse;
def(JSON,'parse',function(text,reviver){var v=parse(text);if(typeof reviver!=='function')return v;
 var walk=function(holder,key){var val=holder[key];if(val&&typeof val==='object'){var ks=Array.isArray(val)?Object.keys(val):Object.keys(val);
  for(var i=0;i<ks.length;i++){var nv=walk(val,ks[i]);if(nv===undefined)delete val[ks[i]];else val[ks[i]]=nv;}}
  return reviver.call(holder,key,val);};return walk({'':v},'');});
if(!G.WeakRef){var WR=function(t){if(t===null||(typeof t!=='object'&&typeof t!=='function'))throw new TypeError('WeakRef: invalid target');this._t=t;};
 def(WR.prototype,'deref',function(){return this._t;});G.WeakRef=WR;}
if(!G.FinalizationRegistry){var FR=function(cb){this._cb=cb;};def(FR.prototype,'register',function(){});def(FR.prototype,'unregister',function(){return false;});G.FinalizationRegistry=FR;}
if(!G.Blob){var B=function(parts,opt){var s='';parts=parts||[];for(var i=0;i<parts.length;i++){var p=parts[i];s+=p instanceof B?p._s:
  (p&&p.byteLength!==undefined&&typeof TextDecoder!=='undefined')?new TextDecoder().decode(p):String(p);}
  this._s=s;this.type=opt&&opt.type?String(opt.type).toLowerCase():'';this.size=typeof TextEncoder!=='undefined'?new TextEncoder().encode(s).length:s.length;};
 def(B.prototype,'text',function(){return Promise.resolve(this._s);});
 def(B.prototype,'arrayBuffer',function(){return Promise.resolve(new TextEncoder().encode(this._s).buffer);});
 def(B.prototype,'slice',function(a,b,t){return new B([this._s.slice(a,b)],{type:t||''});});
 G.Blob=B;var F=function(parts,name,opt){B.call(this,parts,opt);this.name=String(name);this.lastModified=(opt&&opt.lastModified)||Date.now();};
 F.prototype=Object.create(B.prototype);F.prototype.constructor=F;G.File=F;}
if(!G.FormData){var FD=function(form){this._e=[];if(form&&form.elements){for(var i=0;i<form.elements.length;i++){var el=form.elements[i];
  if(!el.name||el.disabled)continue;var t=(el.type||'').toLowerCase();if((t==='checkbox'||t==='radio')&&!el.checked)continue;
  if(t==='submit'||t==='button'||t==='reset'||t==='file')continue;this._e.push([el.name,String(el.value)]);}}};
 var fp=FD.prototype;def(fp,'append',function(k,v){this._e.push([String(k),v instanceof Blob?v:String(v)]);});
 def(fp,'set',function(k,v){this.delete(k);this.append(k,v);});
 def(fp,'get',function(k){for(var i=0;i<this._e.length;i++)if(this._e[i][0]===String(k))return this._e[i][1];return null;});
 def(fp,'getAll',function(k){return this._e.filter(function(e){return e[0]===String(k);}).map(function(e){return e[1];});});
 def(fp,'has',function(k){return this.get(k)!==null;});
 def(fp,'delete',function(k){this._e=this._e.filter(function(e){return e[0]!==String(k);});});
 def(fp,'forEach',function(f,t){var me=this;this._e.forEach(function(e){f.call(t,e[1],e[0],me);});});
 def(fp,'entries',function(){return this._e.map(function(e){return [e[0],e[1]];})[Symbol.iterator]();});
 def(fp,'keys',function(){return this._e.map(function(e){return e[0];})[Symbol.iterator]();});
 def(fp,'values',function(){return this._e.map(function(e){return e[1];})[Symbol.iterator]();});
 def(fp,Symbol.iterator,fp.entries);G.FormData=FD;}
if(!G.Intl){var opts=function(o){return o||{};},pad=function(n){return n<10?'0'+n:''+n;};
 var NF=function(loc,o){this._o=opts(o);};
 def(NF.prototype,'format',function(n){var o=this._o;n=+n;var min=o.minimumFractionDigits,max=o.maximumFractionDigits;
  if(o.style==='percent')n*=100;if(max===undefined)max=o.style==='currency'?2:(o.style==='percent'?0:3);if(min===undefined)min=o.style==='currency'?2:0;if(max<min)max=min;
  var s=n.toFixed(max);if(max>min)s=s.replace(new RegExp('0{1,'+(max-min)+'}$'),'').replace(/\.$/,'');
  var parts=s.split('.');if(o.useGrouping!==false)parts[0]=parts[0].replace(/\B(?=(\d{3})+(?!\d))/g,',');s=parts.join('.');
  if(o.style==='percent')s+='%';if(o.style==='currency'){var sym={USD:'$',EUR:'€',GBP:'£',JPY:'¥',SEK:'kr '}[o.currency]||(o.currency+' ');s=s.charAt(0)==='-'?'-'+sym+s.slice(1):sym+s;}
  return s;});
 def(NF.prototype,'formatToParts',function(n){return [{type:'literal',value:this.format(n)}];});
 def(NF.prototype,'resolvedOptions',function(){return Object.assign({locale:'en-US',numberingSystem:'latn'},this._o);});
 var DF=function(loc,o){this._o=opts(o);};
 def(DF.prototype,'format',function(d){d=d===undefined?new Date():new Date(d);var o=this._o;
  var M=['January','February','March','April','May','June','July','August','September','October','November','December'];
  var W=['Sunday','Monday','Tuesday','Wednesday','Thursday','Friday','Saturday'];var out=[];
  if(o.weekday)out.push(o.weekday==='long'?W[d.getDay()]:W[d.getDay()].slice(0,3));
  if(o.month==='long'||o.month==='short'){var m=o.month==='long'?M[d.getMonth()]:M[d.getMonth()].slice(0,3);out.push(m+(o.day?' '+d.getDate():'')+(o.year?', '+d.getFullYear():''));}
  else if(o.year||o.month||o.day||(!o.hour&&!o.minute&&!o.timeStyle)){out.push((d.getMonth()+1)+'/'+d.getDate()+'/'+d.getFullYear());}
  if(o.hour||o.minute||o.timeStyle){var h=d.getHours(),h12=o.hour12!==false&&o.hourCycle!=='h23';
   out.push((h12?(h%12||12):pad(h))+':'+pad(d.getMinutes())+(o.second?':'+pad(d.getSeconds()):'')+(h12?(h<12?' AM':' PM'):''));}
  return out.join(', ');});
 def(DF.prototype,'formatToParts',function(d){return [{type:'literal',value:this.format(d)}];});
 def(DF.prototype,'resolvedOptions',function(){return Object.assign({locale:'en-US',calendar:'gregory',numberingSystem:'latn',timeZone:'UTC'},this._o);});
 var CO=function(loc,o){this._o=opts(o);};
 def(CO.prototype,'compare',function(a,b){a=String(a);b=String(b);if(this._o.sensitivity==='base'||this._o.sensitivity==='accent'){a=a.toLowerCase();b=b.toLowerCase();}
  if(this._o.numeric){var na=parseFloat(a),nb=parseFloat(b);if(!isNaN(na)&&!isNaN(nb)&&na!==nb)return na<nb?-1:1;}return a<b?-1:a>b?1:0;});
 def(CO.prototype,'resolvedOptions',function(){return {locale:'en-US',usage:'sort',sensitivity:'variant'};});
 var PR=function(loc,o){this._o=opts(o);};
 def(PR.prototype,'select',function(n){if(this._o.type==='ordinal'){var a=n%10,b=n%100;return a===1&&b!==11?'one':a===2&&b!==12?'two':a===3&&b!==13?'few':'other';}return n===1?'one':'other';});
 var RT=function(loc,o){this._o=opts(o);};
 def(RT.prototype,'format',function(v,u){u=String(u).replace(/s$/,'');var a=Math.abs(v),s=a+' '+u+(a===1?'':'s');return v<0?s+' ago':'in '+s;});
 var LF=function(loc,o){this._o=opts(o);};
 def(LF.prototype,'format',function(l){l=Array.from(l);var w=this._o.type==='disjunction'?'or':'and';return l.length<2?l.join(''):l.slice(0,-1).join(', ')+(l.length>2?',':'')+' '+w+' '+l[l.length-1];});
 var SG=function(loc,o){this._o=opts(o);};
 def(SG.prototype,'segment',function(s){s=String(s);var g=this._o.granularity||'grapheme',out=[],re=g==='word'?/\w+|\W/g:g==='sentence'?/[^.!?]+[.!?]*\s*/g:null,m,i=0;
  if(!re){var cs=Array.from(s);for(var k=0;k<cs.length;k++){out.push({segment:cs[k],index:i,input:s});i+=cs[k].length;}}
  else while((m=re.exec(s))){out.push({segment:m[0],index:m.index,input:s,isWordLike:g==='word'?/\w/.test(m[0]):undefined});}
  return out;});
 var I={NumberFormat:NF,DateTimeFormat:DF,Collator:CO,PluralRules:PR,RelativeTimeFormat:RT,ListFormat:LF,Segmenter:SG,
  getCanonicalLocales:function(l){return l===undefined?[]:[].concat(l);},supportedValuesOf:function(){return [];}};
 [NF,DF,CO,PR,RT,LF,SG].forEach(function(C){C.supportedLocalesOf=function(l){return l===undefined?[]:[].concat(l);};});
 G.Intl=I;}
})();
