/*
 * The Run and Debug panel's Call Stack (see state.h): a row per frame (frames without source as one
 * row until opened); a click shows that frame's place and variables.
 */
#include "debug/view/state.h"

#include "debug/disassembly.h"
#include "debug/view/debugview.h"
#include "editor/editor.h"
#include "popups/contextmenu.h"

/* Call Stack ------------------------------------------------------------------------------------ */

/* Whether frame has a file on disk to show. */
static gboolean has_source(JsonNode *frame)
{
  const char *path = json_string_at(frame, "source.path");

  return path && g_file_test(path, G_FILE_TEST_IS_REGULAR);
}

static GtkWidget *stack_row(const char *function, const char *where, int index)
{
  GtkWidget *row = gtk_list_box_row_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *name = gtk_label_new(function), *place = gtk_label_new(where);

  gtk_widget_add_css_class(name, "function");
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(name), 0);
  gtk_widget_set_hexpand(name, TRUE);
  gtk_widget_add_css_class(place, "where");
  gtk_box_append(GTK_BOX(box), name);
  gtk_box_append(GTK_BOX(box), place);
  gtk_widget_set_margin_start(box, 10);
  gtk_widget_set_margin_end(box, 10);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  g_object_set_data(G_OBJECT(row), "frame", GINT_TO_POINTER(index)); /* -1: the hidden frames' row */
  return row;
}

/* The frames, the program's own with their file and line. Frames without a file (the C library's,
 * ASan's) in a row go in one dim row, which opens them. */
void stack_fill(void)
{
  guint n = view.frames ? json_array_get_length(json_node_get_array(view.frames)) : 0;

  gtk_list_box_remove_all(view.stack);
  for (guint i = 0; i < n; i++) {
    JsonNode *frame = json_array_get_element(json_node_get_array(view.frames), i);
    const char *function = json_string_at(frame, "name"), *path = json_string_at(frame, "source.path");
    guint run = 0;
    GtkWidget *row;

    while (!view.all_frames && i + run < n &&
           !has_source(json_array_get_element(json_node_get_array(view.frames), i + run)))
      run++;
    if (run > 1) {
      char *text = g_strdup_printf("%u frames in libraries", run);
      row = stack_row(text, "show", -1);
      gtk_widget_set_tooltip_text(row, "Functions of the C library or of ASan, without source: click to list them");
      gtk_widget_add_css_class(row, "no-source");
      gtk_list_box_append(view.stack, row);
      g_free(text);
      i += run - 1;
      continue;
    }
    if (has_source(frame)) {
      char *file = g_path_get_basename(path);
      char *where = g_strdup_printf("%s:%d", file, (int)json_int_at(frame, "line", 0));
      row = stack_row(function ? function : "?", where, (int)i);
      gtk_widget_set_tooltip_text(row, path);
      g_free(where);
      g_free(file);
    } else {
      row = stack_row(function ? function : "?", "", (int)i);
      gtk_widget_set_tooltip_text(row, "No source: a library without debugging information");
      gtk_widget_add_css_class(row, "no-source");
      gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    }
    gtk_list_box_append(view.stack, row);
  }
}

/* The row of frame index, or NULL (hidden). */
GtkListBoxRow *stack_row_of_frame(int index)
{
  for (GtkListBoxRow *row = gtk_list_box_get_row_at_index(view.stack, 0); row;
       row = GTK_LIST_BOX_ROW(gtk_widget_get_next_sibling(GTK_WIDGET(row))))
    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "frame")) == index)
      return row;
  return NULL;
}

/* A click on a frame: its line in the editor, its variables in the list; on the hidden frames' row:
 * they show. */
void stack_on_frame_activated(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer data G_GNUC_UNUSED)
{
  int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "frame"));
  JsonNode *frame = view.frames && index >= 0 ? json_array_get_element(json_node_get_array(view.frames), index) : NULL;
  const char *path = json_string_at(frame, "source.path");

  if (index < 0) {
    view.all_frames = TRUE;
    stack_fill();
    gtk_list_box_select_row(view.stack, stack_row_of_frame(view.shown));
    return;
  }
  if (!frame || !path)
    return;
  open_file_at(view.app, path, (int)json_int_at(frame, "line", 1), 1);
  if (index != view.shown) {
    var_list_frame(index);
    disassembly_update(view.app);
  }
}

static void copy_stack(gpointer data G_GNUC_UNUSED)
{
  char *text = debug_view_stack_text();

  copy_to_clipboard(text);
  g_free(text);
}

void stack_on_menu(GtkWidget *list, double x, double y, gpointer data G_GNUC_UNUSED)
{
  static const MenuItem items[] = { { "Copy Call Stack", copy_stack } };

  if (gtk_widget_get_first_child(list))
    context_menu(list, x, y, items, G_N_ELEMENTS(items), NULL, NULL);
}
