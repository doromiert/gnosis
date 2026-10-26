/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native command entry and fixed operations for the renderer's modal editor. */
void Page::set_vim_context(int context) {
  state_->vim_context = context;
  if (state_->surface && JUPITER_IS_COMPOSITOR_VIEW(state_->surface))
    jupiter_compositor_view_set_vim_field(JUPITER_COMPOSITOR_VIEW(state_->surface), context != 0);
}
bool Page::handle_vim_key(guint key, GdkModifierType modifiers) {
  const char* operation = jupiter_compositor_vim_key(key, modifiers);
  if (!operation || !vim_mode || is_closed() || !state_->vim_context) return false;
  auto* root = gtk_widget_get_root(widget());
  auto* focus = root ? gtk_root_get_focus(root) : nullptr;
  if (state_->vim_commands || !focus) return false;
  if (state_->surface ? focus != state_->surface : focus != widget() && !gtk_widget_is_ancestor(focus,widget())) return false;
  vim_key(operation);
  return true;
}
void Page::vim_key(const std::string& operation) {
  static const std::set<std::string> allowed = {"undo","redo","scroll-down","scroll-up","delete-word","backspace"};
  if (!vim_mode || is_closed() || !allowed.contains(operation)) return;
  auto unique = state_->metadata_unique.find(state_->vim_context);
  auto source = state_->menu_sessions.find(state_->vim_context);
  if (unique == state_->metadata_unique.end() || source == state_->menu_sessions.end()) return;
  state_->process->command("Runtime.evaluate", "{\"uniqueContextId\":" + quote(unique->second) +
    ",\"expression\":" + quote("globalThis.gnosisVim?.command(" + quote(operation) + ")") + "}", source->second);
}
void Page::dismiss_vim_commands() {
  auto* popover = std::exchange(state_->vim_commands, nullptr);
  state_->vim_command_entry = state_->vim_command_error = nullptr;
  if (!popover) return;
  gtk_popover_popdown(GTK_POPOVER(popover));
  gtk_widget_unparent(popover);
}
void Page::show_vim_commands() {
  if (!vim_mode || is_closed()) return;
  if (state_->vim_commands) { gtk_widget_grab_focus(state_->vim_command_entry); return; }
  auto* popover = gtk_popover_new();
  state_->vim_commands = popover;
  gtk_widget_set_parent(popover, widget());
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), false);
  gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_TOP);
  GdkRectangle point{24, std::max(0, gtk_widget_get_height(widget()) - 18), 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(popover), &point);
  auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  for (auto margin : {gtk_widget_set_margin_start,gtk_widget_set_margin_end,gtk_widget_set_margin_top,gtk_widget_set_margin_bottom}) margin(box,12);
  auto* title = gtk_label_new(_("Command"));
  gtk_widget_add_css_class(title,"heading");gtk_label_set_xalign(GTK_LABEL(title),0);
  gtk_box_append(GTK_BOX(box),title);
  auto* entry = state_->vim_command_entry = gtk_entry_new();
  gtk_widget_set_size_request(entry,std::clamp(gtk_widget_get_width(widget())-64,160,420),-1);
  gtk_entry_set_max_length(GTK_ENTRY(entry),8192);
  gtk_widget_add_css_class(entry,"monospace");
  gtk_editable_set_text(GTK_EDITABLE(entry),":");gtk_editable_set_position(GTK_EDITABLE(entry),-1);
  gtk_box_append(GTK_BOX(box),entry);
  auto* help = state_->vim_command_error = gtk_label_new(_("open · tabopen · reload · back · forward · write · quit · set · help"));
  gtk_label_set_wrap(GTK_LABEL(help),true);gtk_label_set_max_width_chars(GTK_LABEL(help),54);gtk_label_set_xalign(GTK_LABEL(help),0);
  gtk_widget_add_css_class(help,"dim-label");gtk_box_append(GTK_BOX(box),help);
  gtk_popover_set_child(GTK_POPOVER(popover),box);
  auto* weak = new std::weak_ptr<Page>(weak_from_this());
  g_signal_connect_data(entry,"activate",G_CALLBACK(+[](GtkEntry* entry,gpointer data){
    if(auto page=static_cast<std::weak_ptr<Page>*>(data)->lock())
      page->execute_vim_command(gtk_editable_get_text(GTK_EDITABLE(entry)));
  }),weak,+[](gpointer data,GClosure*){delete static_cast<std::weak_ptr<Page>*>(data);},G_CONNECT_DEFAULT);
  auto* keys=gtk_event_controller_key_new();
  g_signal_connect_data(keys,"key-pressed",G_CALLBACK(+[](GtkEventControllerKey*,guint key,guint,GdkModifierType,gpointer data)->gboolean{
    if(key!=GDK_KEY_Escape)return FALSE;
    if(auto page=static_cast<std::weak_ptr<Page>*>(data)->lock()){page->dismiss_vim_commands();page->focus();}
    return TRUE;
  }),new std::weak_ptr<Page>(weak_from_this()),+[](gpointer data,GClosure*){delete static_cast<std::weak_ptr<Page>*>(data);},G_CONNECT_DEFAULT);
  gtk_widget_add_controller(entry,keys);
  g_signal_connect_data(popover,"closed",G_CALLBACK(+[](GtkPopover*,gpointer data){
    if(auto page=static_cast<std::weak_ptr<Page>*>(data)->lock())page->dismiss_vim_commands();
  }),new std::weak_ptr<Page>(weak_from_this()),+[](gpointer data,GClosure*){delete static_cast<std::weak_ptr<Page>*>(data);},G_CONNECT_DEFAULT);
  gtk_popover_popup(GTK_POPOVER(popover));gtk_widget_grab_focus(entry);
}
void Page::execute_vim_command(const std::string& input) {
  if (!state_->vim_commands || is_closed()) return;
  std::string line = input;
  auto trim=[](std::string value){const auto start=value.find_first_not_of(" \t\r\n");if(start==std::string::npos)return std::string();return value.substr(start,value.find_last_not_of(" \t\r\n")-start+1);};
  line=trim(line);if(line.starts_with(':'))line=trim(line.substr(1));
  const auto space=line.find_first_of(" \t");
  const auto name=line.substr(0,space), argument=space==std::string::npos?std::string():trim(line.substr(space+1));
  auto error=[this](const char* message){gtk_label_set_text(GTK_LABEL(state_->vim_command_error),message);gtk_widget_add_css_class(state_->vim_command_error,"error");};
  auto run=[this](const char* action){dismiss_vim_commands();focus();gtk_widget_activate_action(widget(),action,nullptr);};
  if(name.empty()){dismiss_vim_commands();focus();return;}
  if(name=="help"||name=="h"||name=="commands"){
    gtk_label_set_text(GTK_LABEL(state_->vim_command_error),_("open URL · tabopen URL · tabnext · tabprev\nreload[!] · back · forward · write · quit · qall\nundo · redo · set vim · set novim\nEnter a field, i to type, Escape for normal mode; Escape again leaves the field."));return;
  }
  if(name=="open"||name=="o"||name=="e"||name=="tabopen"||name=="tabnew"||name=="tabe"){
    const bool tab=name=="tabopen"||name=="tabnew"||name=="tabe";
    if(argument.empty()){run(tab?"win.new-tab":"win.location");return;}
    std::string url=argument;
    if(!g_uri_peek_scheme(url.c_str()))url="https://"+url;
    if(!url.starts_with("https://")&&!url.starts_with("http://")&&url!="about:blank"){error(_("Use an HTTP or HTTPS address."));return;}
    dismiss_vim_commands();focus();
    gtk_widget_activate_action(widget(),tab?"popup.open-selection-in-new-tab":"popup.open-selection","s",url.c_str());return;
  }
  if(name=="undo"||name=="u"||name=="redo"){
    if(!state_->vim_context){error(_("Enter a text field first."));return;}
    const auto operation=name=="redo"?"redo":"undo";dismiss_vim_commands();focus();vim_key(operation);return;
  }
  if(name=="set"){
    if(argument!="vim"&&argument!="novim"){error(_("Use :set vim or :set novim."));return;}
    dismiss_vim_commands();focus();gtk_widget_activate_action(widget(),"win.set-vim-mode","b",argument=="vim");return;
  }
  if(name=="q"||name=="quit"||name=="close"||name=="bd"){run("tab.close");return;}
  if(name=="qa"||name=="qall"){dismiss_vim_commands();g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);return;}
  if(name=="w"||name=="write"){run("win.save-as");return;}
  if(name=="reload"||name=="r"){run("toolbar.reload");return;}
  if(name=="reload!"||name=="r!"){run("toolbar.reload-bypass-cache");return;}
  if(name=="back"){run("toolbar.navigation-back");return;}
  if(name=="forward"){run("toolbar.navigation-forward");return;}
  if(name=="tabnext"||name=="bn"){run("win.vim-tab-next");return;}
  if(name=="tabprev"||name=="bp"){run("win.vim-tab-previous");return;}
  error(_("Unknown command. Use :help to see available commands."));
}
