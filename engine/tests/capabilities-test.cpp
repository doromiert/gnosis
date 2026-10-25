#include "diagnostic-process.hpp"
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-engine-view.h"
#include "script-result.hpp"
#include "browser.hpp"
#include "include/cef_api_hash.h"
#include <filesystem>
#include <functional>
#include <source_location>
#include <string>

struct DelayedHTMLProvider { GdkContentProvider parent; unsigned writes; };
struct DelayedHTMLProviderClass { GdkContentProviderClass parent; };
G_DEFINE_TYPE(DelayedHTMLProvider, delayed_html_provider, GDK_TYPE_CONTENT_PROVIDER)
static void delayed_html_provider_init(DelayedHTMLProvider* self) { self->writes = 0; }
static void delayed_html_provider_class_init(DelayedHTMLProviderClass* klass) {
  auto* provider = GDK_CONTENT_PROVIDER_CLASS(klass);
  provider->ref_formats = +[](GdkContentProvider*) {
    const char* mime = "text/html";
    return gdk_content_formats_new(&mime, 1);
  };
  provider->write_mime_type_async = +[](GdkContentProvider* provider, const char*, GOutputStream* stream,
      int, GCancellable* cancel, GAsyncReadyCallback callback, gpointer data) {
    // Wayland clipboard managers also fetch this provider; target the local cancellable read.
    if (cancel) ++reinterpret_cast<DelayedHTMLProvider*>(provider)->writes;
    auto* task = g_task_new(provider, cancel, callback, data);
    g_object_set_data_full(G_OBJECT(task), "stream", g_object_ref(stream), g_object_unref);
    g_timeout_add(100, +[](gpointer data) -> gboolean {
      auto* task = G_TASK(data);
      if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return G_SOURCE_REMOVE; }
      auto* stream = G_OUTPUT_STREAM(g_object_get_data(G_OBJECT(task), "stream"));
      static const char html[] = "<b>delayed HTML</b>";
      g_output_stream_write_all_async(stream, html, sizeof(html)-1, G_PRIORITY_DEFAULT,
        g_task_get_cancellable(task), +[](GObject* source, GAsyncResult* result, gpointer data) {
          auto* task = G_TASK(data);
          GError* error = nullptr;
          if (g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, nullptr, &error))
            g_task_return_boolean(task, true);
          else g_task_return_error(task, error);
          g_object_unref(task);
        }, task);
      return G_SOURCE_REMOVE;
    }, task);
  };
  provider->write_mime_type_finish = +[](GdkContentProvider*, GAsyncResult* result, GError** error) -> gboolean {
    return g_task_propagate_boolean(G_TASK(result), error);
  };
}

static void wait_for(const std::function<bool()>& predicate,
                     std::source_location caller = std::source_location::current()) {
  auto deadline = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;
  while (!predicate() && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(nullptr, false);
    g_usleep(1000);
  }
  if (!predicate()) g_error("Timed out at %s:%u", caller.file_name(), caller.line());
}
struct ScriptReply {
  bool ready = false;
  std::string text;
  GError *error = nullptr;
  static void done(GObject *source, GAsyncResult *result, gpointer user) {
      auto *reply = static_cast<ScriptReply*>(user);
      auto *value = ephy_engine_web_view_evaluate_javascript_finish(EPHY_ENGINE_VIEW(source), result, &reply->error);
      if (value) { char *text = g_strdup(script_result_text(value).c_str()); reply->text = text; g_free(text); json_object_unref(value); }
      reply->ready = true;
  }
};
static std::string evaluate(EphyEngineView *view, const char *script, const char* world = nullptr,
                            std::source_location caller = std::source_location::current()) {
  ScriptReply reply;
  ephy_engine_web_view_evaluate_javascript(view, script, -1, world, nullptr, nullptr, ScriptReply::done, &reply);
  wait_for([&]{ return reply.ready; }, caller);
  if (reply.error) g_error("Script failed in %s: %s; %s", world ? world : "main", reply.error->message, script);
  return reply.text;
}
static std::string clipboard_text(GdkClipboard *clipboard) {
  wait_for([&] {
    auto *formats = gdk_clipboard_get_formats(clipboard);
    return gdk_content_formats_contain_gtype(formats, G_TYPE_STRING) ||
      gdk_content_formats_contain_mime_type(formats, "text/plain") ||
      gdk_content_formats_contain_mime_type(formats, "text/plain;charset=utf-8");
  });
  struct Reply { bool ready = false; char *text = nullptr; GError *error = nullptr; } reply;
  gdk_clipboard_read_text_async(clipboard, nullptr, +[](GObject *source, GAsyncResult *result, gpointer user) {
    auto *reply = static_cast<Reply*>(user);
    reply->text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &reply->error);
    reply->ready = true;
  }, &reply);
  wait_for([&]{ return reply.ready; });
  g_assert_no_error(reply.error);
  std::string text = reply.text ? reply.text : ""; g_free(reply.text); return text;
}
static bool check_gpu(EphyEngineView* view) {
  auto result = evaluate(view, R"JS((async()=>{
    if (!navigator.gpu) return JSON.stringify({error:'navigator.gpu unavailable'});
    const adapter = await navigator.gpu.requestAdapter();
    if (!adapter) return JSON.stringify({error:'No WebGPU adapter'});
    const device = await adapter.requestDevice();
    const input = device.createBuffer({size:16, usage:GPUBufferUsage.STORAGE|GPUBufferUsage.COPY_DST|GPUBufferUsage.COPY_SRC});
    device.queue.writeBuffer(input,0,new Uint32Array([1,2,3,4]));
    const read = device.createBuffer({size:16,usage:GPUBufferUsage.MAP_READ|GPUBufferUsage.COPY_DST});
    const module = device.createShaderModule({code:'@group(0) @binding(0) var<storage,read_write> values: array<u32>; @compute @workgroup_size(1) fn main(@builtin(global_invocation_id) id:vec3<u32>) { values[id.x] *= 2u; }'});
    const pipeline = device.createComputePipeline({layout:'auto',compute:{module,entryPoint:'main'}});
    const group = device.createBindGroup({layout:pipeline.getBindGroupLayout(0),entries:[{binding:0,resource:{buffer:input}}]});
    const encoder=device.createCommandEncoder(), pass=encoder.beginComputePass();
    pass.setPipeline(pipeline);pass.setBindGroup(0,group);pass.dispatchWorkgroups(4);pass.end();
    encoder.copyBufferToBuffer(input,0,read,0,16);device.queue.submit([encoder.finish()]);
    await read.mapAsync(GPUMapMode.READ);const values=Array.from(new Uint32Array(read.getMappedRange()));read.unmap();
    const info=adapter.info;device.destroy();
    return JSON.stringify({values,adapter:{vendor:info.vendor,architecture:info.architecture,device:info.device,description:info.description}});
  })().catch(e=>JSON.stringify({error:e.message})))JS");
  g_print("WebGPU compute result: %s\n", result.c_str());
  return result.find("[2,4,6,8]") != std::string::npos;
}
int main(int argc, char **argv) {
  disable_diagnostic_core_dumps();
  if (!cef_api_hash(CEF_API_VERSION, 0)) return 1;
  int sub = ephy_engine_execute_process(argc, argv); if (sub >= 0) return sub;
  const bool clipboard_only = argc == 2 && !strcmp(argv[1], "--clipboard-only");
  const bool gpu_only = argc == 2 && !strcmp(argv[1], "--gpu-only");
  auto *app = adw_application_new("io.github.doromiert.Jupiter.CapabilitiesTest", G_APPLICATION_NON_UNIQUE);
  GError *error = nullptr;
  g_assert_true(g_application_register(G_APPLICATION(app), nullptr, &error));
  g_assert_no_error(error);
  char profile[] = "/tmp/jupiter-capabilities-test-XXXXXX";
  g_assert_nonnull(mkdtemp(profile));
  g_assert_true(ephy_engine_initialize(argc, argv, profile, true));
  disable_diagnostic_core_dumps();
  auto *window = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
  gtk_window_set_default_size(GTK_WINDOW(window), 900, 650);
  auto *view = EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW, nullptr));
  adw_application_window_set_content(window, GTK_WIDGET(view));
  bool loaded = false;
  auto handler = g_signal_connect(view, "load-changed", G_CALLBACK(+[](EphyEngineView*, WebKitLoadEvent event, bool *loaded) {
    if (event == WEBKIT_LOAD_FINISHED) *loaded = true;
  }), &loaded);
  ephy_engine_web_view_load_uri(view, g_getenv("JUPITER_TEST_URL"));
  gtk_window_present(GTK_WINDOW(window));
  wait_for([&]{ return loaded; });
  g_signal_handler_disconnect(view, handler);
  auto finish = [&](bool ok) {
    g_print("Closing native capability window\n");
    gtk_window_destroy(GTK_WINDOW(window));
    g_print("Native window destroyed; waiting for Chromium browser closure\n");
    wait_for([]{ return Browser::active_count == 0; });
    g_print("Chromium browsers closed; shutting down runtime\n");
    ephy_engine_shutdown(); g_object_unref(app);
    g_print("Native capability runtime shutdown completed\n");
    std::filesystem::remove_all(profile);
    return ok ? 0 : 1;
  };
  if (gpu_only) return finish(check_gpu(view));
  gtk_window_present(GTK_WINDOW(window));
  if (g_getenv("JUPITER_TEST_COPY"))
    g_print("Native test window focus: active=%s\n", gtk_window_is_active(GTK_WINDOW(window)) ? "yes" : "no");
  wait_for([&]{ return gtk_window_is_active(GTK_WINDOW(window)); });
  gtk_widget_grab_focus(GTK_WIDGET(view));
  g_print("Capability fixture loaded\n");

  auto *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(view));
  evaluate(view, "document.body.innerHTML='<p id=copy>native clipboard selection</p><input id=paste>';getSelection().selectAllChildren(document.querySelector('#copy'));'ready'");
  bool changed = false;
  auto clipboard_handler = g_signal_connect(clipboard, "changed", G_CALLBACK(+[](GdkClipboard*, bool *changed) { *changed = true; }), &changed);
  ephy_engine_web_view_execute_editing_command(view, "Copy");
  wait_for([&]{ return changed; });
  auto clipboard_value = clipboard_text(clipboard);
  g_assert_cmpstr(clipboard_value.c_str(), ==, "native clipboard selection");
  g_print("Native Copy command and GTK clipboard text: PASS\n");
  auto result = evaluate(view, "navigator.clipboard.writeText('async clipboard text').then(()=> 'written')");
  g_assert_cmpstr(result.c_str(), ==, "written");
  clipboard_value = clipboard_text(clipboard);
  g_assert_cmpstr(clipboard_value.c_str(), ==, "async clipboard text");
  g_print("navigator.clipboard.writeText reaches GTK clipboard: PASS\n");

  unsigned read_prompts = 0;
  auto permission_handler = g_signal_connect(view, "permission-request", G_CALLBACK(+[](EphyEngineView*, EphyEnginePermission* request, unsigned* prompts) -> gboolean {
    if (ephy_engine_permission_get_kind(request) != EPHY_ENGINE_PERMISSION_CLIPBOARD) return false;
    ++*prompts;
    g_assert_true(ephy_engine_permission_decide(request, EPHY_ENGINE_PERMISSION_ALLOW));
    return true;
  }), &read_prompts);
  result = evaluate(view, "navigator.clipboard.readText().catch(e=>e.name+': '+e.message)");
  g_assert_cmpstr(result.c_str(), ==, "async clipboard text");
  g_assert_cmpuint(read_prompts, ==, 1);
  g_print("The actual Chromium clipboard read prompt gates reading the exact GTK clipboard text: PASS\n");

  for (const char* world : {"clipboard-private-one", "clipboard-private-two"}) {
    g_assert_true(evaluate(view, "typeof navigator.clipboard.writeText", world) == "function");
    changed = false;
    ephy_engine_web_view_execute_editing_command(view, "Copy");
    wait_for([&] { return changed; });
    result = evaluate(view, "navigator.clipboard.writeText('isolated: ąß🪐').then(()=> 'written').catch(e=>e.name+': '+e.message)", world);
    g_assert_cmpstr(result.c_str(), ==, "written");
    clipboard_value = clipboard_text(clipboard);
    g_assert_cmpstr(clipboard_value.c_str(), ==, "isolated: ąß🪐");
    auto before_read = read_prompts;
    result = evaluate(view, "navigator.clipboard.readText()", world);
    g_assert_cmpstr(result.c_str(), ==, "isolated: ąß🪐");
    g_assert_cmpuint(read_prompts, ==, before_read + 1);
    result = evaluate(view, R"JS(navigator.clipboard.write([new ClipboardItem({
      'web application/x-jupiter-unhandled': new Blob(['rejected'],{type:'application/x-jupiter-unhandled'})
    })]).then(()=> 'unexpected').catch(e=>e.name+': '+e.message))JS", world);
    g_assert_cmpstr(result.c_str(), ==, "NotAllowedError: Could not write to the native clipboard.");
    g_assert_true(clipboard_text(clipboard) == "isolated: ąß🪐");
  }
  const char* rich_html = R"HTML(<b style="color:red" onclick="globalThis.clipboardScriptRan=true">GTK rich</b><script>globalThis.clipboardScriptRan=true</script><a href="javascript:alert(1)">link</a>)HTML";
  const char* rich_svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" onload="globalThis.clipboardScriptRan=true"><rect width="2" height="1" fill="red"/><script>globalThis.clipboardScriptRan=true</script><a href="javascript:alert(1)">link</a></svg>)SVG";
  const unsigned char pixels[] = {255,0,0,255,0,255,0,255};
  g_autoptr(GBytes) pixel_bytes = g_bytes_new_static(pixels, sizeof pixels);
  g_autoptr(GdkTexture) image = gdk_memory_texture_new(2, 1, GDK_MEMORY_R8G8B8A8, pixel_bytes, 8);
  g_autoptr(GBytes) original_png = gdk_texture_save_to_png_bytes(image);
  gsize png_size;
  const auto* original_data = static_cast<const char*>(g_bytes_get_data(original_png, &png_size));
  g_assert_cmpuint(png_size, >=, 12);
  // An ancillary chunk distinguishes byte preservation from decoding/re-encoding.
  static const char metadata[] = "tEXtJupiter\0metadata survived";
  const std::string chunk(metadata, sizeof metadata-1);
  uint32_t crc = 0xffffffffU;
  for (unsigned char byte : chunk) {
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
  }
  std::string encoded(original_data, png_size-12);
  uint32_t length = g_htonl(chunk.size()-4), checksum = g_htonl(~crc);
  encoded.append(reinterpret_cast<const char*>(&length), 4).append(chunk);
  encoded.append(reinterpret_cast<const char*>(&checksum), 4).append(original_data+png_size-12, 12);
  g_autoptr(GBytes) png = g_bytes_new(encoded.data(), encoded.size());
  const auto* png_data = static_cast<const guchar*>(g_bytes_get_data(png, &png_size));
  g_autofree char* png_base64 = g_base64_encode(png_data, png_size);
  const unsigned char custom_data[] = {0,255,42,0,128};
  g_autoptr(GBytes) custom = g_bytes_new_static(custom_data, sizeof custom_data);
  g_autoptr(GBytes) html = g_bytes_new_static(rich_html, strlen(rich_html));
  g_autoptr(GBytes) svg = g_bytes_new_static(rich_svg, strlen(rich_svg));
  GdkContentProvider* providers[] = {
    gdk_content_provider_new_typed(G_TYPE_STRING, "GTK rich: ąß🪐"),
    gdk_content_provider_new_for_bytes("text/html", html),
    gdk_content_provider_new_for_bytes("image/png", png),
    gdk_content_provider_new_for_bytes("image/svg+xml", svg),
    gdk_content_provider_new_for_bytes("application/x-figma", custom),
  };
  g_autoptr(GdkContentProvider) rich_provider = gdk_content_provider_new_union(providers, G_N_ELEMENTS(providers));
  g_assert_true(gdk_clipboard_set_content(clipboard, rich_provider));
  for (const char* world : {"", "clipboard-private-one"}) {
    const auto before = read_prompts;
    result = evaluate(view, R"JS((async()=>{
      const items=await navigator.clipboard.read();
      if(items.length!==1 || !(items[0] instanceof ClipboardItem)) throw Error('Expected one native ClipboardItem');
      const item=items[0];
      if([...item.types].sort().join('|')!==['image/png','image/svg+xml','text/html','text/plain','web application/x-figma'].sort().join('|')) throw Error('Incorrect rich types: '+item.types);
      if(await (await item.getType('text/plain')).text()!=='GTK rich: ąß🪐') throw Error('Unicode text lost');
      const html=await (await item.getType('text/html')).text();
      const document=new DOMParser().parseFromString(html,'text/html');
      if(document.querySelector('script,[onclick],[href^="javascript:"]') || !document.querySelector('b')?.getAttribute('style')?.includes('color:red')) throw Error('HTML was not safely sanitized with formatting preserved: '+html);
      const xml=await (await item.getType('image/svg+xml')).text();
      const svg=new DOMParser().parseFromString(xml,'image/svg+xml');
      if(svg.querySelector('parsererror,script,[onload],[href^="javascript:"]') || svg.documentElement.namespaceURI!=='http://www.w3.org/2000/svg' || !svg.querySelector('rect')) throw Error('SVG not sanitized as valid SVG: '+xml);
      const bitmap=await createImageBitmap(await item.getType('image/png'));
      const canvas=new OffscreenCanvas(2,1), context=canvas.getContext('2d');context.drawImage(bitmap,0,0);bitmap.close();
      if([...context.getImageData(0,0,2,1).data].join(',')!=='255,0,0,255,0,255,0,255') throw Error('PNG pixels changed');
      if([...new Uint8Array(await (await item.getType('web application/x-figma')).arrayBuffer())].join(',')!=='0,255,42,0,128') throw Error('Custom binary data changed');
      if(globalThis.clipboardScriptRan) throw Error('Clipboard script executed');
      return 'rich clipboard passed';
    })().catch(e=>e.name+': '+e.message))JS", *world ? world : nullptr);
    g_assert_cmpstr(result.c_str(), ==, "rich clipboard passed");
    g_assert_cmpuint(read_prompts, ==, before+1);
    result = evaluate(view, "navigator.clipboard.read({types:['image/png']}).then(async items=>btoa(String.fromCharCode(...new Uint8Array(await (await items[0].getType('image/png')).arrayBuffer()))))", *world ? world : nullptr);
    g_assert_cmpstr(result.c_str(), ==, png_base64);
    result = evaluate(view, "navigator.clipboard.read({types:['text/html'],unsanitized:['text/html']}).then(async items=>await (await items[0].getType('text/html')).text()).catch(e=>e.name+': '+e.message)", *world ? world : nullptr);
    g_assert_cmpstr(result.c_str(), ==, rich_html);
  }
  g_print("Real GTK rich reads preserve Unicode, PNG pixels and binary custom data, sanitize HTML/SVG using Chromium, and honor explicit unsanitized HTML in main/isolated worlds: PASS\n");
  result = evaluate(view, R"JS((async()=>{
    const original=Element.prototype.setHTML, originalConstructor=globalThis.Sanitizer, originalItem=globalThis.ClipboardItem;
    Element.prototype.setHTML=()=>{throw Error('website replaced setHTML')};
    globalThis.Sanitizer=()=>{throw Error('website replaced Sanitizer')};
    globalThis.ClipboardItem=()=>{throw Error('website replaced ClipboardItem')};
    try {
      const items=await navigator.clipboard.read({types:['text/html']});
      if(!(items[0] instanceof originalItem)) throw Error('Lost native ClipboardItem');
      const text=await (await items[0].getType('text/html')).text();
      return text.includes('<b style="color:red">GTK rich</b>') && !text.includes('<script') && !text.includes('onclick') ? 'safe' : text;
    } finally {Element.prototype.setHTML=original;globalThis.Sanitizer=originalConstructor;globalThis.ClipboardItem=originalItem;}
  })().catch(e=>e.name+': '+e.message))JS");
  g_assert_cmpstr(result.c_str(), ==, "safe");
  g_print("Replacing webpage sanitizer/constructor globals does not bypass the captured native rich-read helpers: PASS\n");

  const std::string resource_html = std::string("<img src=\"") + g_getenv("JUPITER_TEST_URL") +
    "clipboard-read-resource?image\" onerror=\"globalThis.clipboardScriptRan=true\"><style>@import url('" +
    g_getenv("JUPITER_TEST_URL") + "clipboard-read-resource?style')</style>";
  g_autoptr(GBytes) resource_bytes = g_bytes_new(resource_html.data(), resource_html.size());
  g_autoptr(GdkContentProvider) resource_provider = gdk_content_provider_new_for_bytes("text/html", resource_bytes);
  g_assert_true(gdk_clipboard_set_content(clipboard, resource_provider));
  result = evaluate(view, R"JS((async()=>{
    const items=await navigator.clipboard.read({types:['text/html']});
    const text=await (await items[0].getType('text/html')).text();
    await new Promise(resolve=>setTimeout(resolve,200));
    const reports=await (await fetch('/settings-reports')).json();
    return !text.includes('onerror') && !text.includes('<style') && !reports.some(r=>r.clipboardReadFetch) && !globalThis.clipboardScriptRan ? 'inert' : JSON.stringify({text,reports});
  })().catch(e=>e.name+': '+e.message))JS");
  g_assert_cmpstr(result.c_str(), ==, "inert");
  g_print("Reading/sanitizing clipboard HTML does not execute script or fetch embedded image/CSS resources: PASS\n");

  const auto policy_prompts = read_prompts;
  evaluate(view, R"JS(new Promise(resolve=>{
    const frame=document.createElement('iframe');frame.id='clipboard-policy';
    frame.allow="clipboard-read 'none'";frame.srcdoc='<p>Clipboard policy</p>';
    frame.onload=()=>{frame.contentWindow.focus();resolve(true)};document.body.append(frame);
  }))JS");
  result = evaluate(view, R"JS(document.querySelector('#clipboard-policy').contentWindow.navigator.clipboard.read().then(()=> 'unexpected').catch(e=>e.name+'|'+e.message.toLowerCase()))JS");
  g_assert_true(result.starts_with("NotAllowedError|") && result.find("permissions policy") != std::string::npos);
  g_assert_cmpuint(read_prompts, ==, policy_prompts);
  evaluate(view, "document.querySelector('#clipboard-policy').remove();window.focus();true");
  g_print("Chromium denies an iframe's disallowed clipboard-read permissions policy before any GTK prompt/data read: PASS\n");
  const auto before_invalid = read_prompts;
  result = evaluate(view, "navigator.clipboard.read({unsanitized:['image/png']}).then(()=> 'unexpected').catch(e=>e.name)");
  g_assert_cmpstr(result.c_str(), ==, "NotAllowedError");
  g_assert_cmpuint(read_prompts, ==, before_invalid);
  g_autoptr(GBytes) invalid_png = g_bytes_new_static("not a PNG", 9);
  GdkContentProvider* invalid_providers[] = {
    gdk_content_provider_new_typed(G_TYPE_STRING, "selected text"),
    gdk_content_provider_new_for_bytes("image/png", invalid_png),
  };
  g_autoptr(GdkContentProvider) invalid_provider = gdk_content_provider_new_union(invalid_providers, G_N_ELEMENTS(invalid_providers));
  g_assert_true(gdk_clipboard_set_content(clipboard, invalid_provider));
  result = evaluate(view, "navigator.clipboard.read({types:['text/plain']}).then(async items=>items[0].types.length===1 ? await (await items[0].getType('text/plain')).text() : 'unexpected formats').catch(e=>e.name+': '+e.message)");
  g_assert_cmpstr(result.c_str(), ==, "selected text");
  result = evaluate(view, R"JS((async()=>{
    const items=await navigator.clipboard.read({types:['image/png']});
    const blob=await items[0].getType('image/png');
    if(await blob.text()!=='not a PNG') return 'PNG bytes were reinterpreted';
    try {const bitmap=await createImageBitmap(blob);bitmap.close();return 'unexpected decode'}
    catch(error){return 'renderer rejected invalid PNG'}
  })().catch(e=>e.name+': '+e.message))JS");
  g_assert_cmpstr(result.c_str(), ==, "renderer rejected invalid PNG");
  for (const char* options : {"{types:[]}", "{types:null}"}) {
    result = evaluate(view, (std::string("navigator.clipboard.read(")+options+").then(items=>String(items.length)).catch(e=>e.name)").c_str());
    g_assert_cmpstr(result.c_str(), ==, "0");
  }
  g_print("PNG bytes pass through unchanged for renderer decoding; invalid unsanitized types are denied and selecting text/empty types avoids unwanted formats: PASS\n");
  gdk_clipboard_set_texture(clipboard, image);
  result = evaluate(view, "navigator.clipboard.read({types:['image/png']}).then(async items=>{const bitmap=await createImageBitmap(await items[0].getType('image/png'));const size=bitmap.width+'x'+bitmap.height;bitmap.close();return size}).catch(e=>e.name+': '+e.message)");
  g_assert_cmpstr(result.c_str(), ==, "2x1");
  g_print("A native GTK texture is exposed as a real decodable PNG ClipboardItem: PASS\n");

  g_autoptr(GdkContentProvider) delayed = GDK_CONTENT_PROVIDER(g_object_new(delayed_html_provider_get_type(), nullptr));
  auto* delayed_state = reinterpret_cast<DelayedHTMLProvider*>(delayed);
  g_assert_true(gdk_clipboard_set_content(clipboard, delayed));
  result = evaluate(view, "navigator.clipboard.read({types:['text/html']}).then(async items=>await (await items[0].getType('text/html')).text()).catch(e=>e.name+': '+e.message)");
  g_assert_cmpstr(result.c_str(), ==, "<b>delayed HTML</b>");
  g_assert_cmpuint(delayed_state->writes, ==, 1);
  delayed_state->writes = 0;
  ScriptReply changed_read;
  ephy_engine_web_view_evaluate_javascript(view,
    "navigator.clipboard.read({types:['text/html']}).then(()=> 'unexpected').catch(e=>e.name)", -1,
    nullptr, nullptr, nullptr, ScriptReply::done, &changed_read);
  wait_for([&] { return delayed_state->writes != 0; });
  gdk_clipboard_set_text(clipboard, "new clipboard owner");
  wait_for([&] { return changed_read.ready; });
  g_assert_no_error(changed_read.error);
  g_assert_cmpstr(changed_read.text.c_str(), ==, "DataError");
  g_print("Changing the actual GTK clipboard during a delayed rich read cancels the read without mixing owners' data: PASS\n");

  gdk_clipboard_set_text(clipboard, "GTK source: ąß🪐");
  result = evaluate(view, "navigator.clipboard.readText().catch(e=>e.name+': '+e.message)");
  g_assert_cmpstr(result.c_str(), ==, "GTK source: ąß🪐");
  gdk_clipboard_set_content(clipboard, nullptr);
  result = evaluate(view, "navigator.clipboard.readText().catch(e=>e.name+': '+e.message)");
  g_assert_cmpstr(result.c_str(), ==, "");
  result = evaluate(view, "navigator.clipboard.read().then(items=>String(items.length))");
  g_assert_cmpstr(result.c_str(), ==, "0");
  g_print("Clipboard reads use actual GTK-owned text and handle an empty clipboard: PASS\n");
  g_signal_handler_disconnect(view, permission_handler);
  unsigned denied_reads = 0;
  permission_handler = g_signal_connect(view, "permission-request", G_CALLBACK(+[](EphyEngineView*, EphyEnginePermission* request, unsigned* prompts) -> gboolean {
    if (ephy_engine_permission_get_kind(request) != EPHY_ENGINE_PERMISSION_CLIPBOARD) return false;
    ++*prompts;
    ephy_engine_permission_decide(request, EPHY_ENGINE_PERMISSION_DENY);
    return true;
  }), &denied_reads);
  gdk_clipboard_set_text(clipboard, "must not disclose");
  for (const char* world : {"", "clipboard-private-one"}) {
    result = evaluate(view, "navigator.clipboard.readText().then(()=> 'unexpected').catch(e=>e.name)", *world ? world : nullptr);
    g_assert_cmpstr(result.c_str(), ==, "NotAllowedError");
    result = evaluate(view, "navigator.clipboard.read().then(()=> 'unexpected').catch(e=>e.name)", *world ? world : nullptr);
    g_assert_cmpstr(result.c_str(), ==, "NotAllowedError");
  }
  g_assert_cmpuint(denied_reads, ==, 4);
  g_signal_handler_disconnect(view, permission_handler);
  g_print("Chromium permission denial prevents GTK clipboard disclosure in main and isolated worlds: PASS\n");
  g_print("Isolated worlds receive their own native clipboard success/denial replies and preserve Unicode without changing clipboard data on rejection: PASS\n");

  evaluate(view, "addEventListener('copy',e=>{e.clipboardData.setData('text/plain','page copy only');e.clipboardData.setData('text/html','<b>page copy</b>');e.preventDefault()},{once:true});document.querySelector('#paste').blur();getSelection().selectAllChildren(document.querySelector('#copy'));true");
  changed = false;
  ephy_engine_web_view_execute_editing_command(view, "Copy");
  wait_for([&] { return changed; });
  g_assert_true(clipboard_text(clipboard) == "page copy only");
  auto* formats = gdk_clipboard_get_formats(clipboard);
  g_assert_true(gdk_content_formats_contain_mime_type(formats, "text/html"));
  g_print("Creating isolated worlds does not overwrite the website's custom Copy data: PASS\n");
  evaluate(view, "addEventListener('copy',e=>{e.clipboardData.setData('text/plain','private custom copy');e.preventDefault()},{once:true});true", "clipboard-private-one");
  changed = false;
  ephy_engine_web_view_execute_editing_command(view, "Copy");
  wait_for([&] { return changed; });
  g_assert_true(clipboard_text(clipboard) == "private custom copy");
  g_print("An isolated world's custom Copy handler still writes its actual DataTransfer content: PASS\n");

  evaluate(view, "(()=>{const t=document.createElement('textarea');t.id='cut';t.value='cut once';document.body.append(t);t.focus();t.setSelectionRange(0,3);return true})()");
  changed = false;
  ephy_engine_web_view_execute_editing_command(view, "Cut");
  wait_for([&] { return changed; });
  g_assert_true(clipboard_text(clipboard) == "cut");
  wait_for([&] { return evaluate(view, "document.getElementById('cut').value") == " once"; });
  g_print("Native Cut forwards the selection and deletes it once with multiple worlds present: PASS\n");

  const char* worlds[] = {nullptr, "clipboard-race-one", "clipboard-race-two"};
  for (auto* world : worlds) g_assert_true(evaluate(view, "typeof navigator.clipboard.writeText", world) == "function");
  evaluate(view, "document.querySelector('#cut').blur();getSelection().selectAllChildren(document.querySelector('#copy'));true");
  changed = false;
  ephy_engine_web_view_execute_editing_command(view, "Copy");
  wait_for([&] { return changed; });
  ScriptReply concurrent[3];
  for (unsigned i = 0; i < 3; ++i)
    ephy_engine_web_view_evaluate_javascript(view,
      "navigator.clipboard.writeText('concurrent worlds').then(()=> 'written')", -1,
      worlds[i], nullptr, nullptr, ScriptReply::done, &concurrent[i]);
  wait_for([&] { return concurrent[0].ready && concurrent[1].ready && concurrent[2].ready; });
  for (const auto& reply : concurrent) {
    g_assert_no_error(reply.error);
    g_assert_true(reply.text == "written");
  }
  g_assert_true(clipboard_text(clipboard) == "concurrent worlds");
  g_print("Concurrent writes with overlapping local request IDs complete only in their originating worlds: PASS\n");

  auto* other_window = GTK_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
  gtk_window_present(other_window);
  wait_for([&] { return gtk_window_is_active(other_window); });
  result = evaluate(view, R"JS((()=>{
    const previous=Object.getOwnPropertyDescriptor(document,'hasFocus');
    Object.defineProperty(document,'hasFocus',{configurable:true,value:()=>true});
    return navigator.clipboard.writeText('background overwrite').then(()=> 'unexpected')
      .catch(e=>e.name+': '+e.message).finally(()=>{
        if(previous) Object.defineProperty(document,'hasFocus',previous); else delete document.hasFocus;
      });
  })())JS", "clipboard-private-one");
  g_assert_true(result == "NotAllowedError: Could not write to the native clipboard.");
  g_assert_true(clipboard_text(clipboard) == "concurrent worlds");
  gtk_window_destroy(other_window);
  gtk_window_present(GTK_WINDOW(window));
  wait_for([&] { return gtk_window_is_active(GTK_WINDOW(window)); });
  g_print("The native focus gate denies a background isolated world's write even when script overrides document.hasFocus(): PASS\n");

  gdk_clipboard_set_text(clipboard, "native paste text");
  evaluate(view, "document.querySelector('#paste').focus();document.querySelector('#paste').value='';'ready'");
  ephy_engine_web_view_execute_editing_command(view, "Paste");
  wait_for([&]{ return evaluate(view, "document.querySelector('#paste').value") == "native paste text"; });
  g_print("GTK clipboard Paste inserts into the actual Chromium input: PASS\n");

  const bool gpu_ok = clipboard_only || check_gpu(view);

  loaded = false;
  handler = g_signal_connect(view, "load-changed", G_CALLBACK(+[](EphyEngineView*, WebKitLoadEvent event, bool* loaded) {
    if (event == WEBKIT_LOAD_FINISHED) *loaded = true;
  }), &loaded);
  ephy_engine_web_view_reload(view);
  wait_for([&] { return loaded; });
  g_signal_handler_disconnect(view, handler);
  gtk_window_present(GTK_WINDOW(window));
  wait_for([&] { return gtk_window_is_active(GTK_WINDOW(window)); });
  evaluate(view, "document.body.innerHTML='<p id=copy>reload gesture</p>';getSelection().selectAllChildren(copy);true");
  g_assert_true(evaluate(view, "typeof navigator.clipboard.writeText", "clipboard-private-one") == "function");
  changed = false;
  ephy_engine_web_view_execute_editing_command(view, "Copy");
  wait_for([&] { return changed; });
  result = evaluate(view, "navigator.clipboard.writeText('fresh world after reload').then(()=> 'written').catch(e=>e.name+': '+e.message)", "clipboard-private-one");
  g_assert_cmpstr(result.c_str(), ==, "written");
  g_assert_true(clipboard_text(clipboard) == "fresh world after reload");
  unsigned reload_prompts = 0;
  permission_handler = g_signal_connect(view, "permission-request", G_CALLBACK(+[](EphyEngineView*, EphyEnginePermission* request, unsigned* prompts) -> gboolean {
    if (ephy_engine_permission_get_kind(request) != EPHY_ENGINE_PERMISSION_CLIPBOARD) return false;
    ++*prompts;
    ephy_engine_permission_decide(request, EPHY_ENGINE_PERMISSION_ALLOW);
    return true;
  }), &reload_prompts);
  result = evaluate(view, "navigator.clipboard.read({types:['text/plain']}).then(async items=>await (await items[0].getType('text/plain')).text()).catch(e=>e.name+': '+e.message)", "clipboard-private-one");
  g_assert_cmpstr(result.c_str(), ==, "fresh world after reload");
  g_assert_cmpuint(reload_prompts, ==, 1);
  g_signal_handler_disconnect(view, permission_handler);
  g_print("Reload releases old world bindings; a recreated isolated world receives its own fresh clipboard result: PASS\n");
  g_signal_handler_disconnect(clipboard, clipboard_handler);
  return finish(gpu_ok);
}
