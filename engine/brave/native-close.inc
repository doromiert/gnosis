/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native beforeunload preflight for the GTK shell. Chromium still runs the
 * real frame handlers and supplies the real user decision. An armed preflight
 * suppresses only the subsequent unload, so cancelling a later tab in a window
 * never destroys earlier tabs. All ordinary native closes retain their delegate.
 */
typedef struct NativeCloseCheck NativeCloseCheck;
struct NativeCloseCheck {
  NativeCloseCheck *next;
  void *contents;
  char *target;
  gint64 id;
  gboolean cancelled;
};
static NativeCloseCheck *native_close_checks;
static void (*native_close_original_reply)(void *, bool, bool *);

static void *native_close_contents(const char *target) {
  size_t length = strlen(target);
  if (!length || length > 128) return NULL;
  unsigned char value[24] __attribute__((aligned(8))) = {0};
  if (length <= 22) { memcpy(value, target, length); value[23] = length; }
  else { memcpy(value, &target, sizeof target); memcpy(value + 8, &length, sizeof length); value[23] = 0x80; }
  void *agent = ((void *(*)(const void *))(image + UI_AGENT_FOR_ID_RVA))(value);
  if (!agent) return NULL;
  void *contents = ((void *(*)(void *))pointer_at(pointer_at(agent, 0), 0x60))(agent);
  unsigned *references = (unsigned *)((char *)agent + 8);
  if (!--*references) ((NativeVoid)pointer_at(pointer_at(agent, 0), 0xe8))(agent);
  return contents;
}
static void native_close_free(NativeCloseCheck *check) {
  g_free(check->target); g_free(check);
}
static void native_close_send(gint64 id, const char *target, gboolean proceed, const char *error) {
  g_autoptr(JsonObject) reply = json_object_new();
  json_object_set_string_member(reply, "kind", "beforeunload-result");
  json_object_set_int_member(reply, "id", id);
  json_object_set_string_member(reply, "target", target);
  json_object_set_boolean_member(reply, "proceed", proceed);
  json_object_set_string_member(reply, "error", error ? error : "");
  send_object(reply);
}
static void native_close_reply(void *contents, bool proceed, bool *fire_unload) {
  NativeCloseCheck **link = &native_close_checks;
  while (*link) {
    NativeCloseCheck *check = *link;
    if (check->contents != contents) { link = &check->next; continue; }
    *link = check->next;
    if (native_close_contents(check->target) != contents) {
      native_close_free(check);
      continue;
    }
    /* Retired requests still suppress a late ACK. Removing their guard at
     * cancellation could close a newly navigated page behind the user's back. */
    *fire_unload = false;
    if (!check->cancelled) native_close_send(check->id, check->target, proceed, "");
    native_close_free(check);
    return;
  }
  native_close_original_reply(contents, proceed, fire_unload);
}
static gboolean native_close_request(JsonObject *message) {
  const char *kind = json_object_get_string_member_with_default(message, "kind", "");
  if (strcmp(kind, "beforeunload-check") && strcmp(kind, "beforeunload-cancel")) return FALSE;
  const char *target = json_object_get_string_member_with_default(message, "target", "");
  gint64 id = json_object_get_int_member_with_default(message, "id", 0);
  if (id <= 0 || !*target || strlen(target) > 128) return TRUE;
  if (!strcmp(kind, "beforeunload-cancel")) {
    for (NativeCloseCheck *check = native_close_checks; check; check = check->next)
      if (check->id == id && !strcmp(check->target, target)) check->cancelled = TRUE;
    return TRUE;
  }
  void *contents = native_close_contents(target);
  if (!contents) { native_close_send(id, target, FALSE, "The page is no longer available"); return TRUE; }
  guint count = 0;
  NativeCloseCheck **link = &native_close_checks;
  while (*link) {
    NativeCloseCheck *check = *link;
    if (native_close_contents(check->target) != check->contents) {
      *link = check->next; native_close_free(check); continue;
    }
    if (check->contents == contents) {
      native_close_send(id, target, FALSE, "A previous close check is still finishing"); return TRUE;
    }
    count++; link = &check->next;
  }
  if (count >= 1024) { native_close_send(id, target, FALSE, "Too many active close checks"); return TRUE; }
  NativeCloseCheck *check = g_new0(NativeCloseCheck, 1);
  check->id = id; check->target = g_strdup(target); check->contents = contents;
  check->next = native_close_checks; native_close_checks = check;
  ((void (*)(void *, bool))(image + UI_CLOSE_DISPATCH_RVA))(contents, false);
  return TRUE;
}
static gboolean native_close_install(void) {
  if (!matches(UI_CLOSE_REPLY_RVA, UI_CLOSE_REPLY_BYTES) ||
      !matches(UI_CLOSE_DISPATCH_RVA, UI_CLOSE_DISPATCH_BYTES)) return FALSE;
  unsigned char *trampoline = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (trampoline == MAP_FAILED) return FALSE;
  /* The first fourteen bytes are complete, non-relative instructions. */
  memcpy(trampoline, (void *)(image + UI_CLOSE_REPLY_RVA), 14);
  absolute_jump(trampoline + 14, (void *)(image + UI_CLOSE_REPLY_RVA + 14));
  if (mprotect(trampoline, 4096, PROT_READ | PROT_EXEC)) { munmap(trampoline, 4096); return FALSE; }
  native_close_original_reply = (void (*)(void *, bool, bool *))trampoline;
  unsigned char jump[14]; absolute_jump(jump, native_close_reply);
  if (!replace_code(UI_CLOSE_REPLY_RVA, jump, sizeof jump)) { munmap(trampoline, 4096); return FALSE; }
  return TRUE;
}
