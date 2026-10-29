/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "browser.hpp"
#include <adwaita.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
static std::shared_ptr<GnosisBrave::Page> page;
static std::string worker, role, root, report;
static bool busy;
static gint64 started;
static unsigned sequence;
static void fail(const char* message){g_printerr("PAIRING_NATIVE_FAIL %s\n",message);_exit(77);}
// These fixtures never contact the user's running KeePassXC/database.
extern "C" char* g_find_program_in_path(const char* name) {
 if(!strcmp(name,"keepassxc-proxy"))return nullptr;
 auto real=reinterpret_cast<char*(*)(const char*)>(dlsym(RTLD_NEXT,"g_find_program_in_path"));return real(name);
}
static gboolean tick(gpointer) {
 if(g_get_monotonic_time()-started>180*G_USEC_PER_SEC)fail("timeout");
 g_autofree char* phase=nullptr;g_file_get_contents((root+"/phase").c_str(),&phase,nullptr,nullptr);
 if((phase&&!strcmp(phase,"quit"))||g_file_test((root+"/stop-"+role).c_str(),G_FILE_TEST_EXISTS)){page.reset();g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);return G_SOURCE_REMOVE;}
 if(busy||page->session.empty())return G_SOURCE_CONTINUE;
 if(worker.empty()) {
  busy=true;
  GnosisBrave::browser_command(role=="private","Target.getTargets","{}",[](JsonObject* result,const std::string& error){
   busy=false;if(!result||!error.empty())return;
   auto* targets=json_object_get_array_member(result,"targetInfos");const char* id=nullptr;
   for(guint i=0;i<json_array_get_length(targets);i++) {
    auto* target=json_array_get_object_element(targets,i);
    if(!strcmp(json_object_get_string_member_with_default(target,"url",""),"chrome-extension://giohadoodmdcadnkcpdmhmmdnaidnckm/background/background_service.js"))
     id=json_object_get_string_member(target,"targetId");
   }
   if(role=="private"||role=="disabled") {
    if(id)fail("KeePassXC loaded in an excluded context");
    if(g_get_monotonic_time()-started>5*G_USEC_PER_SEC)
     g_file_set_contents((root+"/"+role+".json").c_str(),"{\"excluded\":true}",-1,nullptr);
    return;
   }
   if(!id)return;
   busy=true;GnosisBrave::browser_command(false,"Target.attachToTarget","{\"targetId\":"+GnosisBrave::quote(id)+",\"flatten\":false}",[](JsonObject* result,const std::string& error){
    busy=false;if(!result||!error.empty())return;worker=json_object_get_string_member(result,"sessionId");
   });
  });return G_SOURCE_CONTINUE;
 }
 const std::string script = "(async()=>{const role="+GnosisBrave::quote(role)+",phase="+GnosisBrave::quote(phase?phase:"")+",report="+GnosisBrave::quote(report)+";"+R"JS(
  if(typeof gnosisKeePassXCPairing!=='object'||!keepass.keyRing)return;
  const hash='a'.repeat(64), other='b'.repeat(64), key='A'.repeat(43)+'=';
  if((role==='main'||role==='custom')&&globalThis.gnosisPairingProbePhase!==phase){
   globalThis.gnosisPairingProbePhase=phase;
   const id=role==='custom'?'custom':phase==='rotate'?'updated':'connected';
   const ring=phase==='clear'?{}:{[hash]:{hash,id,key}};
   keepass.keyRing=ring;
   await browser.storage.local.set({keyRing:ring,gnosisFixtureMain:role});
  }else if((role==='app'||role==='late')&&!globalThis.gnosisPairingProbeSeeded){
   globalThis.gnosisPairingProbeSeeded=true;
   keepass.keyRing={...keepass.keyRing,[other]:{hash:other,id:role+'-only',key}};
   await browser.storage.local.set({keyRing:keepass.keyRing,gnosisFixtureApp:role+'-only'});
  }
  const data=await browser.storage.local.get(['keyRing','gnosisFixtureMain','gnosisFixtureApp']);
  const previous=keepass.databaseHash;keepass.databaseHash=hash;const [id]=keepass.getCryptoKey();keepass.databaseHash=previous;
  await fetch(report,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({role,phase,id,
   storedId:data.keyRing?.[hash]?.id||null,own:data.keyRing?.[other]?.id||null,mainSentinel:data.gnosisFixtureMain||null,appSentinel:data.gnosisFixtureApp||null})});
 })().catch(()=>fetch()JS"+GnosisBrave::quote(report)+R"JS(,{method:'POST',body:JSON.stringify({role:)JS"+
 GnosisBrave::quote(role)+R"JS(,error:true})})))JS";
 const std::string message="{\"id\":"+std::to_string(++sequence)+",\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":"+GnosisBrave::quote(script)+",\"awaitPromise\":true}}";
 busy=true;GnosisBrave::browser_command(false,"Target.sendMessageToTarget","{\"sessionId\":"+GnosisBrave::quote(worker)+",\"message\":"+GnosisBrave::quote(message)+"}",[](JsonObject*,const std::string& error){busy=false;if(!error.empty())fail("worker command failed");});
 return G_SOURCE_CONTINUE;
}
static gboolean start(gpointer) {
 auto* app=g_application_get_default();if(!app)return G_SOURCE_CONTINUE;
 auto* window=gtk_application_get_active_window(GTK_APPLICATION(app));if(!window)return G_SOURCE_CONTINUE;
 auto embed=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"));
 auto view=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"));
 auto getpage=reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));
 if(!embed(window))return G_SOURCE_CONTINUE;page=getpage(view(embed(window)));if(!page)return G_SOURCE_CONTINUE;
 role=g_getenv("GNOSIS_PAIRING_PROBE_ROLE");root=g_getenv("GNOSIS_PAIRING_PROBE_ROOT");report=g_getenv("GNOSIS_PAIRING_PROBE_REPORT");
 started=g_get_monotonic_time();g_timeout_add(500,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void setup(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(3,start,nullptr);}
