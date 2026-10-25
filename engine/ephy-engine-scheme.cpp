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
#include <functional>
EphyEngineView* ephy_engine_view_for_browser(int);
namespace {
struct Body {
  std::mutex mutex;
  CefRefPtr<CefCallback> callback;
  std::function<void()> completion;
  std::vector<char> bytes;
  std::string mime="text/html";
  std::string error;
  size_t offset=0;
  bool cancelled=false, failed=false, completed=false;
  void finish(bool error) {
    CefRefPtr<CefCallback> next;
    std::function<void()> done;
    { std::lock_guard lock(mutex); if(cancelled||completed)return;failed=error;completed=true;next=callback;callback=nullptr;done=std::move(completion); }
    if(next)next->Continue();
    if(done)done();
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
  gboolean main_resource;
  std::shared_ptr<Body>* body;
};
G_DEFINE_TYPE(EphyEngineSchemeRequest,ephy_engine_scheme_request,G_TYPE_OBJECT)
static void ephy_engine_scheme_request_class_init(EphyEngineSchemeRequestClass* klass) {
  G_OBJECT_CLASS(klass)->finalize=+[](GObject* object) {
    auto* r=EPHY_ENGINE_SCHEME_REQUEST(object);
    // A handler must either finish the request or retain it for asynchronous
    // work. Abandoned requests fail, rather than leaving their task/view alive.
    if (r->body) (*r->body)->finish(true);
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
  Resource(std::string uri,int browser,std::string scheme,bool main_resource):uri_(std::move(uri)),scheme_(std::move(scheme)),browser_(browser),main_resource_(main_resource),body_(std::make_shared<Body>()){}
  bool Open(CefRefPtr<CefRequest>,bool& handle,CefRefPtr<CefCallback> callback) override {
    handle=false;
    { std::lock_guard lock(body_->mutex);body_->callback=callback; }
    struct Dispatch { std::string uri,scheme;int browser;bool main_resource;std::shared_ptr<Body> body; };
    g_main_context_invoke(nullptr,+[](gpointer p)->gboolean {
      std::unique_ptr<Dispatch> d(static_cast<Dispatch*>(p));
      Handler handler{};
      {std::lock_guard lock(handlers_mutex);auto it=handlers.find(d->scheme);if(it!=handlers.end())handler=it->second;}
      auto* view=ephy_engine_view_for_browser(d->browser);
      if(!view||!handler.callback){d->body->finish(true);return G_SOURCE_REMOVE;}
      auto* request=EPHY_ENGINE_SCHEME_REQUEST(g_object_new(EPHY_TYPE_ENGINE_SCHEME_REQUEST,nullptr));
      request->uri=g_strdup(d->uri.c_str());request->view=EPHY_ENGINE_VIEW(g_object_ref(view));request->main_resource=d->main_resource;
      auto* uri=g_uri_parse(d->uri.c_str(),G_URI_FLAGS_PARSE_RELAXED,nullptr);
      request->path=g_strdup(uri?g_uri_get_path(uri):"");if(uri)g_uri_unref(uri);
      request->body=new std::shared_ptr<Body>(std::move(d->body));
      handler.callback(request,handler.user);g_object_unref(request);
      return G_SOURCE_REMOVE;
    },new Dispatch{uri_,scheme_,browser_,main_resource_,body_});
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
  std::string uri_,scheme_;int browser_;bool main_resource_;std::shared_ptr<Body> body_;
  IMPLEMENT_REFCOUNTING(Resource);
};
class Factory final : public CefSchemeHandlerFactory {
public:
  CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser,CefRefPtr<CefFrame> frame,const CefString& scheme,CefRefPtr<CefRequest> request) override {
    if(!browser||!frame||request->GetMethod()!="GET")return nullptr;
    auto name=scheme.ToString();
    // Display isolation protects internal documents. Resources use Epiphany's
    // own additional path/origin checks in ephy_resource_request_cb.
    return new Resource(request->GetURL(),browser->GetIdentifier(),name,frame->IsMain() && request->GetResourceType()==RT_MAIN_FRAME);
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
void ephy_engine_register_uri_scheme(const char* scheme,EphyEngineSchemeRequestCallback callback,gpointer user,GDestroyNotify destroy) {
  if(!strcmp(scheme,"view-source"))return; // Chromium implements view-source itself.
  std::lock_guard lock(handlers_mutex);handlers[scheme]={callback,user,destroy};
}
gboolean ephy_engine_uri_scheme_request_is_main_resource(EphyEngineSchemeRequest* r){return r->main_resource;}
const char* ephy_engine_uri_scheme_request_get_uri(EphyEngineSchemeRequest* r){return r->uri;}
const char* ephy_engine_uri_scheme_request_get_path(EphyEngineSchemeRequest* r){return r->path;}
EphyEngineView* ephy_engine_uri_scheme_request_get_web_view(EphyEngineSchemeRequest* r){return r->view;}
void ephy_engine_uri_scheme_request_finish(EphyEngineSchemeRequest* r,GInputStream* stream,gint64,const char* mime) {
  std::string resolved_mime = mime ? mime : "";
  if (!mime) {
    // Internal resources must render even when no system MIME database is installed.
    if (auto* extension = strrchr(r->path, '.')) {
      if (!g_ascii_strcasecmp(extension, ".js")) resolved_mime = "text/javascript";
      else if (!g_ascii_strcasecmp(extension, ".css")) resolved_mime = "text/css";
      else if (!g_ascii_strcasecmp(extension, ".svg")) resolved_mime = "image/svg+xml";
    }
    if (resolved_mime.empty()) {
      g_autofree char* content_type = g_content_type_guess(r->path, nullptr, 0, nullptr);
      g_autofree char* inferred_mime = content_type ? g_content_type_get_mime_type(content_type) : nullptr;
      resolved_mime = inferred_mime ? inferred_mime : "application/octet-stream";
    }
  }
  auto body=*r->body;
  { std::lock_guard lock(body->mutex);body->mime=std::move(resolved_mime); }
  read_next(new Read{EPHY_ENGINE_SCHEME_REQUEST(g_object_ref(r)),G_INPUT_STREAM(g_object_ref(stream))});
}
void ephy_engine_uri_scheme_request_finish_error(EphyEngineSchemeRequest* r,GError* error) {
  if (error) { std::lock_guard lock((*r->body)->mutex); (*r->body)->error = error->message; }
  (*r->body)->finish(true);
}
namespace {
struct SchemeResult { GBytes* bytes; char* mime; };
void free_scheme_result(gpointer data) {
  auto* result = static_cast<SchemeResult*>(data);
  g_bytes_unref(result->bytes); g_free(result->mime); delete result;
}
}
void ephy_engine_uri_scheme_fetch_async(EphyEngineView* view, const char* uri, gboolean main,
                                      GCancellable* cancel, GAsyncReadyCallback callback, gpointer user) {
  auto* task = g_task_new(view, cancel, callback, user);
  if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return; }
  g_autofree char* scheme = g_uri_parse_scheme(uri);
  Handler handler{};
  { std::lock_guard lock(handlers_mutex); auto found = handlers.find(scheme ? scheme : ""); if (found != handlers.end()) handler = found->second; }
  if (!handler.callback) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "No internal handler for %s", scheme ? scheme : "URI");
    g_object_unref(task); return;
  }
  auto body = std::make_shared<Body>();
  g_task_set_task_data(task, new std::shared_ptr<Body>(body), +[](gpointer data) { delete static_cast<std::shared_ptr<Body>*>(data); });
  body->completion = [task] {
    auto body = *static_cast<std::shared_ptr<Body>*>(g_task_get_task_data(task));
    if (!g_task_return_error_if_cancelled(task)) {
      if (body->failed) g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", body->error.empty() ? "Internal resource request failed" : body->error.c_str());
      else g_task_return_pointer(task, new SchemeResult{g_bytes_new(body->bytes.data(), body->bytes.size()), g_strdup(body->mime.c_str())}, free_scheme_result);
    }
    g_object_unref(task);
  };
  auto* request = EPHY_ENGINE_SCHEME_REQUEST(g_object_new(EPHY_TYPE_ENGINE_SCHEME_REQUEST, nullptr));
  request->uri = g_strdup(uri); request->view = EPHY_ENGINE_VIEW(g_object_ref(view)); request->main_resource = main;
  auto* parsed = g_uri_parse(uri, G_URI_FLAGS_PARSE_RELAXED, nullptr);
  request->path = g_strdup(parsed ? g_uri_get_path(parsed) : ""); if (parsed) g_uri_unref(parsed);
  request->body = new std::shared_ptr<Body>(body);
  handler.callback(request, handler.user); g_object_unref(request);
}
GBytes* ephy_engine_uri_scheme_fetch_finish(GAsyncResult* result, char** mime, GError** error) {
  auto* value = static_cast<SchemeResult*>(g_task_propagate_pointer(G_TASK(result), error));
  if (!value) return nullptr;
  GBytes* bytes = g_bytes_ref(value->bytes);
  if (mime) *mime = g_strdup(value->mime);
  free_scheme_result(value); return bytes;
}
