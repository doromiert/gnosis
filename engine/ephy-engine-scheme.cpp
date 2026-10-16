/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-engine-scheme.h"
#include "include/cef_browser.h"
#include "include/cef_callback.h"
#include "include/cef_resource_handler.h"
#include "include/cef_request.h"
#include "include/cef_response.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
EphyEngineView* ephy_engine_view_for_browser(int);
namespace {
struct Body {
  std::mutex mutex;
  CefRefPtr<CefCallback> callback;
  std::vector<char> bytes;
  std::string mime="text/html";
  size_t offset=0;
  bool cancelled=false, failed=false, completed=false;
  void finish(bool error) {
    CefRefPtr<CefCallback> next;
    { std::lock_guard lock(mutex); if(cancelled||completed)return;failed=error;completed=true;next=callback;callback=nullptr; }
    if(next)next->Continue();
  }
};
struct Handler { EphyEngineSchemeRequestCallback callback; gpointer user; GDestroyNotify destroy; };
std::map<std::string,Handler> handlers;
std::mutex handlers_mutex;
}
struct _EphyEngineSchemeRequest {
  GObject parent;
  char *uri, *path;
  EphyEngineView* view;
  std::shared_ptr<Body>* body;
};
G_DEFINE_TYPE(EphyEngineSchemeRequest,ephy_engine_scheme_request,G_TYPE_OBJECT)
static void ephy_engine_scheme_request_class_init(EphyEngineSchemeRequestClass* klass) {
  G_OBJECT_CLASS(klass)->finalize=+[](GObject* object) {
    auto* r=EPHY_ENGINE_SCHEME_REQUEST(object);
    g_clear_object(&r->view);
    g_free(r->uri);g_free(r->path);delete r->body;
    G_OBJECT_CLASS(ephy_engine_scheme_request_parent_class)->finalize(object);
  };
}
static void ephy_engine_scheme_request_init(EphyEngineSchemeRequest*) {}
namespace {
struct Read { EphyEngineSchemeRequest* request; GInputStream* stream; };
void read_next(Read* r) {
  g_input_stream_read_bytes_async(r->stream,65536,G_PRIORITY_DEFAULT,nullptr,
    +[](GObject* object,GAsyncResult* result,gpointer user) {
      auto* r=static_cast<Read*>(user);auto body=*r->request->body;
      GError* error=nullptr;GBytes* bytes=g_input_stream_read_bytes_finish(G_INPUT_STREAM(object),result,&error);
      bool more=false, failed=error!=nullptr;
      if(bytes) {
        gsize size=0;auto* data=static_cast<const char*>(g_bytes_get_data(bytes,&size));
        { std::lock_guard lock(body->mutex);
          if(!body->cancelled && size && body->bytes.size()+size<=64*1024*1024) {body->bytes.insert(body->bytes.end(),data,data+size);more=true;}
          else if(size)failed=true;
        }
        g_bytes_unref(bytes);
      }
      g_clear_error(&error);
      if(more)read_next(r);
      else {body->finish(failed);g_object_unref(r->stream);g_object_unref(r->request);delete r;}
    },r);
}
class Resource final : public CefResourceHandler {
public:
  Resource(std::string uri,int browser,std::string scheme):uri_(std::move(uri)),scheme_(std::move(scheme)),browser_(browser),body_(std::make_shared<Body>()){}
  bool Open(CefRefPtr<CefRequest>,bool& handle,CefRefPtr<CefCallback> callback) override {
    handle=false;
    { std::lock_guard lock(body_->mutex);body_->callback=callback; }
    struct Dispatch { std::string uri,scheme;int browser;std::shared_ptr<Body> body; };
    g_main_context_invoke(nullptr,+[](gpointer p)->gboolean {
      std::unique_ptr<Dispatch> d(static_cast<Dispatch*>(p));
      Handler handler{};
      {std::lock_guard lock(handlers_mutex);auto it=handlers.find(d->scheme);if(it!=handlers.end())handler=it->second;}
      auto* view=ephy_engine_view_for_browser(d->browser);
      if(!view||!handler.callback){d->body->finish(true);return G_SOURCE_REMOVE;}
      auto* request=EPHY_ENGINE_SCHEME_REQUEST(g_object_new(EPHY_TYPE_ENGINE_SCHEME_REQUEST,nullptr));
      request->uri=g_strdup(d->uri.c_str());request->view=EPHY_ENGINE_VIEW(g_object_ref(view));
      auto* uri=g_uri_parse(d->uri.c_str(),G_URI_FLAGS_PARSE_RELAXED,nullptr);
      request->path=g_strdup(uri?g_uri_get_path(uri):"");if(uri)g_uri_unref(uri);
      request->body=new std::shared_ptr<Body>(std::move(d->body));
      handler.callback(request,handler.user);g_object_unref(request);
      return G_SOURCE_REMOVE;
    },new Dispatch{uri_,scheme_,browser_,body_});
    return true;
  }
  void GetResponseHeaders(CefRefPtr<CefResponse> response,int64_t& length,CefString&) override {
    std::lock_guard lock(body_->mutex);
    if(body_->failed) {response->SetError(ERR_FAILED);length=0;return;}
    response->SetStatus(200);response->SetMimeType(body_->mime.substr(0,body_->mime.find(';')));response->SetCharset("utf-8");length=body_->bytes.size();
    CefResponse::HeaderMap headers;
    headers.insert({"Cache-Control","no-store"});headers.insert({"X-Content-Type-Options","nosniff"});response->SetHeaderMap(headers);
  }
  bool Read(void* output,int size,int& read,CefRefPtr<CefResourceReadCallback>) override {
    std::lock_guard lock(body_->mutex);read=0;
    if(body_->cancelled||body_->failed)return false;
    read=std::min(size,int(body_->bytes.size()-body_->offset));
    if(read){memcpy(output,body_->bytes.data()+body_->offset,read);body_->offset+=read;return true;}
    return false;
  }
  void Cancel() override {std::lock_guard lock(body_->mutex);body_->cancelled=true;body_->callback=nullptr;}
private:
  std::string uri_,scheme_;int browser_;std::shared_ptr<Body> body_;
  IMPLEMENT_REFCOUNTING(Resource);
};
class Factory final : public CefSchemeHandlerFactory {
public:
  CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser,CefRefPtr<CefFrame> frame,const CefString& scheme,CefRefPtr<CefRequest> request) override {
    if(!browser||!frame||request->GetMethod()!="GET")return nullptr;
    auto name=scheme.ToString();
    // Display isolation protects internal documents. Resources use Epiphany's
    // own additional path/origin checks in ephy_resource_request_cb.
    return new Resource(request->GetURL(),browser->GetIdentifier(),name);
  }
private:
  IMPLEMENT_REFCOUNTING(Factory);
};
}
void ephy_engine_register_schemes(CefRawPtr<CefSchemeRegistrar> registrar) {
  registrar->AddCustomScheme("ephy-about",CEF_SCHEME_OPTION_DISPLAY_ISOLATED|CEF_SCHEME_OPTION_SECURE);
  registrar->AddCustomScheme("ephy-reader",CEF_SCHEME_OPTION_DISPLAY_ISOLATED|CEF_SCHEME_OPTION_SECURE);
  registrar->AddCustomScheme("ephy-resource",CEF_SCHEME_OPTION_SECURE);
}
void ephy_engine_install_scheme_handlers(CefRefPtr<CefRequestContext> context) {
  for(auto* name:{"ephy-about","ephy-reader","ephy-resource"}) {
    if (context) context->RegisterSchemeHandlerFactory(name,"",new Factory);
    else CefRegisterSchemeHandlerFactory(name,"",new Factory);
  }
}
void ephy_engine_clear_scheme_handlers() {
  std::map<std::string,Handler> old;
  {std::lock_guard lock(handlers_mutex);old.swap(handlers);}
  for(auto& [name,handler]:old)if(handler.destroy)handler.destroy(handler.user);
}
void ephy_engine_context_register_uri_scheme(WebKitWebContext*,const char* scheme,EphyEngineSchemeRequestCallback callback,gpointer user,GDestroyNotify destroy) {
  if(!strcmp(scheme,"view-source"))return; // Chromium implements view-source itself.
  std::lock_guard lock(handlers_mutex);handlers[scheme]={callback,user,destroy};
}
const char* ephy_engine_uri_scheme_request_get_uri(EphyEngineSchemeRequest* r){return r->uri;}
const char* ephy_engine_uri_scheme_request_get_path(EphyEngineSchemeRequest* r){return r->path;}
EphyEngineView* ephy_engine_uri_scheme_request_get_web_view(EphyEngineSchemeRequest* r){return r->view;}
void ephy_engine_uri_scheme_request_finish(EphyEngineSchemeRequest* r,GInputStream* stream,gint64,const char* mime) {
  g_autofree char* content_type = mime ? nullptr : g_content_type_guess(r->path, nullptr, 0, nullptr);
  g_autofree char* inferred_mime = content_type ? g_content_type_get_mime_type(content_type) : nullptr;
  auto body=*r->body;
  { std::lock_guard lock(body->mutex);body->mime=mime?mime:inferred_mime?inferred_mime:"application/octet-stream"; }
  read_next(new Read{EPHY_ENGINE_SCHEME_REQUEST(g_object_ref(r)),G_INPUT_STREAM(g_object_ref(stream))});
}
void ephy_engine_uri_scheme_request_finish_error(EphyEngineSchemeRequest* r,GError*) {(*r->body)->finish(true);}
