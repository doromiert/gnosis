/* SPDX-License-Identifier: GPL-3.0-or-later
 * Bounded, source-derived GTK previews for native desktop drags. */
#include <libxml/HTMLparser.h>

typedef struct { GtkWidget parent; } GnosisDragImage;
typedef struct { GtkWidgetClass parent; } GnosisDragImageClass;
G_DEFINE_TYPE(GnosisDragImage, gnosis_drag_image, GTK_TYPE_WIDGET)
static void gnosis_drag_image_measure(GtkWidget *widget, GtkOrientation orientation, int for_size,
                                     int *minimum, int *natural, int *min_baseline, int *nat_baseline) {
  (void)widget; (void)orientation; (void)for_size;
  *minimum = *natural = 78;
  *min_baseline = *nat_baseline = -1;
}
static void gnosis_drag_image_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  GdkTexture *texture = g_object_get_data(G_OBJECT(widget), "gnosis-drag-texture");
  if (!texture) return;
  float width = gtk_widget_get_width(widget), height = gtk_widget_get_height(widget);
  float image_width = gdk_texture_get_width(texture), image_height = gdk_texture_get_height(texture);
  float scale = MAX(width / image_width, height / image_height);
  graphene_rect_t clip = GRAPHENE_RECT_INIT(0, 0, width, height);
  graphene_rect_t image = GRAPHENE_RECT_INIT((width - image_width * scale) / 2,
    (height - image_height * scale) / 2, image_width * scale, image_height * scale);
  gtk_snapshot_push_clip(snapshot, &clip);
  gtk_snapshot_append_texture(snapshot, texture, &image);
  gtk_snapshot_pop(snapshot);
}
static void gnosis_drag_image_class_init(GnosisDragImageClass *klass) {
  GtkWidgetClass *widget = GTK_WIDGET_CLASS(klass);
  widget->measure = gnosis_drag_image_measure; widget->snapshot = gnosis_drag_image_snapshot;
}
static void gnosis_drag_image_init(GnosisDragImage *self) { (void)self; }

typedef struct {
  GtkWidget *image_card, *picture, *text_card, *label, *link_card;
  GtkWidget *host;
  GdkDrag *drag;
  GCancellable *cancel;
  char *text, *image_url;
  gboolean link, image_expected, image_requested, binary_pending;
} DragPreview;
typedef struct {
  GWeakRef box;
  GOutputStream *output;
  GCancellable *cancel;
  int kind; /* 0: text, 1: HTML, 2: image */
} PreviewRead;
static void preview_weak_free(gpointer data) {
  GWeakRef *weak = data; g_weak_ref_clear(weak); g_free(weak);
}
static void preview_free(gpointer data) {
  DragPreview *preview = data;
  g_cancellable_cancel(preview->cancel); g_object_unref(preview->cancel);
  g_free(preview->text); g_free(preview->image_url); g_free(preview);
}
static void preview_update(DragPreview *preview) {
  gboolean text = preview->text && *preview->text && (!preview->link || preview->image_expected);
  gtk_widget_set_visible(preview->image_card, preview->image_expected);
  gtk_widget_set_visible(preview->text_card, text || (!preview->link && !preview->image_expected));
  gtk_widget_set_visible(preview->link_card, preview->link && !preview->image_expected);
  gtk_label_set_text(GTK_LABEL(preview->label), text ? preview->text : "");
}
static void preview_request_image(DragPreview *preview) {
  if (!preview->image_requested && !preview->binary_pending && preview->image_url && *preview->image_url &&
      !g_cancellable_is_cancelled(preview->cancel)) {
    preview->image_requested = TRUE;
    g_signal_emit_by_name(preview->host, "drag-preview-image", preview->drag, preview->image_url);
  }
}
void jupiter_compositor_drag_set_image(GdkDrag *drag, GdkTexture *image) {
  GWeakRef *weak = g_object_get_data(G_OBJECT(drag), "gnosis-drag-preview");
  GtkWidget *box = weak ? g_weak_ref_get(weak) : NULL;
  if (!box) return;
  DragPreview *preview = g_object_get_data(G_OBJECT(box), "gnosis-drag-preview");
  if (image && !g_cancellable_is_cancelled(preview->cancel)) {
    preview->image_expected = TRUE;
    g_object_set_data_full(G_OBJECT(preview->picture), "gnosis-drag-texture", g_object_ref(image), g_object_unref);
    gtk_widget_queue_draw(preview->picture);
    preview_update(preview);
  }
  g_object_unref(box);
}
static char *preview_text(const char *bytes, gsize length) {
  g_autofree char *valid = g_utf8_make_valid(bytes, length);
  const char *end = g_utf8_offset_to_pointer(valid, MIN(g_utf8_strlen(valid, -1), 160));
  return g_strstrip(g_strndup(valid, end - valid));
}
static xmlNode *preview_image_node(xmlNode *node) {
  for (; node; node = node->next) {
    if (node->type == XML_ELEMENT_NODE && !xmlStrcasecmp(node->name, BAD_CAST "img")) return node;
    xmlNode *image = preview_image_node(node->children);
    if (image) return image;
  }
  return NULL;
}
static void preview_read_free(PreviewRead *read) {
  g_weak_ref_clear(&read->box); g_object_unref(read->output); g_object_unref(read->cancel); g_free(read);
}
static void preview_image_decoded(GObject *input, GAsyncResult *result, gpointer data) {
  (void)input; PreviewRead *read = data;
  g_autoptr(GError) error = NULL;
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream_finish(result, &error);
  GtkWidget *box = g_weak_ref_get(&read->box);
  if (box && !g_cancellable_is_cancelled(read->cancel)) {
    DragPreview *preview = g_object_get_data(G_OBJECT(box), "gnosis-drag-preview");
    preview->binary_pending = FALSE;
    if (pixbuf) {
      GdkTexture *texture = gdk_texture_new_for_pixbuf(pixbuf);
      jupiter_compositor_drag_set_image(preview->drag, texture); g_object_unref(texture);
    } else preview_request_image(preview);
  }
  g_clear_object(&pixbuf); g_clear_object(&box); preview_read_free(read);
}
static void preview_received(GObject *provider, GAsyncResult *result, gpointer data) {
  PreviewRead *read = data;
  g_autoptr(GError) error = NULL;
  gdk_content_provider_write_mime_type_finish(GDK_CONTENT_PROVIDER(provider), result, &error);
  GtkWidget *box = g_weak_ref_get(&read->box);
  if (!box || g_cancellable_is_cancelled(read->cancel)) {
    g_clear_object(&box); preview_read_free(read); return;
  }
  DragPreview *preview = g_object_get_data(G_OBJECT(box), "gnosis-drag-preview");
  GMemoryOutputStream *output = G_MEMORY_OUTPUT_STREAM(read->output);
  const char *bytes = g_memory_output_stream_get_data(output);
  gsize length = g_memory_output_stream_get_data_size(output);
  if (read->kind == 2) {
    if (length && !error) {
      GInputStream *input = g_memory_input_stream_new_from_data(g_memdup2(bytes, length), length, g_free);
      gdk_pixbuf_new_from_stream_at_scale_async(input, 320, 320, TRUE, read->cancel, preview_image_decoded, read);
      g_object_unref(input); g_object_unref(box); return;
    }
    preview->binary_pending = FALSE; preview_request_image(preview);
  } else if (length) {
    if (read->kind == 1) {
      htmlDocPtr doc = htmlReadMemory(bytes, length, NULL, "UTF-8",
        HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_COMPACT);
      if (doc) {
        xmlNode *root = xmlDocGetRootElement(doc), *image = preview_image_node(root);
        if (image) {
          xmlChar *url = xmlGetProp(image, BAD_CAST "src");
          if (url && xmlStrlen(url) <= 65536) {
            preview->image_expected = TRUE;
            g_free(preview->image_url); preview->image_url = g_strdup((const char *)url);
            xmlChar *text = xmlNodeGetContent(root);
            g_free(preview->text); preview->text = text ? preview_text((const char *)text, xmlStrlen(text)) : NULL;
            xmlFree(text); preview_request_image(preview);
          }
          xmlFree(url);
        }
        xmlFreeDoc(doc);
      }
    } else if (!preview->link && !preview->image_expected) {
      g_free(preview->text); preview->text = preview_text(bytes, length);
    }
    preview_update(preview);
  }
  g_object_unref(box); preview_read_free(read);
}
static void preview_read(GtkWidget *box, GdkContentProvider *provider, const char *mime, int kind, GCancellable *cancel) {
  PreviewRead *read = g_new0(PreviewRead, 1);
  g_weak_ref_init(&read->box, box); read->kind = kind; read->cancel = g_object_ref(cancel);
  gsize capacity = kind == 2 ? 8 * 1024 * 1024 : 64 * 1024;
  read->output = g_memory_output_stream_new(g_malloc(capacity), capacity, NULL, g_free);
  gdk_content_provider_write_mime_type_async(provider, mime, read->output, G_PRIORITY_DEFAULT,
    cancel, preview_received, read);
}
static GtkWidget *preview_card(const char *style, int size) {
  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request(card, size, size);
  gtk_widget_set_overflow(card, GTK_OVERFLOW_HIDDEN);
  gtk_widget_add_css_class(card, "gnosis-drag-card");
  gtk_widget_add_css_class(card, style);
  return card;
}
static void setup_drag_preview(DesktopDrag *drag, GdkContentProvider *provider) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  for (guint margin = 0; margin < 4; margin++) {
    if (margin == 0) gtk_widget_set_margin_start(box, 8);
    if (margin == 1) gtk_widget_set_margin_end(box, 8);
    if (margin == 2) gtk_widget_set_margin_top(box, 8);
    if (margin == 3) gtk_widget_set_margin_bottom(box, 8);
  }
  DragPreview *preview = g_new0(DragPreview, 1);
  preview->cancel = g_cancellable_new(); preview->host = GTK_WIDGET(drag->host); preview->drag = drag->outer;
  g_object_set_data_full(G_OBJECT(box), "gnosis-drag-preview", preview, preview_free);
  g_object_set_data_full(G_OBJECT(drag->outer), "gnosis-drag-preview-cancel", g_object_ref(preview->cancel), g_object_unref);
  GWeakRef *weak = g_new0(GWeakRef, 1); g_weak_ref_init(weak, box);
  g_object_set_data_full(G_OBJECT(drag->outer), "gnosis-drag-preview", weak, preview_weak_free);
  preview->image_card = preview_card("gnosis-drag-image", 80);
  preview->picture = g_object_new(gnosis_drag_image_get_type(), NULL);
  gtk_widget_set_hexpand(preview->picture, TRUE); gtk_widget_set_vexpand(preview->picture, TRUE);
  gtk_box_append(GTK_BOX(preview->image_card), preview->picture);
  preview->text_card = preview_card("gnosis-drag-text", 80);
  preview->label = gtk_label_new("");
  gtk_label_set_wrap(GTK_LABEL(preview->label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(preview->label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_ellipsize(GTK_LABEL(preview->label), PANGO_ELLIPSIZE_END);
  gtk_label_set_lines(GTK_LABEL(preview->label), 4);
  gtk_label_set_width_chars(GTK_LABEL(preview->label), 7); gtk_label_set_max_width_chars(GTK_LABEL(preview->label), 7);
  gtk_label_set_xalign(GTK_LABEL(preview->label), 0); gtk_label_set_yalign(GTK_LABEL(preview->label), 0);
  gtk_box_append(GTK_BOX(preview->text_card), preview->label);
  preview->link_card = preview_card("gnosis-drag-link", 48);
  GtkWidget *link = gtk_image_new_from_icon_name("insert-link-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(link), 24);
  gtk_widget_set_hexpand(link, TRUE); gtk_widget_set_vexpand(link, TRUE);
  gtk_box_append(GTK_BOX(preview->link_card), link);
  gtk_box_append(GTK_BOX(box), preview->image_card); gtk_box_append(GTK_BOX(box), preview->text_card);
  gtk_box_append(GTK_BOX(box), preview->link_card);
  GdkContentFormats *formats = gdk_content_provider_ref_formats(provider);
  preview->link = gdk_content_formats_contain_mime_type(formats, "text/x-moz-url") ||
    gdk_content_formats_contain_mime_type(formats, "text/uri-list");
  const char *image = NULL;
  gsize count; const char *const *mimes = gdk_content_formats_get_mime_types(formats, &count);
  for (gsize i = 0; i < count; i++) if (g_str_has_prefix(mimes[i], "image/") ||
      (g_str_has_prefix(mimes[i], "application/octet-stream;name=") &&
       g_regex_match_simple("\\.(png|jpe?g|gif|webp|avif|bmp|ico|svg)(\"|;|$)", mimes[i], G_REGEX_CASELESS, 0))) {
    image = mimes[i]; break;
  }
  if (image) { preview->image_expected = preview->binary_pending = TRUE; preview_read(box, provider, image, 2, preview->cancel); }
  if (gdk_content_formats_contain_mime_type(formats, "text/html")) preview_read(box, provider, "text/html", 1, preview->cancel);
  if (!preview->link && !preview->image_expected) {
    const char *plain = gdk_content_formats_contain_mime_type(formats, "text/plain;charset=utf-8") ? "text/plain;charset=utf-8" : "text/plain";
    if (gdk_content_formats_contain_mime_type(formats, plain)) preview_read(box, provider, plain, 0, preview->cancel);
  }
  preview_update(preview); gdk_content_formats_unref(formats);
  gtk_drag_icon_set_child(GTK_DRAG_ICON(gtk_drag_icon_get_for_drag(drag->outer)), box);
}
