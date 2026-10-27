/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exact 155.0.8059.40 Views/model adapter. Authentication, device discovery,
 * credential choice and caBLE pairing remain entirely in Chromium. */
typedef struct AuthenticatorView AuthenticatorView;
struct AuthenticatorView { AuthenticatorView *next; void *view, *contents; };
static AuthenticatorView *authenticator_views;
static GHashTable *authenticator_owners;
static NativeVoid original_authenticator_destroy, original_authenticator_update;
static void (*original_authenticator_construct)(void *, void *, void *);

/* Bind the private CDP target to its actual WebContents before any request.
 * Never infer a credential request's owner from whichever tab has focus. */
static void authenticator_owner(JsonObject *message) {
  const char *target = json_object_get_string_member_with_default(message, "target", "");
  size_t length = strlen(target);
  if (!length || length > 128) return;
  if (!authenticator_owners) authenticator_owners = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
  if (!json_object_get_boolean_member_with_default(message, "enabled", FALSE)) {
    GHashTableIter iter; gpointer value;
    g_hash_table_iter_init(&iter, authenticator_owners);
    while (g_hash_table_iter_next(&iter, NULL, &value)) if (!strcmp(target, value)) g_hash_table_iter_remove(&iter);
    return;
  }
  unsigned char string[24] __attribute__((aligned(8))) = {0};
  if (length <= 22) { memcpy(string, target, length); string[23] = length; }
  else { memcpy(string, &target, 8); memcpy(string + 8, &length, 8); string[23] = 0x80; }
  void *agent = ((void *(*)(const void *))(image + UI_AGENT_FOR_ID_RVA))(string);
  if (!agent) return;
  void *contents = ((void *(*)(void *))pointer_at(pointer_at(agent, 0), 0x60))(agent);
  if (contents) g_hash_table_replace(authenticator_owners, contents, g_strdup(target));
  unsigned *references = (unsigned *)((char *)agent + 8);
  if (!--*references) ((NativeVoid)pointer_at(pointer_at(agent, 0), 0xe8))(agent);
}

static gboolean authenticator_supported(int step) {
  return (step >= 3 && step <= 13) || (step >= 15 && step <= 17) || step == 19 ||
    (step >= 20 && step <= 23) || (step >= 27 && step <= 30) || step == 32 || step == 33 || step == 34 || step == 35 || step == 36;
}
static int authenticator_step(void *model) {
  int value; memcpy(&value, (char *)model + 0x264, sizeof value); return value;
}
static char *authenticator_text(void *sheet, size_t slot) {
  return native_label((uintptr_t)pointer_at(pointer_at(sheet, 0), slot) - image, sheet);
}
static void authenticator_free_u16(unsigned char *value) {
  if (value[23] & 0x80) {
    size_t capacity; memcpy(&capacity, value + 16, sizeof capacity);
    ((void (*)(void *, size_t))(image + UI_SIZED_DELETE_RVA))(pointer_at(value, 0), capacity * 2);
  }
}
static char *authenticator_utf8(void *value) {
  const unsigned char *s = value;
  const char *text; size_t size;
  if (s[23] & 0x80) { text = pointer_at(value, 0); memcpy(&size, s + 8, sizeof size); }
  else { text = value; size = s[23]; }
  if (size > 8192 || (!text && size) || !g_utf8_validate(text, size, NULL)) return NULL;
  return g_strndup(text, size);
}
static void *authenticator_sheet(void *view) {
  void *sheet_view = pointer_at(view, 0x600);
  return sheet_view ? pointer_at(sheet_view, 0x310) : NULL;
}
static gboolean authenticator_account_sheet(void *sheet) {
  return pointer_at(sheet,0) == (void *)(image+UI_AUTHENTICATOR_ACCOUNT_SHEET_RVA) &&
    pointer_at(pointer_at(sheet,0),0x60) == (void *)(image+UI_AUTHENTICATOR_ACCOUNT_TITLE_RVA) &&
    matches(UI_AUTHENTICATOR_ACCOUNT_TITLE_RVA,UI_AUTHENTICATOR_ACCOUNT_TITLE_BYTES) &&
    matches(UI_AUTHENTICATOR_ACCOUNT_SELECT_RVA,UI_AUTHENTICATOR_ACCOUNT_SELECT_BYTES) &&
    matches(UI_AUTHENTICATOR_ACCOUNT_DESCRIPTION_RVA,UI_AUTHENTICATOR_ACCOUNT_DESCRIPTION_BYTES);
}
/* Mechanism::Hybrid is alternative 4 in this pinned binary's variant, whose
 * discriminator is at +0x38 (also read by its native mechanism metric helper).
 * Offer only transports the real request controller made available. */
static int authenticator_phone_mechanism(void *model) {
  size_t count; memcpy(&count, (char *)model + 0x70, sizeof count);
  unsigned char *mechanisms = pointer_at(model, 0x68);
  if (!mechanisms || count > 64 || !*((unsigned char *)model + 0x250)) return -1;
  for (size_t i = 0; i < count; ++i)
    if (mechanisms[i * 0x98 + 0x38] == 4) return (int)i;
  return -1;
}
static gboolean authenticator_accounts(void *sheet,void *model,JsonArray *options) {
  if (!authenticator_account_sheet(sheet)) return FALSE;
  int mode;memcpy(&mode,(char *)sheet+0x54,sizeof mode);
  size_t count,capacity;memcpy(&count,(char *)model+0xf8,sizeof count);memcpy(&capacity,(char *)model+0x100,sizeof capacity);
  void *credentials=pointer_at(model,0xf0);
  if ((mode!=0&&mode!=1)||!credentials||!count||count>64||capacity<count||capacity>1024) return FALSE;
  for(size_t i=0;i<count;i++) {
    unsigned char *credential=(unsigned char *)credentials+i*0xe8;
    unsigned char *name=credential+0x50;
    if(name[24]>1) return FALSE;
    g_autofree char *label=name[24]?authenticator_utf8(name):g_strdup("");
    if(!label)return FALSE;
    if(!*label) {
      unsigned char unnamed[24] __attribute__((aligned(8)))={0};
      ((void (*)(void *,int))(image+UI_RESOURCE_STRING_RVA))(unnamed,0x33fb);
      g_free(label);label=copy_u16(unnamed);authenticator_free_u16(unnamed);
      if(!label)return FALSE;
    }
    unsigned char description[24] __attribute__((aligned(8)))={0};
    ((void (*)(void *,void *,int))(image+UI_AUTHENTICATOR_ACCOUNT_DESCRIPTION_RVA))(description,credential,0);
    g_autofree char *detail=copy_u16(description);authenticator_free_u16(description);
    if(!detail)return FALSE;
    JsonObject *item=json_object_new();
    json_object_set_int_member(item,"index",i);
    json_object_set_string_member(item,"label",label);
    json_object_set_string_member(item,"description",detail);
    json_array_add_object_element(options,item);
  }
  return TRUE;
}
static Request *authenticator_request(void *view) {
  for (Request *r = requests; r; r = r->next) if (r->authenticator && r->view == view) return r;
  return NULL;
}
static gboolean publish_authenticator(void *view) {
  if (channel < 0 || pointer_at(view, 0) != (void *)(image + UI_AUTHENTICATOR_VTABLE_RVA)) return FALSE;
  void *model = pointer_at(view, 0x5f8), *sheet = authenticator_sheet(view);
  if (!model || !sheet) return FALSE;
  int step = authenticator_step(model);
  if (!authenticator_supported(step)) {
    Request *old = remove_request(0, view);
    if (old) { notify_closed(old->id); g_free(old); }
    return FALSE;
  }
  void *widget = ((void *(*)(void *))(image + UI_PERMISSION_GET_WIDGET_RVA))((char *)view + 0x2c8);
  if (!widget) return FALSE;
  g_autofree char *title = authenticator_text(sheet, 0x60);
  g_autofree char *description = authenticator_text(sheet, 0x68);
  g_autofree char *accept = authenticator_text(sheet, 0x30);
  g_autofree char *cancel = authenticator_text(sheet, 0x20);
  g_autofree char *other = authenticator_text(sheet, 0x58);
  g_autofree char *rp = authenticator_utf8((char *)model + 0x50);
  if (!title || !description || !accept || !cancel || !other || !rp) return FALSE;
  size_t count; memcpy(&count, (char *)model + 0x70, sizeof count);
  void *mechanisms = pointer_at(model, 0x68);
  if (count > 64 || (count && !mechanisms)) return FALSE;
  g_autoptr(JsonObject) message = json_object_new();
  json_object_set_string_member(message, "kind", "authenticator");
  const char *target = NULL;
  for (AuthenticatorView *entry = authenticator_views; entry; entry = entry->next)
    if (entry->view == view && authenticator_owners) target = g_hash_table_lookup(authenticator_owners, entry->contents);
  if (!target) return FALSE;
  json_object_set_string_member(message, "target", target);
  json_object_set_int_member(message, "step", step);
  json_object_set_string_member(message, "title", title);
  json_object_set_string_member(message, "description", description);
  json_object_set_string_member(message, "rp", rp);
  json_object_set_string_member(message, "accept", accept);
  json_object_set_string_member(message, "cancel", cancel);
  json_object_set_string_member(message, "other", other);
  void *table = pointer_at(sheet, 0);
  int accept_state = ((int (*)(void *))pointer_at(table, 0x28))(sheet);
  json_object_set_int_member(message, "accept_state", accept_state);
  json_object_set_boolean_member(message, "cancel_visible", ((bool (*)(void *))pointer_at(table, 0x18))(sheet));
  json_object_set_boolean_member(message, "other_visible", ((bool (*)(void *))pointer_at(table, 0x40))(sheet));
  json_object_set_boolean_member(message, "busy", ((bool (*)(void *))pointer_at(table, 0x10))(sheet));
  json_object_set_boolean_member(message, "enabled", !*((unsigned char *)model + 0x261));
  json_object_set_boolean_member(message, "security_key", step == 20 && *((unsigned char *)model + 0x20a));
  json_object_set_boolean_member(message, "create", *((unsigned char *)model + 0x44) == 0);
  JsonArray *notes = json_array_new();
  unsigned char descriptions[24] __attribute__((aligned(8))) = {0};
  ((void (*)(void *, void *))pointer_at(table, 0x70))(descriptions, sheet);
  size_t note_count, note_capacity;
  memcpy(&note_count, descriptions + 8, sizeof note_count);
  memcpy(&note_capacity, descriptions + 16, sizeof note_capacity);
  void *note_data = pointer_at(descriptions, 0);
  if (note_count > 64 || note_capacity < note_count || note_capacity > 1024) { json_array_unref(notes); return FALSE; }
  for (size_t i = 0; i < note_count; ++i) {
    unsigned char *value = (unsigned char *)note_data + i * 24;
    g_autofree char *note = copy_u16(value);
    if (note && *note) json_array_add_string_element(notes, note);
    authenticator_free_u16(value);
  }
  if (note_data) ((void (*)(void *, size_t))(image + UI_SIZED_DELETE_RVA))(note_data, note_capacity * 24);
  for (size_t slot = 0x78; slot <= 0x80; slot += 8) {
    g_autofree char *note = authenticator_text(sheet, slot);
    if (note && *note) json_array_add_string_element(notes, note);
  }
  if (step == 20) {
    unsigned char warning[32] __attribute__((aligned(8))) = {0};
    ((void (*)(void *, void *))(image + UI_AUTHENTICATOR_ATTESTATION_RVA))(warning, sheet);
    if (warning[24]) {
      g_autofree char *note = copy_u16(warning);
      if (note && *note) json_array_add_string_element(notes, note);
      authenticator_free_u16(warning);
    }
  }
  json_object_set_array_member(message, "notes", notes);
  JsonArray *options = json_array_new();
  if (step == 3 || step == 36) for (size_t i = 0; i < count; ++i) {
    size_t priority; memcpy(&priority, (char *)model + 0x80, sizeof priority);
    if (step == 36 && (!*((unsigned char *)model + 0x88) || i != priority)) continue;
    void *mechanism = (char *)mechanisms + i * 0x98;
    g_autofree char *label = copy_u16((char *)mechanism + 0x40);
    g_autofree char *detail = copy_u16((char *)mechanism + 0x70);
    if (!label || !detail) { json_array_unref(options); return FALSE; }
    JsonObject *item = json_object_new();
    json_object_set_int_member(item, "index", i);
    json_object_set_string_member(item, "label", label);
    json_object_set_string_member(item, "description", detail);
    json_array_add_object_element(options, item);
  }
  gboolean accounts=step==34||step==35;
  if(accounts&&!authenticator_accounts(sheet,model,options)){json_array_unref(options);return FALSE;}
  json_object_set_boolean_member(message,"accounts",accounts);
  json_object_set_boolean_member(message,"phone_available",accounts && authenticator_phone_mechanism(model)>=0);
  json_object_set_array_member(message, "options", options);
  if (step == 20) {
    if (!*((unsigned char *)model + 0x250)) return FALSE;
    g_autofree char *qr = authenticator_utf8((char *)model + 0x238);
    if (!qr || !g_str_has_prefix(qr, "FIDO:/")) return FALSE;
    /* Pairing material travels only over the inherited private socket. Never
     * print it, persist it or expose it to page scripts. */
    json_object_set_string_member(message, "qr", qr);
  }
  Request *r = authenticator_request(view);
  gboolean fresh = !r;
  if (fresh) { r = g_new0(Request, 1); r->id = ++sequence; r->view = view; r->widget = widget; r->authenticator = TRUE; }
  r->created = g_get_monotonic_time(); ++r->revision;
  json_object_set_int_member(message, "id", r->id);
  json_object_set_int_member(message, "revision", r->revision);
  if (!send_object(message)) { if (fresh) g_free(r); return FALSE; }
  if (fresh) { r->next = requests; requests = r; }
  return TRUE;
}
static void authenticator_construct(void *view, void *contents, void *model) {
  original_authenticator_construct(view, contents, model);
  if (!jupiter_adapter_enabled("passkeys")) return;
  AuthenticatorView *entry = g_new0(AuthenticatorView, 1);
  entry->view = view; entry->contents = contents; entry->next = authenticator_views; authenticator_views = entry;
}
static void authenticator_destroy(void *view) {
  AuthenticatorView **link = &authenticator_views;
  while (*link) { if ((*link)->view == view) { AuthenticatorView *old = *link; *link = old->next; g_free(old); break; } link = &(*link)->next; }
  Request *r = remove_request(0, view);
  if (r) { notify_closed(r->id); g_free(r); }
  original_authenticator_destroy(view);
}
static void authenticator_update(void *view) {
  gboolean was_bridged = authenticator_request(view) != NULL;
  original_authenticator_update(view);
  if (was_bridged && !publish_authenticator(view)) {
    Request *old = remove_request(0, view);
    if (old) { notify_closed(old->id); g_free(old); }
    int step = authenticator_step(pointer_at(view, 0x5f8));
    if (step >= 3 && step != 14 && !*((unsigned char *)view + 0x608)) {
      void *widget = ((void *(*)(void *))(image + UI_PERMISSION_GET_WIDGET_RVA))((char *)view + 0x2c8);
      original_widget_show(widget);
    }
  }
}
static gboolean show_authenticator_widget(void *widget) {
  for (AuthenticatorView *entry = authenticator_views; entry; entry = entry->next) {
    void *owner = ((void *(*)(void *))(image + UI_PERMISSION_GET_WIDGET_RVA))((char *)entry->view + 0x2c8);
    if (owner == widget) return !authenticator_sheet(entry->view) || publish_authenticator(entry->view);
  }
  return FALSE;
}
static void authenticator_show(void *view) {
  if (*((unsigned char *)view + 0x608)) return;
  if (!jupiter_adapter_enabled("passkeys") || !publish_authenticator(view)) {
    void *widget = ((void *(*)(void *))(image + UI_PERMISSION_GET_WIDGET_RVA))((char *)view + 0x2c8);
    original_widget_show(widget);
  }
}
static void decide_authenticator(Request *r, gboolean accept, const char *choice) {
  void *sheet = authenticator_sheet(r->view), *model = pointer_at(r->view, 0x5f8);
  if (!sheet || !model) { g_free(r); return; }
  if (!accept) {
    g_free(r);
    ((NativeVoid)pointer_at(pointer_at(sheet, 0), 0x98))(sheet);
    return;
  }
  /* Relink before calling Chromium: an action can synchronously update or
   * destroy the native view, and its observer must find the live request. */
  r->next = requests; requests = r;
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!choice || strlen(choice) > 512 || !json_parser_load_from_data(parser, choice, -1, NULL) ||
      !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) return;
  JsonObject *message = json_node_get_object(json_parser_get_root(parser));
  if ((guint64)json_object_get_int_member_with_default(message, "revision", 0) != r->revision) return;
  const char *action = json_object_get_string_member_with_default(message, "action", "");
  void *table = pointer_at(sheet, 0);
  if (!strcmp(action, "phone") && !*((unsigned char *)model + 0x261) &&
      (authenticator_step(model)==34||authenticator_step(model)==35) &&
      authenticator_account_sheet(sheet) && authenticator_phone_mechanism(model)>=0) {
    void *view = r->view;
    /* OnBack -> model.StartOver -> the native request delegate reconstructs
     * discovery with the original RP, challenge and transport restrictions.
     * Do not simply display the old QR after an authenticator has completed. */
    ((NativeVoid)pointer_at(table, 0x88))(sheet);
    r = authenticator_request(view);
    if (!r) return;
    model = pointer_at(view, 0x5f8);
    if (!model || authenticator_step(model)!=3 || *((unsigned char *)model+0x261)) return;
    int selected = authenticator_phone_mechanism(model);
    if (selected<0) return;
    void *mechanism = (char *)pointer_at(model,0x68)+selected*0x98;
    native_call(UI_AUTHENTICATOR_MECHANISM_METRIC_RVA,mechanism);
    native_call(UI_REPEATING_CLOSURE_RUN_RVA,(char *)mechanism+0x90);
  }
  else if (!strcmp(action, "back") && ((bool (*)(void *))pointer_at(table, 0x40))(sheet))
    ((NativeVoid)pointer_at(table, 0x88))(sheet);
  else if (!strcmp(action, "accept") && ((int (*)(void *))pointer_at(table, 0x28))(sheet) == 1)
    ((NativeVoid)pointer_at(table, 0x90))(sheet);
  else if (!strcmp(action,"account") && !*((unsigned char *)model+0x261) &&
      (authenticator_step(model)==34||authenticator_step(model)==35) && authenticator_account_sheet(sheet)) {
    gint64 selected=json_object_get_int_member_with_default(message,"index",-1);
    size_t count;memcpy(&count,(char *)model+0xf8,sizeof count);
    if(selected<0||(guint64)selected>=count||count>64)return;
    ((void (*)(void *,int))(image+UI_AUTHENTICATOR_ACCOUNT_SELECT_RVA))(sheet,(int)selected);
    ((NativeVoid)pointer_at(table,0x90))(sheet);
  }
  else if (!strcmp(action, "choose") && !*((unsigned char *)model + 0x261)) {
    const gint64 selected = json_object_get_int_member_with_default(message, "index", -1);
    size_t count; memcpy(&count, (char *)model + 0x70, sizeof count);
    int step = authenticator_step(model);
    if ((step != 3 && step != 36) || selected < 0 || (guint64)selected >= count || count > 64) return;
    size_t priority; memcpy(&priority, (char *)model + 0x80, sizeof priority);
    if (step == 36 && (!*((unsigned char *)model + 0x88) || (guint64)selected != priority)) return;
    void *mechanism = (char *)pointer_at(model, 0x68) + selected * 0x98;
    native_call(UI_AUTHENTICATOR_MECHANISM_METRIC_RVA, mechanism);
    native_call(UI_REPEATING_CLOSURE_RUN_RVA, (char *)mechanism + 0x90);
  }
}
