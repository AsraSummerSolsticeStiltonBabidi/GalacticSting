//==============================================================================
// ExtractReferenceNames.js - regenerates the fixture in tests/NoteNameTests.cpp.
//
// The body below (from "function M(n,m)" to the end of edoNames, plus upsDowns'
// uniqueness gate) is the REFERENCE implementation, copied verbatim out of the
// notation page this project ported: "Notation" by Richie Greene, MIT licence,
// https://github.com/richiegreene/notation . Nothing in it was edited - the only
// additions are the three ES3 shims at the top, which exist so Windows' own JScript
// engine (an ES3 engine, no Array.map / forEach / String.repeat) can run it with no
// Node install and no changes to the algorithm.
//
// Run it, and paste the tab-separated output into the kReference table:
//     cscript //nologo //E:JScript tools\ExtractReferenceNames.js
//
// This is what makes the C++ port checkable: the test asserts against names this
// program printed, not against names a human remembered.
//==============================================================================
// ES3 shims so the reference source runs unmodified under cscript //E:JScript.
if(!String.prototype.repeat){String.prototype.repeat=function(n){var s="";for(var i=0;i<n;i++)s+=this;return s;};}
if(!Array.prototype.forEach){Array.prototype.forEach=function(f){for(var i=0;i<this.length;i++)f(this[i],i,this);};}
if(!Array.prototype.map){Array.prototype.map=function(f){var r=[];for(var i=0;i<this.length;i++)r.push(f(this[i],i,this));return r;};}
function M(n,m){return((n%m)+m)%m}
function edoNames(m){
  if(m<7&&m!==5) return null;
  var p5=Math.round(Math.log(1.5)/Math.log(2)*m);
  var p2=p5*2-m;
  var a1=((m%7===0||m%5===0)&&m<36)?m:(p5*7-m*4);
  var ud=1;
  var penta=(p5/m>0.6)||(m<35&&m%5===0);
  var halves=a1%2===0;
  if(halves) a1=Math.floor(a1/2);
  var N=[];for(var i=0;i<m;i++)N.push({s:0,su:0,sl:0,sn:0,f:0,fu:0,fl:0,fn:0});
  function base(){
    var set=[[0,0],[p2,1],[m-p5,3],[p5,4],[p5+p2,5]];
    if(!penta){set.push([2*p2,2]);set.push([p5+2*p2,6]);}
    set.forEach(function(x){N[M(x[0],m)].sn=x[1];N[M(x[0],m)].fn=x[1];});
  }
  function tr(x){return(x-Math.floor(x)===0.5)?(x>0?Math.floor(x):Math.ceil(x)):Math.round(x)}
  function sc(x){var ra=tr(x/a1),ru=tr(((ra*a1)-x)/ud),ldc=(ru*ud)-((ra*a1)-x);return{ra:ra,ru:-ru,ldc:ldc}}
  function fc(x){var ra=tr(x/a1),ru=tr(((ra*a1)-x)/ud),ldc=((ra*a1)-x)-(ru*ud);return{ra:ra,ru:ru,ldc:ldc}}
  function ss(o,nom,c){o.sn=nom;o.s=c.ra;o.su=c.ru;o.sl=c.ldc}
  function sf(o,nom,c){o.fn=nom;o.f=c.ra;o.fu=c.ru;o.fl=c.ldc}
  base();
  var i,c;
  for(i=1;i<p2;i++){c=sc(i);ss(N[M(i,m)],0,c);ss(N[M(m-p5+i,m)],3,c);ss(N[M(p5+i,m)],4,c);}
  if(penta){
    for(i=1;i<(m-p5)-p2;i++){c=sc(i);ss(N[M(p2+i,m)],1,c);ss(N[M(p5+p2+i,m)],5,c);}
  }else{
    for(i=1;i<p2;i++){c=sc(i);ss(N[M(p2+i,m)],1,c);ss(N[M(p5+p2+i,m)],5,c);}
    for(i=1;i<(m-p5)-(p2*2);i++){c=sc(i);ss(N[M(p2*2+i,m)],2,c);ss(N[M(p5+p2*2+i,m)],6,c);}
  }
  for(i=1;i<p2;i++){c=fc(i);sf(N[M(p2-i,m)],1,c);sf(N[M(p5-i,m)],4,c);sf(N[M(p5+p2-i,m)],5,c);}
  if(penta){
    for(i=1;i<(m-p5)-p2;i++){c=fc(i);sf(N[M(m-i,m)],0,c);sf(N[M(m-p5-i,m)],3,c);}
  }else{
    for(i=1;i<(m-p5)-(p2*2);i++){c=fc(i);sf(N[M(m-i,m)],0,c);sf(N[M(m-p5-i,m)],3,c);}
    for(i=1;i<p2;i++){c=fc(i);sf(N[M(p2*2-i,m)],2,c);sf(N[M(p5+p2*2-i,m)],6,c);}
  }
  var LET=["F","C","G","D","A","E","B"],CC={0:1,1:3,2:5,3:0,4:2,5:4,6:6};
  function nom(x){return LET[CC[M(x,7)]]}
  function upd(u){if(u===-2)return"#";if(u===2)return"ww";return"v".repeat(Math.max(0,-u))+"w".repeat(Math.max(0,u))}
  function lft(l){return"\\".repeat(Math.max(0,-l))+"/".repeat(Math.max(0,l))}
  function sh(s,h){var r="";if(h){if(s%2!==0){if(s===3){r+="g";s-=3}else{r+="h";s-=1}}if(s>0){while(s>=4){r+="d";s-=4}if(s>=2){r+="c";s-=2}}}else{while(s>=2){r+="d";s-=2}if(s===1)r+="c"}return r}
  function fl(s,h){var r="";if(h){if(s%2!==0){if(s===3){r+="ea";s-=3}else{r+="e";s-=1}}if(s>0){while(s>=4){r+="aa";s-=4}if(s>=2){r+="a";s-=2}}}else{while(s>=2){r+="aa";s-=2}if(s===1)r+="a"}return r}
  return N.map(function(o){
    var sn=lft(o.sl)+upd(o.su)+nom(o.sn)+sh(o.s,halves);
    var fn=lft(o.fl)+upd(o.fu)+nom(o.fn)+fl(o.f,halves);
    var ts=Math.abs(o.s)+Math.abs(o.su)+Math.abs(o.sl);
    var tf=Math.abs(o.f)+Math.abs(o.fu)+Math.abs(o.fl);
    if(ts===0&&tf!==0)return sn;
    if(tf===0&&ts!==0)return fn;
    if(ts===0&&tf===0)return sn;
    if(ts<tf)return sn;
    if(tf<ts)return fn;
    return sn.length<=fn.length?sn:fn;
  });
}
function upsDowns(m){
  var r=null;
  try{ r=edoNames(m); }catch(e){ r=null; }
  if(r){ var seen={},k; for(k=0;k<r.length;k++){ if(seen[r[k]]){ r=null; break; } seen[r[k]]=1; } }
  return r;
}
for(var m=1;m<=72;m++){
  var r=upsDowns(m);
  WScript.Echo(m+"\t"+(r===null?"NULL":r.join(" ")));
}
