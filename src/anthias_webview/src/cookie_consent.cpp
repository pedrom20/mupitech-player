#include "cookie_consent.h"

namespace cookieConsent
{

QString dismissScript()
{
    // A C++ raw string literal, not the manual single-quote-JS
    // concatenation rotation.cpp uses — that convention reads fine for
    // a couple of short lines but would be unreadable at this length.
    // Single quotes are used for every JS string so this can still be
    // dropped into a QStringLiteral-concatenation call site unescaped
    // if ever needed.
    static const char kScript[] = R"JS(
(function(){
  function tryKnownApis(){
    try{if(window.OneTrust&&typeof window.OneTrust.AcceptAll==='function'){window.OneTrust.AcceptAll();return true;}}catch(e){}
    try{if(window.Didomi&&typeof window.Didomi.setUserAgreeToAll==='function'){window.Didomi.setUserAgreeToAll();return true;}}catch(e){}
    try{if(window.klaro&&typeof window.klaro.getManager==='function'){var m=window.klaro.getManager();if(m&&typeof m.acceptAll==='function'){m.acceptAll();return true;}}}catch(e){}
    try{if(window.CookieConsent&&typeof window.CookieConsent.accept==='function'){window.CookieConsent.accept();return true;}}catch(e){}
    try{if(window.cookieconsent&&typeof window.cookieconsent.acceptAll==='function'){window.cookieconsent.acceptAll();return true;}}catch(e){}
    return false;
  }

  var ACCEPT_WORDS=['accept all','accept cookies','i accept','accept','agree','i agree','allow all','allow cookies','got it','aceitar tudo','aceitar todos','aceitar cookies','aceitar','concordo','aceito','permitir todos','ok'];
  var BANNER_HINT=/cookie|consent|gdpr|rgpd|privacy|privacidade/i;

  function visible(el){
    if(!el)return false;
    var r=el.getBoundingClientRect();
    return r.width>0&&r.height>0&&el.offsetParent!==null;
  }

  function textMatches(el){
    var t=(el.innerText||el.textContent||el.value||'').trim().toLowerCase();
    if(!t||t.length>40)return false;
    for(var i=0;i<ACCEPT_WORDS.length;i++){
      if(t===ACCEPT_WORDS[i]||t.indexOf(ACCEPT_WORDS[i]+' ')===0){return true;}
    }
    return false;
  }

  function findBannerContainers(){
    var all=document.querySelectorAll('div,section,aside,footer,dialog,[role="dialog"],[role="alertdialog"]');
    var found=[];
    for(var i=0;i<all.length;i++){
      var el=all[i];
      var idcls=(el.id||'')+' '+(el.className&&el.className.toString?el.className.toString():'');
      if(BANNER_HINT.test(idcls)&&visible(el)){found.push(el);}
    }
    return found;
  }

  function clickAcceptIn(container){
    var candidates=container.querySelectorAll('button,a[role="button"],a,input[type="submit"],input[type="button"],[role="button"]');
    for(var i=0;i<candidates.length;i++){
      var el=candidates[i];
      if(visible(el)&&textMatches(el)){el.click();return true;}
    }
    return false;
  }

  function run(){
    if(tryKnownApis())return true;
    var containers=findBannerContainers();
    for(var i=0;i<containers.length;i++){
      if(clickAcceptIn(containers[i]))return true;
    }
    return false;
  }

  run();
  try{
    new MutationObserver(run).observe(document.documentElement,{childList:true,subtree:true});
  }catch(e){}
})();
)JS";
    return QString::fromUtf8(kScript);
}

}  // namespace cookieConsent
