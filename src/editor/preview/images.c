/*
 * The preview's images (see state.h): local files, web addresses (fetched by curl in the background
 * and kept while the preview is open), SVG drawn by GTK itself (4.22 or newer; before it gdk-pixbuf), and mermaid diagrams
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
#define SVG_PIXELS 16000000  /* at most so many pixels drawn (64 MB): a huge SVG is not shown */
#define WEB_FETCHES 4        /* web images fetched at a time */

void preview_remote_free(gpointer data)
{
  if (data != PENDING && data != FAILED)
    g_object_unref(data);
}

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

static GdkPaintable *scaled_new(GdkTexture *texture)
{
  TcodeScaled *scaled = g_object_new(TCODE_TYPE_SCALED, NULL);

  scaled->texture = texture;
  return GDK_PAINTABLE(scaled);
}

#if GTK_CHECK_VERSION(4, 22, 0)
static void ignore_svg_error(GtkWidget *svg G_GNUC_UNUSED, GError *error G_GNUC_UNUSED)
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

  if (w <= 0 || h <= 0 || (double)w * h * SVG_SCALE * SVG_SCALE > SVG_PIXELS)
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
  return scaled_new(texture);
}
#else
static void at_svg_scale(GdkPixbufLoader *loader, int width, int height, gpointer data G_GNUC_UNUSED)
{
  if (width > 0 && height > 0 && (double)width * height * SVG_SCALE * SVG_SCALE <= SVG_PIXELS)
    gdk_pixbuf_loader_set_size(loader, width * SVG_SCALE, height * SVG_SCALE);
  else /* too big to draw (or no size): a pixel, which svg_from_pixbuf turns down */
    gdk_pixbuf_loader_set_size(loader, 1, 1);
}

/* Before GTK 4.22 (no GtkSvg; gdk_texture_new_from_bytes reads no SVG): gdk-pixbuf's SVG loader
 * (librsvg), SVG_SCALE times the SVG's size like rasterize(). NULL without the loader. */
static GdkPaintable *svg_from_pixbuf(GBytes *bytes)
{
  GdkPixbufLoader *loader = gdk_pixbuf_loader_new_with_type("svg", NULL);
  GdkPaintable *paintable = NULL;
  gsize size;
  const guchar *data = g_bytes_get_data(bytes, &size);

  if (!loader)
    return NULL;
  g_signal_connect(loader, "size-prepared", G_CALLBACK(at_svg_scale), NULL);
  if (gdk_pixbuf_loader_write(loader, data, size, NULL) && gdk_pixbuf_loader_close(loader, NULL)) {
    GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
    if (pixbuf && gdk_pixbuf_get_width(pixbuf) >= SVG_SCALE && gdk_pixbuf_get_height(pixbuf) >= SVG_SCALE) {
      GBytes *pixels = gdk_pixbuf_read_pixel_bytes(pixbuf);
      GdkTexture *texture =
          gdk_memory_texture_new(gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf),
                                 gdk_pixbuf_get_has_alpha(pixbuf) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8, pixels,
                                 gdk_pixbuf_get_rowstride(pixbuf));
      g_bytes_unref(pixels);
      paintable = scaled_new(texture);
    }
  } else {
    gdk_pixbuf_loader_close(loader, NULL);
  }
  g_object_unref(loader);
  return paintable;
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
    /* nothing it would fetch; ANIMATIONS on, as without it GtkSvg (4.24) leaves <text> out (badges lost their
     * labels), and it is drawn once into a texture anyway: nothing runs */
    gtk_svg_set_features(image, GTK_SVG_SYSTEM_RESOURCES | GTK_SVG_ANIMATIONS);
    gtk_svg_load_from_bytes(image, bytes);
    paintable = rasterize(GDK_PAINTABLE(image));
    g_object_unref(image);
    return paintable;
  }
#else
  if (svg && (paintable = svg_from_pixbuf(bytes)))
    return paintable;
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
  char *key;    /* p->remote's key */
  char *path;   /* the file the job writes (a diagram), or NULL: its output is the image */
  gboolean web; /* a web image's fetch: the next waiting one starts when it ends */
} Job;

static void start_web(Preview *p, const char *url);

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
    if (job->web) {
      char *next;
      job->p->web_running--;
      if ((next = g_queue_pop_head(&job->p->web_waiting))) {
        start_web(job->p, next);
        g_free(next);
      }
    }
  }
  g_clear_error(&error);
  if (out)
    g_bytes_unref(out);
  g_free(job->key);
  g_free(job->path);
  g_free(job);
}

/* Runs argv in the background for p->remote's key (path: the file it writes, or NULL: its output);
 * FALSE when it did not start. */
static gboolean start_job(Preview *p, const char *key, const char *path, const char *const *argv, gboolean web)
{
  GSubprocess *process =
      g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
  Job *job;

  if (!process) {
    g_hash_table_replace(p->remote, g_strdup(key), FAILED);
    return FALSE;
  }
  job = g_new0(Job, 1);
  job->p = p;
  job->key = g_strdup(key);
  job->path = g_strdup(path);
  job->web = web;
  g_hash_table_replace(p->remote, g_strdup(key), PENDING);
  g_subprocess_communicate_async(process, NULL, p->cancel, job_done, job);
  g_object_unref(process);
  return TRUE;
}

/* What p->remote holds for key: a new reference to its paintable, or NULL (pending, failed, unknown:
 * *known tells). */
static GdkPaintable *remote_lookup(Preview *p, const char *key, gboolean *known)
{
  gpointer value;

  *known = g_hash_table_lookup_extended(p->remote, key, NULL, &value);
  return *known && value != PENDING && value != FAILED ? g_object_ref(value) : NULL;
}

/* Fetches url with curl now (a slot is free). */
static void start_web(Preview *p, const char *url)
{
  char *curl = g_find_program_in_path("curl");

  if (!curl) {
    g_hash_table_replace(p->remote, g_strdup(url), FAILED);
    return;
  }
  const char *const argv[] = {
    curl,          "-sSfL",         "--max-time",  "20", "--max-filesize", MAX_BYTES, "--proto",
    "=http,https", "--proto-redir", "=http,https", "-A", TCODE_NAME,       "--url",   url,
    NULL
  };
  if (start_job(p, url, NULL, argv, TRUE))
    p->web_running++;
  g_free(curl);
}

/* A web image: fetched in the background, WEB_FETCHES at a time (a README with fifty badges does not
 * start fifty curls), unless settings.web_images is off. */
static GdkPaintable *web_image(Preview *p, const char *url)
{
  gboolean known;
  GdkPaintable *paintable = remote_lookup(p, url, &known);

  if (known || !p->app->settings.web_images)
    return paintable;
  if (p->web_running >= WEB_FETCHES) {
    g_hash_table_replace(p->remote, g_strdup(url), PENDING);
    g_queue_push_tail(&p->web_waiting, g_strdup(url));
    return NULL;
  }
  start_web(p, url);
  return NULL;
}

/* A local image decoded in a thread: a big PNG took a quarter of a second, and the preview is shown
 * again on every save. The texture is kept while the preview is open (until the file changes). */
typedef struct {
  Preview *p;
  char *key, *path;
} Decode;

static void decode_free(gpointer data)
{
  Decode *decode = data;

  g_free(decode->key);
  g_free(decode->path);
  g_free(decode);
}

static void decode_in_thread(GTask *task, gpointer source G_GNUC_UNUSED, gpointer data,
                             GCancellable *cancel G_GNUC_UNUSED)
{
  Decode *decode = data;
  char *bytes;
  gsize size;
  GdkTexture *texture = NULL;

  if (g_file_get_contents(decode->path, &bytes, &size, NULL)) {
    GBytes *contents = g_bytes_new_take(bytes, size);
    texture = gdk_texture_new_from_bytes(contents, NULL); /* textures may be made in any thread */
    g_bytes_unref(contents);
  }
  g_task_return_pointer(task, texture, texture ? g_object_unref : NULL);
}

static void decode_done(GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer data G_GNUC_UNUSED)
{
  Decode *decode = g_task_get_task_data(G_TASK(result));
  GError *error = NULL;
  GdkTexture *texture = g_task_propagate_pointer(G_TASK(result), &error);

  if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) { /* else the preview is gone */
    Preview *p = decode->p;
    char *prefix = g_strconcat(decode->path, "\n", NULL);
    GHashTableIter iter;
    gpointer key;
    g_hash_table_iter_init(&iter, p->remote); /* the file as it was before it changed */
    while (g_hash_table_iter_next(&iter, &key, NULL))
      if (g_str_has_prefix(key, prefix) && strcmp(key, decode->key) != 0)
        g_hash_table_iter_remove(&iter);
    g_free(prefix);
    g_hash_table_replace(p->remote, g_strdup(decode->key), texture ? (gpointer)texture : FAILED);
    if (texture)
      preview_reload_later(p);
  } else if (texture) {
    g_object_unref(texture);
  }
  g_clear_error(&error);
}

static GdkPaintable *local_image(Preview *p, const char *path)
{
  GStatBuf st;
  gboolean known;
  char *lower = g_ascii_strdown(path, -1);
  gboolean svg = g_str_has_suffix(lower, ".svg") || g_str_has_suffix(lower, ".svgz");

  g_free(lower);
  if (svg || g_stat(path, &st) != 0) /* drawn by GTK's widgets code: in this thread (and small) */
    return paintable_from_file(path);

  char *key = g_strdup_printf("%s\n%lld:%lld", path, (long long)st.st_mtime, (long long)st.st_size);
  GdkPaintable *paintable = remote_lookup(p, key, &known);
  if (!known) {
    Decode *decode = g_new0(Decode, 1);
    GTask *task = g_task_new(NULL, p->cancel, decode_done, NULL);
    decode->p = p;
    decode->key = g_strdup(key);
    decode->path = g_strdup(path);
    g_task_set_task_data(task, decode, decode_free);
    g_task_set_return_on_cancel(task, TRUE);
    g_hash_table_replace(p->remote, g_strdup(key), PENDING);
    g_task_run_in_thread(task, decode_in_thread);
    g_object_unref(task);
  }
  g_free(key);
  return paintable;
}

GdkPaintable *preview_image(Preview *p, const char *dir, const char *src)
{
  char *path;
  GdkPaintable *paintable;

  if (g_str_has_prefix(src, "https://") || g_str_has_prefix(src, "http://"))
    return web_image(p, src);
  path = preview_local_path(dir, src);
  paintable = path ? local_image(p, path) : NULL;
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
      start_job(p, key, png, argv, FALSE);
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
  GdkPaintable *paintable = paintable_from_file(p->path);
  GtkTextIter end, from;
  GStatBuf st;
  char *type = g_content_type_guess(p->path, NULL, 0, NULL), *kind = g_content_type_get_description(type);
  char *bytes = g_stat(p->path, &st) == 0 ? g_format_size(st.st_size) : g_strdup("");

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
