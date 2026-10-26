/*
 * The command line (see application.h).
 */
#include "application/application.h"

#include <gio/gunixsocketaddress.h>
#include <string.h>

#ifndef TCODE_VERSION /* set by the Makefile */
#define TCODE_VERSION "unknown"
#endif

static gboolean new_window;

gboolean cli_new_window(void)
{
  return new_window;
}

/* --send "<command> <arg>": one request to the t-code whose shell this runs in ($TCODE_SOCKET), ended
 * with a NUL (a path may hold a line break). The build for debugging ends with it (project/run.c). */
static int send_request(const char *request)
{
  const char *path = g_getenv("TCODE_SOCKET");
  GSocketClient *client = g_socket_client_new();
  GSocketAddress *address = path ? g_unix_socket_address_new(path) : NULL;
  GError *error = NULL;
  GSocketConnection *conn =
      address ? g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, &error) : NULL;
  gboolean ok = conn && g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)), request,
                                                  strlen(request) + 1, NULL, NULL, &error);

  if (!ok)
    g_printerr("t-code --send: %s\n", error ? error->message : "TCODE_SOCKET is not set (run it in t-code's shell)");
  g_clear_error(&error);
  g_clear_object(&conn);
  g_clear_object(&address);
  g_object_unref(client);
  return ok ? 0 : 1;
}

static int on_local_options(GApplication *app G_GNUC_UNUSED, GVariantDict *options, gpointer data G_GNUC_UNUSED)
{
  const char *request = NULL;

  if (g_variant_dict_contains(options, "version")) {
    g_print("t-code %s\n", TCODE_VERSION);
    return 0;
  }
  if (g_variant_dict_lookup(options, "send", "&s", &request))
    return send_request(request);
  new_window = g_variant_dict_contains(options, "new-window");
  return -1;
}

void cli_add_options(GApplication *app)
{
  g_application_add_main_option(app, "version", 'v', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, "Show the version and exit",
                                NULL);
  g_application_add_main_option(app, "new-window", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
                                "Open an empty window that asks for a folder", NULL);
  g_application_add_main_option(app, "send", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING,
                                "Send a request to the t-code this runs in", "\"COMMAND ARG\"");
  g_signal_connect(app, "handle-local-options", G_CALLBACK(on_local_options), NULL);
}
