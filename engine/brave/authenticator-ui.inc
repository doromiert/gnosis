/* SPDX-License-Identifier: GPL-3.0-or-later
 * GTK presentation for Chromium-owned WebAuthn requests. */
void Page::dismiss_passkey_provider() {
  state_->passkey_provider_id.clear();
  if (!state_->passkey_provider) return;
  auto* cancel = std::exchange(state_->passkey_provider, nullptr);
  g_cancellable_cancel(cancel);
  g_object_unref(cancel);
}
void Page::show_passkey_provider(JsonObject* request) {
  dismiss_passkey_provider();
  auto process = state_->process;
  const std::string id = text(request, "id");
  auto* root = gtk_widget_get_root(widget());
  if (!GTK_IS_WINDOW(root)) {
    process->command("Runtime.evaluate", "{\"expression\":" + quote("jupiterKeePassXC.resolvePasskey(" + quote(id) + ", 'cancel')") + "}", process->keepassxc_session);
    return;
  }
  auto* dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
    json_object_get_boolean_member_with_default(request, "create", false) ? _("Create a Passkey") : _("Use a Passkey"), nullptr));
  g_autoptr(GUri) address = g_uri_parse(text(request, "url"), G_URI_FLAGS_PARSE_RELAXED, nullptr);
  g_autofree char* body = g_strdup_printf(json_object_get_boolean_member_with_default(request, "create", false) ?
    _("Choose where to save a passkey for %s.") : _("Choose where to use your passkey for %s."),
    address && g_uri_get_host(address) ? g_uri_get_host(address) : "");
  adw_alert_dialog_set_body(dialog, body);
  adw_alert_dialog_add_responses(dialog, "cancel", _("Cancel"), "device", _("Phone or Security Key…"), "keepassxc", _("KeePassXC"), nullptr);
  adw_alert_dialog_set_default_response(dialog, "keepassxc");
  adw_alert_dialog_set_close_response(dialog, "cancel");
  adw_alert_dialog_set_response_appearance(dialog, "keepassxc", ADW_RESPONSE_SUGGESTED);
  state_->passkey_provider = g_cancellable_new();
  state_->passkey_provider_id = id;
  struct Choice { std::weak_ptr<Page> page; std::weak_ptr<Process> process; std::string id; GCancellable* cancel; guint timeout; };
  auto* choice = new Choice{weak_from_this(), process, id, G_CANCELLABLE(g_object_ref(state_->passkey_provider)), 0};
  choice->timeout = g_timeout_add_full(G_PRIORITY_DEFAULT,
    std::clamp<gint64>(json_object_get_int_member_with_default(request, "timeout", 60000), 15000, 120000),
    +[](gpointer data) -> gboolean { g_cancellable_cancel(G_CANCELLABLE(data)); return G_SOURCE_REMOVE; },
    g_object_ref(choice->cancel), g_object_unref);
  if (state_->surface) jupiter_compositor_view_release_buttons(JUPITER_COMPOSITOR_VIEW(state_->surface));
  adw_alert_dialog_choose(dialog, GTK_WIDGET(root), choice->cancel, +[](GObject* source, GAsyncResult* result, gpointer data) {
    std::unique_ptr<Choice> choice(static_cast<Choice*>(data));
    const char* response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
    // A cancelled timeout source may already have fired.
    if (g_main_context_find_source_by_id(nullptr, choice->timeout)) g_source_remove(choice->timeout);
    if (auto process = choice->process.lock(); process && !process->closing && !process->keepassxc_session.empty()) {
      const auto expression = "jupiterKeePassXC.resolvePasskey(" + quote(choice->id) + "," + quote(response ? response : "cancel") + ")";
      process->command("Runtime.evaluate", "{\"expression\":" + quote(expression) + "}", process->keepassxc_session);
    }
    if (auto page = choice->page.lock(); page && page->state_->passkey_provider == choice->cancel) {
      page->state_->passkey_provider_id.clear();
      g_clear_object(&page->state_->passkey_provider);
    }
    g_object_unref(choice->cancel);
  }, choice);
}
void Page::dismiss_authenticator(bool cancel) {
  if (!state_->authenticator) return;
  auto ui = std::move(state_->authenticator);
  if (ui->enable_timer) g_source_remove(ui->enable_timer);
  if (ui->closed) g_signal_handler_disconnect(ui->dialog, ui->closed);
  adw_dialog_force_close(ui->dialog);
  g_object_unref(ui->dialog);
  if (cancel && state_->process) state_->process->native_decide(ui->id, false);
}
void Page::authenticator_action(const std::string& action, int index, guint64 revision) {
  if (!state_->authenticator || state_->authenticator->revision != revision || is_closed()) return;
  if (action == "cancel") { dismiss_authenticator(true); return; }
  auto& ui = *state_->authenticator;
  for (auto* widget : ui.actions) gtk_widget_set_sensitive(widget, false);
  const auto choice = "{\"revision\":" + std::to_string(revision) + ",\"action\":" + quote(action) +
    ",\"index\":" + std::to_string(index) + "}";
  state_->process->native_decide(ui.id, true, choice);
}
void Page::show_authenticator(JsonObject* message) {
  const auto id = json_object_get_int_member(message, "id");
  const auto revision = json_object_get_int_member(message, "revision");
  auto* root = gtk_widget_get_root(widget());
  if (!root || !GTK_IS_WINDOW(root) || is_closed()) {
    if (state_->process) state_->process->native_decide(id, false);
    return;
  }
  bool fresh = !state_->authenticator || state_->authenticator->id != id;
  if (fresh) {
    dismiss_authenticator(true);
    state_->authenticator = std::make_unique<AuthenticatorUI>();
    auto& ui = *state_->authenticator;
    ui.id = id;
    ui.dialog = ADW_DIALOG(g_object_ref_sink(adw_dialog_new()));
    adw_dialog_set_title(ui.dialog, _("Passkeys & Security Keys"));
    adw_dialog_set_content_width(ui.dialog, 480);
    auto* toolbar = ui.toolbar = adw_toolbar_view_new();
    auto* header = ui.header = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    auto* scroll = ui.scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), true);
    ui.content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    for (auto margin : {gtk_widget_set_margin_start, gtk_widget_set_margin_end, gtk_widget_set_margin_top, gtk_widget_set_margin_bottom})
      margin(ui.content, 24);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), ui.content);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroll);
    ui.footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(ui.footer, GTK_ALIGN_END);
    gtk_widget_set_margin_start(ui.footer, 24);
    gtk_widget_set_margin_end(ui.footer, 24);
    gtk_widget_set_margin_bottom(ui.footer, 24);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar), ui.footer);
    adw_dialog_set_child(ui.dialog, toolbar);
    auto* weak = new std::weak_ptr<Page>(weak_from_this());
    ui.closed = g_signal_connect_data(ui.dialog, "closed", G_CALLBACK(+[](AdwDialog*, gpointer data) {
      if (auto page = static_cast<std::weak_ptr<Page>*>(data)->lock()) page->dismiss_authenticator(true);
    }), weak, +[](gpointer data, GClosure*) { delete static_cast<std::weak_ptr<Page>*>(data); }, G_CONNECT_DEFAULT);
  }
  auto& ui = *state_->authenticator;
  if (ui.revision >= static_cast<guint64>(revision)) return;
  ui.revision = revision;
  if (ui.enable_timer) { g_source_remove(ui.enable_timer); ui.enable_timer = 0; }
  ui.actions.clear();
  while (auto* child = gtk_widget_get_first_child(ui.content)) gtk_box_remove(GTK_BOX(ui.content), child);
  while (auto* child = gtk_widget_get_first_child(ui.footer)) gtk_box_remove(GTK_BOX(ui.footer), child);
  const bool enabled = json_object_get_boolean_member(message, "enabled");
  const bool cancel_visible = json_object_get_boolean_member(message, "cancel_visible");
  adw_dialog_set_can_close(ui.dialog, cancel_visible);
  auto label = [&ui](const char* text, const char* style = nullptr) {
    auto* widget = gtk_label_new(text);
    gtk_label_set_wrap(GTK_LABEL(widget), true);
    gtk_label_set_wrap_mode(GTK_LABEL(widget), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(widget), 48);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    if (style) gtk_widget_add_css_class(widget, style);
    gtk_box_append(GTK_BOX(ui.content), widget);
    return widget;
  };
  label(text(message, "title"), "title-2");
  if (*text(message, "description")) label(text(message, "description"));
  struct Action { std::weak_ptr<Page> page; std::string operation; int index; guint64 revision; };
  auto connect = [this, &ui, revision](GtkWidget* widget, const char* signal, const char* operation, int index, bool sensitive, bool protect = true) {
    auto* action = new Action{weak_from_this(), operation, index, static_cast<guint64>(revision)};
    g_signal_connect_data(widget, signal, G_CALLBACK(+[](GtkWidget*, gpointer data) {
      auto* action = static_cast<Action*>(data);
      if (auto page = action->page.lock()) page->authenticator_action(action->operation, action->index, action->revision);
    }), action, +[](gpointer data, GClosure*) { delete static_cast<Action*>(data); }, G_CONNECT_DEFAULT);
    g_object_set_data(G_OBJECT(widget), "native-sensitive", GINT_TO_POINTER(sensitive));
    gtk_widget_set_sensitive(widget, protect ? false : sensitive);
    if (protect) ui.actions.push_back(widget);
  };
  auto* options = json_object_get_array_member(message, "options");
  const bool accounts=json_object_get_boolean_member_with_default(message,"accounts",false);
  if (json_array_get_length(options)) {
    auto* list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");
    for (guint i = 0; i < json_array_get_length(options); ++i) {
      auto* item = json_array_get_object_element(options, i);
      auto* row = adw_action_row_new();
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), text(item, "label"));
      adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), false);
      adw_action_row_set_subtitle(ADW_ACTION_ROW(row), text(item, "description"));
      adw_action_row_set_title_lines(ADW_ACTION_ROW(row), 2);
      adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 3);
      const bool choose=!accounts||json_array_get_length(options)>1;
      gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), choose);
      adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name(accounts?"avatar-default-symbolic":"dialog-password-symbolic"));
      if(choose) {
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("go-next-symbolic"));
        connect(row, "activated", accounts?"account":"choose", json_object_get_int_member(item, "index"), enabled);
      }
      gtk_list_box_append(GTK_LIST_BOX(list), row);
    }
    gtk_box_append(GTK_BOX(ui.content), list);
  }
  if (json_object_get_boolean_member_with_default(message, "phone_available", false)) {
    auto* list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");
    auto* row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), _("Use a Phone or Tablet"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), _("Scan a QR code to use a passkey on another device"));
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), true);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("phone-symbolic"));
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("go-next-symbolic"));
    connect(row, "activated", "phone", -1, enabled);
    gtk_list_box_append(GTK_LIST_BOX(list), row);
    gtk_box_append(GTK_BOX(ui.content), list);
  }
  if (const char* source = text(message, "qr"); *source) {
    label(_("Use your phone or tablet"), "heading");
    label(json_object_get_boolean_member(message, "create") ?
      _("Scan this code with the device where you want to create your passkey.") :
      _("Scan this code with the device that holds your passkey."));
    QRcode* code = QRcode_encodeString8bit(source, 0, QR_ECLEVEL_L);
    if (!code) { dismiss_authenticator(true); return; }
    struct Matrix { int width; std::vector<unsigned char> cells; };
    auto* matrix = new Matrix{code->width, std::vector<unsigned char>(code->data, code->data + code->width * code->width)};
    QRcode_free(code);
    auto* area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(area), 280);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), 280);
    gtk_widget_set_halign(area, GTK_ALIGN_CENTER);
    gtk_accessible_update_property(GTK_ACCESSIBLE(area), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Passkey pairing QR code"), -1);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), +[](GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
      auto* matrix = static_cast<Matrix*>(data);
      const int scale = std::max(1, std::min(width, height) / (matrix->width + 8));
      const double x = (width - matrix->width * scale) / 2.0;
      const double y = (height - matrix->width * scale) / 2.0;
      cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
      cairo_set_source_rgb(cr, 1, 1, 1); cairo_paint(cr);
      cairo_set_source_rgb(cr, 0, 0, 0);
      for (int row = 0; row < matrix->width; ++row)
        for (int col = 0; col < matrix->width; ++col)
          if (matrix->cells[row * matrix->width + col] & 1)
            cairo_rectangle(cr, x + col * scale, y + row * scale, scale, scale);
      cairo_fill(cr);
    }, matrix, +[](gpointer data) { delete static_cast<Matrix*>(data); });
    gtk_box_append(GTK_BOX(ui.content), area);
    if (json_object_get_boolean_member(message, "security_key")) {
      gtk_box_append(GTK_BOX(ui.content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
      label(_("Use a security key"), "heading");
      label(_("Insert your security key and touch it to continue."));
    }
  }
  if (auto* node = json_object_get_member(message, "notes"); node && JSON_NODE_HOLDS_ARRAY(node)) {
    auto* notes = json_node_get_array(node);
    for (guint i = 0; i < json_array_get_length(notes); ++i)
      label(json_array_get_string_element(notes, i));
  }
  if (json_object_get_boolean_member(message, "busy")) {
    auto* spinner = gtk_spinner_new(); gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(ui.content), spinner);
  }
  auto* footer = ui.footer;
  if (json_object_get_boolean_member(message, "other_visible")) {
    auto* back = gtk_button_new_with_label(*text(message, "other") ? text(message, "other") : _("Back"));
    connect(back, "clicked", "back", -1, enabled);
    gtk_box_append(GTK_BOX(footer), back);
  }
  if (cancel_visible) {
    auto* cancel = gtk_button_new_with_label(*text(message, "cancel") ? text(message, "cancel") : _("Cancel"));
    connect(cancel, "clicked", "cancel", -1, true, false);
    gtk_box_append(GTK_BOX(footer), cancel);
  }
  const int accept_state = json_object_get_int_member(message, "accept_state");
  if (accept_state) {
    auto* accept = gtk_button_new_with_label(*text(message, "accept") ? text(message, "accept") : _("Continue"));
    gtk_widget_add_css_class(accept, "suggested-action");
    connect(accept, "clicked", "accept", -1, enabled && accept_state == 1);
    gtk_box_append(GTK_BOX(footer), accept);
  }
  gtk_widget_set_visible(footer, gtk_widget_get_first_child(footer) != nullptr);
  // Sheets change in place (for example, touching a key opens its account
  // list). Recompute the dialog size instead of retaining the previous sheet's
  // height. Keep actions outside the scroller, including on small windows.
  const int available = std::max(180, gtk_widget_get_height(GTK_WIDGET(root)) - 48);
  int footer_height = 0, header_height = 0, content_height = 0;
  gtk_widget_measure(footer, GTK_ORIENTATION_VERTICAL, 480, nullptr, &footer_height, nullptr, nullptr);
  gtk_widget_measure(ui.header, GTK_ORIENTATION_VERTICAL, 480, nullptr, &header_height, nullptr, nullptr);
  gtk_widget_measure(ui.content, GTK_ORIENTATION_VERTICAL, 480, nullptr, &content_height, nullptr, nullptr);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(ui.scroll), std::max(1, available - footer_height - header_height));
  adw_dialog_set_content_height(ui.dialog, std::min(available, content_height + header_height + footer_height));
  auto* weak = new std::weak_ptr<Page>(weak_from_this());
  ui.enable_timer = g_timeout_add_full(G_PRIORITY_DEFAULT, 550, +[](gpointer data) -> gboolean {
    if (auto page = static_cast<std::weak_ptr<Page>*>(data)->lock(); page && page->state_->authenticator) {
      auto& ui = *page->state_->authenticator; ui.enable_timer = 0;
      for (auto* widget : ui.actions) gtk_widget_set_sensitive(widget, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "native-sensitive")));
    }
    return G_SOURCE_REMOVE;
  }, weak, +[](gpointer data) { delete static_cast<std::weak_ptr<Page>*>(data); });
  if (fresh) adw_dialog_present(ui.dialog, GTK_WIDGET(root));
}
