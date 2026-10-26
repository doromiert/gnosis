/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native FindTabHelper bridge for the exact pinned Chromium binary. The
 * renderer's find machinery retains matching, frame traversal and highlights. */
typedef struct NativeFind NativeFind;
struct NativeFind {
  NativeFind *next;
  char *target;
  void *helper_identity; /* Never dereference without resolving target again. */
  gint64 id;
  int request_id;
  gint64 started;
};
typedef struct { void *agent, *helper; } NativeFindTarget;
static NativeFind *native_finds;
static guint native_find_watch;
static bool native_find_ready;
static void (*native_find_original_reply)(void *, int, int, const void *, int, bool);

static void native_find_release(NativeFindTarget *target) {
  if (!target->agent) return;
  unsigned *references = (unsigned *)((char *)target->agent + 8);
  if (!--*references) ((NativeVoid)pointer_at(pointer_at(target->agent, 0), 0xe8))(target->agent);
  target->agent = target->helper = NULL;
}
static NativeFindTarget native_find_resolve(const char *target) {
  NativeFindTarget result = {0};
  size_t length = strlen(target);
  unsigned char value[24] __attribute__((aligned(8))) = {0};
  /* GetForId borrows this const libc++ string; the private target ID is
   * supplied only by our already-attached browser process. */
  if (length <= 22) { memcpy(value, target, length); value[23] = length; }
  else { memcpy(value, &target, 8); memcpy(value + 8, &length, 8); value[23] = 0x80; }
  result.agent = ((void *(*)(const void *))(image + UI_AGENT_FOR_ID_RVA))(value);
  if (!result.agent) return result;
  void *contents = ((void *(*)(void *))pointer_at(pointer_at(result.agent, 0), 0x60))(result.agent);
  if (contents) result.helper = ((void *(*)(void *, void *))(image + UI_GET_USER_DATA_RVA))
    ((char *)contents + 8, (void *)(image + UI_FIND_KEY_RVA));
  return result;
}
static void native_find_result(const char *target, gint64 id, int count, int ordinal, const char *error) {
  g_autoptr(JsonObject) message = json_object_new();
  json_object_set_string_member(message, "kind", "find-result");
  json_object_set_int_member(message, "id", id);
  json_object_set_string_member(message, "target", target);
  json_object_set_int_member(message, "count", count);
  json_object_set_int_member(message, "ordinal", ordinal);
  json_object_set_string_member(message, "error", error);
  send_object(message);
}
static void native_find_free(NativeFind *request) { g_free(request->target); g_free(request); }
static void native_find_retire(const char *target, const char *error) {
  NativeFind **link = &native_finds;
  while (*link) {
    NativeFind *request = *link;
    if (strcmp(request->target, target)) { link = &request->next; continue; }
    *link = request->next;
    native_find_result(request->target, request->id, -1, 0, error);
    native_find_free(request);
  }
}
static bool native_find_complete(NativeFind *request, void *helper) {
  int id, count, ordinal;
  memcpy(&id, (char *)helper + 0x74, 4);
  memcpy(&count, (char *)helper + 0x78, 4);
  memcpy(&ordinal, (char *)helper + 0x8c, 4);
  if (id != request->request_id || !*((unsigned char *)helper + 0x90) ||
      *((unsigned char *)helper + 0x12) || count < 0 || ordinal < 0 || ordinal > count) return false;
  native_find_result(request->target, request->id, count, ordinal, "");
  return true;
}
/* A target can be closing while its DevToolsAgentHost still resolves. Such a
 * WebContents may never report a final find reply. Retire those requests rather
 * than accumulating stale helper identities for the lifetime of the browser. */
static gboolean native_find_expire(gpointer unused) {
  (void)unused;
  NativeFind **link = &native_finds;
  while (*link) {
    NativeFind *request = *link;
    NativeFindTarget owner = native_find_resolve(request->target);
    bool gone = !owner.helper || owner.helper != request->helper_identity;
    bool expired = g_get_monotonic_time() - request->started >= 4 * G_USEC_PER_SEC;
    native_find_release(&owner);
    if (!gone && !expired) { link = &request->next; continue; }
    *link = request->next;
    native_find_result(request->target, request->id, -1, 0,
      gone ? "The native search target closed" : "The native search target did not respond");
    native_find_free(request);
  }
  if (native_finds) return G_SOURCE_CONTINUE;
  native_find_watch = 0;
  return G_SOURCE_REMOVE;
}
static void native_find_reply(void *helper, int id, int count, const void *rect, int ordinal, bool final) {
  native_find_original_reply(helper, id, count, rect, ordinal, final);
  if (!final) return;
  NativeFind **link = &native_finds;
  while (*link) {
    NativeFind *request = *link;
    if (request->helper_identity != helper || request->request_id != id) { link = &request->next; continue; }
    /* Native observers ran above and may have closed a tab. Resolve its
     * current target and helper before reading the merged final result. */
    NativeFindTarget owner = native_find_resolve(request->target);
    bool done = owner.helper == helper && native_find_complete(request, owner.helper);
    native_find_release(&owner);
    if (!done) { link = &request->next; continue; }
    *link = request->next; native_find_free(request);
  }
}
static bool native_find_dispatch(JsonObject *message) {
  const char *kind = json_object_get_string_member_with_default(message, "kind", "");
  if (strcmp(kind, "find") && strcmp(kind, "find-stop")) return false;
  JsonNode *target_node = json_object_get_member(message, "target");
  JsonNode *id_node = json_object_get_member(message, "id");
  if (!target_node || json_node_get_value_type(target_node) != G_TYPE_STRING ||
      !id_node || json_node_get_value_type(id_node) != G_TYPE_INT64) return true;
  const char *target = json_node_get_string(target_node);
  gint64 id = json_node_get_int(id_node);
  if (!*target || strlen(target) > 128 || id <= 0) return true;
  native_find_retire(target, "Search was replaced or stopped");
  if (!native_find_ready) { native_find_result(target, id, -1, 0, "Native find is unavailable"); return true; }
  NativeFindTarget owner = native_find_resolve(target);
  if (!owner.helper) {
    native_find_result(target, id, -1, 0, "The native search target is unavailable");
    native_find_release(&owner); return true;
  }
  if (!strcmp(kind, "find-stop")) {
    ((void (*)(void *, int))(image + UI_FIND_STOP_RVA))(owner.helper, 1); /* kClear */
    native_find_release(&owner); return true;
  }
  JsonNode *text = json_object_get_member(message, "text");
  JsonNode *forward = json_object_get_member(message, "forward");
  JsonNode *sensitive = json_object_get_member(message, "caseSensitive");
  JsonNode *match = json_object_get_member(message, "findMatch");
  if (!text || json_node_get_value_type(text) != G_TYPE_STRING ||
      !forward || json_node_get_value_type(forward) != G_TYPE_BOOLEAN ||
      !sensitive || json_node_get_value_type(sensitive) != G_TYPE_BOOLEAN ||
      !match || json_node_get_value_type(match) != G_TYPE_BOOLEAN) {
    native_find_result(target, id, -1, 0, "Invalid native search request");
    native_find_release(&owner); return true;
  }
  const char *query = json_node_get_string(text);
  if (strlen(query) > 8192) {
    native_find_result(target, id, -1, 0, "Search text is too long");
    native_find_release(&owner); return true;
  }
  glong length = 0;
  g_autofree gunichar2 *utf16 = g_utf8_to_utf16(query, -1, NULL, &length, NULL);
  if (!utf16) {
    native_find_result(target, id, -1, 0, "Invalid search text");
    native_find_release(&owner); return true;
  }
  if (!length) {
    ((void (*)(void *, int))(image + UI_FIND_STOP_RVA))(owner.helper, 1);
    native_find_result(target, id, 0, 0, ""); native_find_release(&owner); return true;
  }
  size_t total = 0;
  for (NativeFind *request = native_finds; request; request = request->next) ++total;
  if (total >= 2048) {
    native_find_result(target, id, -1, 0, "Too many active searches");
    native_find_release(&owner); return true;
  }
  unsigned char value[24] __attribute__((aligned(8))) = {0};
  gunichar2 *buffer = ((gunichar2 *(*)(void *, size_t))(image + UI_FIND_STRING_RVA))(value, length);
  memcpy(buffer, utf16, (length + 1) * sizeof *buffer);
  int previous_id; memcpy(&previous_id, (char *)owner.helper + 0x14, 4);
  NativeFind *request = g_new0(NativeFind, 1);
  request->target = g_strdup(target); request->id = id; request->helper_identity = owner.helper;
  request->request_id = -1; request->started = g_get_monotonic_time();
  request->next = native_finds; native_finds = request;
  if (!native_find_watch) native_find_watch = g_timeout_add(250, native_find_expire, NULL);
  ((void (*)(void *, void *, bool, bool, bool, bool))(image + UI_FIND_START_RVA))
    (owner.helper, value, json_node_get_boolean(forward), json_node_get_boolean(sensitive), json_node_get_boolean(match), false);
  memcpy(&request->request_id, (char *)owner.helper + 0x14, 4);
  if (value[23] & 0x80) {
    size_t capacity; memcpy(&capacity, value + 16, 8); capacity &= G_MAXSIZE >> 1;
    ((void (*)(void *, size_t))(image + UI_SIZED_DELETE_RVA))(pointer_at(value, 0), capacity * sizeof(gunichar2));
  }
  /* StartFinding deliberately skips duplicate count-only requests. Return its
   * completed native result instead of leaving the shell waiting forever. */
  if (previous_id == request->request_id && native_find_complete(request, owner.helper)) {
    native_finds = request->next; native_find_free(request);
  }
  native_find_release(&owner);
  return true;
}
static void native_find_shutdown(void) {
  if (native_find_watch) { g_source_remove(native_find_watch); native_find_watch = 0; }
  while (native_finds) {
    NativeFind *request = native_finds; native_finds = request->next;
    NativeFindTarget owner = native_find_resolve(request->target);
    if (owner.helper) ((void (*)(void *, int))(image + UI_FIND_STOP_RVA))(owner.helper, 1);
    native_find_release(&owner); native_find_free(request);
  }
}
static bool native_find_install(void) {
  if (!matches(UI_FIND_START_RVA, UI_FIND_START_BYTES) || !matches(UI_FIND_STOP_RVA, UI_FIND_STOP_BYTES) ||
      !matches(UI_FIND_REPLY_RVA, UI_FIND_REPLY_BYTES) || !matches(UI_FIND_STRING_RVA, UI_FIND_STRING_BYTES)) return false;
  unsigned char *trampoline = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (trampoline == MAP_FAILED) return false;
  memcpy(trampoline, (void *)(image + UI_FIND_REPLY_RVA), 15);
  absolute_jump(trampoline + 15, (void *)(image + UI_FIND_REPLY_RVA + 15));
  if (mprotect(trampoline, 4096, PROT_READ | PROT_EXEC)) { munmap(trampoline, 4096); return false; }
  native_find_original_reply = (void (*)(void *, int, int, const void *, int, bool))trampoline;
  unsigned char jump[14]; absolute_jump(jump, native_find_reply);
  if (!replace_code(UI_FIND_REPLY_RVA, jump, sizeof jump)) { munmap(trampoline, 4096); return false; }
  native_find_ready = true;
  return true;
}
