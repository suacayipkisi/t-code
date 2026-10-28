/*
 * The preview's images (see state.h): local files, web addresses (fetched by curl in the background
 * and kept while the preview is open), SVG drawn by GTK itself (4.22 or newer), and mermaid diagrams
 * drawn by mermaid-cli (mmdc) when it is installed. What is not there yet shows its alt text (a diagram:
 * its code), and the preview is shown again when it arrives.
 */
#include "editor/preview/state.h"

#include "application/identity.h"
#include "window/palette.h"

#include <glib/gstdio.h>
#include <string.h>

#define PENDING GINT_TO_POINTER(1) /* p->remote's values besides paintables */
#define FAILED GINT_TO_POINTER(2)
#define MAX_BYTES "20000000" /* curl's limit for one image */
#define SVG_SCALE 2          /* an SVG is drawn at twice its size */

void preview_remote_free(gpointer data)
{
  if (data != PENDING && data != FAILED)
    g_object_unref(data);
}

static void ignore_svg_error(GtkWidget *svg G_GNUC_UNUSED, GError *error G_GNUC_UNUSED)
{
}

#if GTK_CHECK_VERSION(4, 22, 0)
/* A texture drawn SVG_SCALE times smaller than it is: an SVG's sharp texture at the SVG's size. */
#define TCODE_TYPE_SCALED (tcode_scaled_get_type())
G_DECLARE_FINAL_TYPE(TcodeScaled, tcode_scaled, TCODE, SCALED, GObject)

struct _TcodeScaled {
  GObject parent;
  GdkTexture *texture;
};

static void tcode_scaled_snapshot(GdkPaintable *paintable, GdkSnapshot *snapshot, double width, double height)
{
  gdk_paintable_snapshot(GDK_PAINTABLE(TCODE_SCALED(paintable)->texture), snapshot, width, height);
}

static int tcode_scaled_get_intrinsic_width(GdkPaintable *paintable)
{
  return gdk_texture_get_width(TCODE_SCALED(paintable)->texture) / SVG_SCALE;
}

static int tcode_scaled_get_intrinsic_height(GdkPaintable *paintable)
{
  return gdk_texture_get_height(TCODE_SCALED(paintable)->texture) / SVG_SCALE;
}

static GdkPaintableFlags tcode_scaled_get_flags(GdkPaintable *paintable G_GNUC_UNUSED)
{
  return GDK_PAINTABLE_STATIC_SIZE | GDK_PAINTABLE_STATIC_CONTENTS;
}

static void tcode_scaled_paintable_init(GdkPaintableInterface *iface)
{
  iface->snapshot = tcode_scaled_snapshot;
  iface->get_intrinsic_width = tcode_scaled_get_intrinsic_width;
  iface->get_intrinsic_height = tcode_scaled_get_intrinsic_height;
  iface->get_flags = tcode_scaled_get_flags;
}

G_DEFINE_FINAL_TYPE_WITH_CODE(TcodeScaled, tcode_scaled, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(GDK_TYPE_PAINTABLE, tcode_scaled_paintable_init))

static void tcode_scaled_finalize(GObject *object)
{
  g_object_unref(TCODE_SCALED(object)->texture);
  G_OBJECT_CLASS(tcode_scaled_parent_class)->finalize(object);
}

static void tcode_scaled_class_init(TcodeScaledClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = tcode_scaled_finalize;
}

static void tcode_scaled_init(TcodeScaled *self G_GNUC_UNUSED)
{
}

/* An SVG drawn once into a texture, SVG_SCALE times its size (sharp on HiDPI): cairo draws text that
 * GPU renderers can get wrong (badges scale 110 px text down to 11), and a texture costs nothing to
 * draw again. Its size in the text is its SVG's. NULL when it has no size. */
static GdkPaintable *rasterize(GdkPaintable *svg)
{
  int w = gdk_paintable_get_intrinsic_width(svg), h = gdk_paintable_get_intrinsic_height(svg);
  GtkSnapshot *snapshot;
  GskRenderNode *node;
  cairo_surface_t *surface;
  GdkTexture *texture = NULL;
  TcodeScaled *scaled;

  if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
    return NULL;
  snapshot = gtk_snapshot_new();
  gtk_snapshot_scale(snapshot, SVG_SCALE, SVG_SCALE);
  gdk_paintable_snapshot(svg, snapshot, w, h);
  node = gtk_snapshot_free_to_node(snapshot);
  surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w * SVG_SCALE, h * SVG_SCALE);
  if (node) {
    cairo_t *cr = cairo_create(surface);
    gsk_render_node_draw(node, cr);
    cairo_destroy(cr);
    gsk_render_node_unref(node);
  }
  cairo_surface_flush(surface);
  GBytes *pixels = g_bytes_new(cairo_image_surface_get_data(surface),
                               (gsize)cairo_image_surface_get_stride(surface) * h * SVG_SCALE);
  texture = gdk_memory_texture_new(w * SVG_SCALE, h * SVG_SCALE, GDK_MEMORY_DEFAULT, pixels,
                                   cairo_image_surface_get_stride(surface));
  g_bytes_unref(pixels);
  cairo_surface_destroy(surface);
  scaled = g_object_new(TCODE_TYPE_SCALED, NULL);
  scaled->texture = texture;
  return GDK_PAINTABLE(scaled);
}
#endif

/* An image file's bytes as a paintable (name: its file or address, for the extension), or NULL. */
static GdkPaintable *paintable_from_bytes(GBytes *bytes, const char *name)
{
  gsize size;
  const char *data = g_bytes_get_data(bytes, &size);
  char *lower = g_ascii_strdown(name ? name : "", -1);
  gboolean svg = strstr(lower, ".svg") || g_strstr_len(data, MIN(size, 1024), "<svg");
  GdkPaintable *paintable = NULL;

  g_free(lower);
  if (!size)
    return NULL;
#if GTK_CHECK_VERSION(4, 22, 0)
  if (svg) {
    GtkSvg *image = gtk_svg_new();
    if (g_signal_lookup("error", GTK_TYPE_SVG)) /* a web page's SVG may use what GtkSvg lacks: no warnings */
      g_signal_connect(image, "error", G_CALLBACK(ignore_svg_error), NULL);
    gtk_svg_set_features(image, GTK_SVG_SYSTEM_RESOURCES); /* no animations, nothing it would fetch */
    gtk_svg_load_from_bytes(image, bytes);
    paintable = rasterize(GDK_PAINTABLE(image));
    g_object_unref(image);
    return paintable;
  }
#else
  (void)svg;
#endif
  paintable = GDK_PAINTABLE(gdk_texture_new_from_bytes(bytes, NULL));
  return paintable;
}

static GdkPaintable *paintable_from_file(const char *path)
{
  char *data;
  gsize size;
  GdkPaintable *paintable = NULL;

  if (g_file_get_contents(path, &data, &size, NULL)) {
    GBytes *bytes = g_bytes_new_take(data, size);
    paintable = paintable_from_bytes(bytes, path);
    g_bytes_unref(bytes);
  }
  return paintable;
}

/* Background jobs -------------------------------------------------------------------------------- */

typedef struct {
  Preview *p;
  char *key;  /* p->remote's key */
  char *path; /* the file the job writes (a diagram), or NULL: its output is the image */
} Job;

static void job_done(GObject *source, GAsyncResult *result, gpointer data)
{
  Job *job = data;
  GBytes *out = NULL;
  GError *error = NULL;
  gboolean ok = g_subprocess_communicate_finish(G_SUBPROCESS(source), result, &out, NULL, &error);

  if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) { /* else the preview is gone */
    GdkPaintable *paintable = NULL;
    if (ok && g_subprocess_get_successful(G_SUBPROCESS(source)))
      paintable = job->path ? paintable_from_file(job->path) : out ? paintable_from_bytes(out, job->key) : NULL;
    g_hash_table_replace(job->p->remote, g_strdup(job->key), paintable ? (gpointer)paintable : FAILED);
    if (paintable)
      preview_reload_later(job->p);
  }
  g_clear_error(&error);
  if (out)
    g_bytes_unref(out);
  g_free(job->key);
  g_free(job->path);
  g_free(job);
}

/* Runs argv in the background for p->remote's key (path: the file it writes, or NULL: its output). */
static void start_job(Preview *p, const char *key, const char *path, const char *const *argv)
{
  GSubprocess *process =
      g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
  Job *job;

  if (!process) {
    g_hash_table_replace(p->remote, g_strdup(key), FAILED);
    return;
  }
  job = g_new0(Job, 1);
  job->p = p;
  job->key = g_strdup(key);
  job->path = g_strdup(path);
  g_hash_table_replace(p->remote, g_strdup(key), PENDING);
  g_subprocess_communicate_async(process, NULL, p->cancel, job_done, job);
  g_object_unref(process);
}

/* What p->remote holds for key: a new reference to its paintable, or NULL (pending, failed, unknown:
 * *known tells). */
static GdkPaintable *remote_lookup(Preview *p, const char *key, gboolean *known)
{
  gpointer value;

  *known = g_hash_table_lookup_extended(p->remote, key, NULL, &value);
  return *known && value != PENDING && value != FAILED ? g_object_ref(value) : NULL;
}

static GdkPaintable *web_image(Preview *p, const char *url)
{
  static char *curl;
  gboolean known;
  GdkPaintable *paintable = remote_lookup(p, url, &known);

  if (known)
    return paintable;
  if (!curl)
    curl = g_find_program_in_path("curl");
  if (!curl) {
    g_hash_table_replace(p->remote, g_strdup(url), FAILED);
    return NULL;
  }
  const char *const argv[] = {
    curl,          "-sSfL",         "--max-time",  "20", "--max-filesize", MAX_BYTES, "--proto",
    "=http,https", "--proto-redir", "=http,https", "-A", TCODE_NAME,       "--url",   url,
    NULL
  };
  start_job(p, url, NULL, argv);
  return NULL;
}

GdkPaintable *preview_image(Preview *p, const char *dir, const char *src)
{
  char *path;
  GdkPaintable *paintable;

  if (g_str_has_prefix(src, "https://") || g_str_has_prefix(src, "http://"))
    return web_image(p, src);
  path = preview_local_path(dir, src);
  paintable = path ? paintable_from_file(path) : NULL;
  g_free(path);
  return paintable;
}

static char *mmdc_path(void)
{
  static char *mmdc;
  static gboolean looked;

  if (!looked) {
    mmdc = g_find_program_in_path("mmdc");
    looked = TRUE;
  }
  return mmdc;
}

gboolean preview_can_draw_mermaid(void)
{
  return mmdc_path() != NULL;
}

GdkPaintable *preview_mermaid(Preview *p, const char *source)
{
  const char *mmdc = mmdc_path();
  const char *theme = palette_light() ? "default" : "dark";
  char *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, source, -1);
  char *key = g_strdup_printf("mermaid:%s-%s", sum, theme);
  char *dir = g_build_filename(g_get_user_cache_dir(), TCODE_NAME, "mermaid", NULL);
  char *name = g_strdup_printf("%s-%s.png", sum, theme), *png = g_build_filename(dir, name, NULL);
  gboolean known;
  GdkPaintable *paintable = remote_lookup(p, key, &known);

  if (!known && g_file_test(png, G_FILE_TEST_IS_REGULAR)) { /* drawn before */
    paintable = paintable_from_file(png);
    g_hash_table_replace(p->remote, g_strdup(key), paintable ? g_object_ref(paintable) : FAILED);
  } else if (!known) {
    char *input = g_strconcat(png, ".mmd", NULL);
    if (mmdc && g_mkdir_with_parents(dir, 0700) == 0 && g_file_set_contents(input, source, -1, NULL)) {
      const char *const argv[] = {
        mmdc, "-q", "-i", input, "-o", png, "-t", theme, "-b", "transparent", "-s", "2", NULL
      };
      start_job(p, key, png, argv);
    } else {
      g_hash_table_replace(p->remote, g_strdup(key), FAILED);
    }
    g_free(input);
  }
  g_free(png);
  g_free(name);
  g_free(dir);
  g_free(key);
  g_free(sum);
  return paintable;
}

/* Pictures ---------------------------------------------------------------------------------------- */

static void on_picture_click(GtkGestureClick *gesture, int n_press G_GNUC_UNUSED, double x G_GNUC_UNUSED,
                             double y G_GNUC_UNUSED, Preview *p)
{
  GtkWidget *picture = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  char *url = g_strdup(g_object_get_data(G_OBJECT(picture), "t-code-href")); /* following it may free the picture */

  preview_follow(p, url);
  g_free(url);
}

GtkWidget *preview_picture_new(Preview *p, GdkPaintable *paintable, int width, const char *href, const char *title)
{
  GtkWidget *picture = gtk_picture_new_for_paintable(paintable);

  g_object_set_data(G_OBJECT(picture), "t-code-width",
                    GINT_TO_POINTER(width > 0 ? width : gdk_paintable_get_intrinsic_width(paintable)));
  gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
  if (title && *title)
    gtk_widget_set_tooltip_text(picture, title);
  if (href && *href) { /* an image inside a link (a badge): clicking it follows the link */
    GtkGesture *click = gtk_gesture_click_new();
    g_object_set_data_full(G_OBJECT(picture), "t-code-href", g_strdup(href), g_free);
    g_signal_connect(click, "released", G_CALLBACK(on_picture_click), p);
    gtk_widget_add_controller(picture, GTK_EVENT_CONTROLLER(click));
    gtk_widget_set_cursor_from_name(picture, "pointer");
  }
  return picture;
}

/* Image files ------------------------------------------------------------------------------------ */

/* A click on an image's picture switches between fitting the view and its own size. */
static void on_image_click(GtkGestureClick *gesture, int n_press G_GNUC_UNUSED, double x G_GNUC_UNUSED,
                           double y G_GNUC_UNUSED, Preview *p)
{
  GtkWidget *picture = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  gboolean actual = !g_object_get_data(G_OBJECT(picture), "t-code-actual");

  g_object_set_data(G_OBJECT(picture), "t-code-actual", GINT_TO_POINTER(actual));
  gtk_widget_set_tooltip_text(picture, actual ? "Click to fit the view" : "Click to show at its own size");
  preview_size_picture(picture, preview_content_width(p));
}

void preview_render_image(Preview *p)
{
  GtkTextBuffer *b = p->buffer;
  GdkPaintable *paintable = paintable_from_file(p->tab->path);
  GtkTextIter end, from;
  GStatBuf st;
  char *type = g_content_type_guess(p->tab->path, NULL, 0, NULL), *kind = g_content_type_get_description(type);
  char *bytes = g_stat(p->tab->path, &st) == 0 ? g_format_size(st.st_size) : g_strdup("");

  if (paintable) {
    GtkWidget *picture = preview_picture_new(p, paintable, 0, NULL, NULL);
    GtkGesture *click = gtk_gesture_click_new();
    char *line = g_strdup_printf("\n%d × %d · %s · %s", gdk_paintable_get_intrinsic_width(paintable),
                                 gdk_paintable_get_intrinsic_height(paintable), bytes, kind);
    g_signal_connect(click, "released", G_CALLBACK(on_image_click), p);
    gtk_widget_add_controller(picture, GTK_EVENT_CONTROLLER(click));
    gtk_widget_set_tooltip_text(picture, "Click to show at its own size");
    preview_size_picture(picture, preview_content_width(p));
    gtk_text_buffer_get_end_iter(b, &end);
    gtk_text_view_add_child_at_anchor(p->view, picture, gtk_text_buffer_create_child_anchor(b, &end));
    g_ptr_array_add(p->pictures, picture);
    gtk_text_buffer_get_end_iter(b, &end);
    gtk_text_buffer_insert_with_tags_by_name(b, &end, line, -1, "caption", NULL);
    g_free(line);
    g_object_unref(paintable);
  } else {
    char *line = g_strdup_printf("This image cannot be shown (%s, %s).", kind, bytes);
    gtk_text_buffer_get_end_iter(b, &end);
    gtk_text_buffer_insert_with_tags_by_name(b, &end, line, -1, "error", NULL);
    g_free(line);
  }
  gtk_text_buffer_get_bounds(b, &from, &end);
  gtk_text_buffer_apply_tag_by_name(b, "center", &from, &end);
  gtk_text_buffer_apply_tag_by_name(b, "base", &from, &end);
  g_free(bytes);
  g_free(kind);
  g_free(type);
}
