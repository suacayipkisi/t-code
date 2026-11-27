#include "plugins/tapi.h"

#include <string.h>

/* Reads 1 to 3 decimal digits at *text (no sign, no spaces), moving past them; -1 when there are none or more. */
static int digits(const char **text)
{
  int value = 0, count = 0;

  while (g_ascii_isdigit(**text)) {
    if (++count > 3)
      return -1;
    value = value * 10 + (**text - '0');
    (*text)++;
  }
  return count ? value : -1;
}

int tapi_parse(const char *text)
{
  int major, minor = 0;

  if (!text || !g_str_has_prefix(text, "tapi_"))
    return 0;
  text += strlen("tapi_");
  if ((major = digits(&text)) < 1)
    return 0;
  if (*text == '_') {
    text++;
    if ((minor = digits(&text)) < 0)
      return 0;
  }
  return *text ? 0 : TAPI_VERSION(major, minor);
}

gboolean tapi_spoken(int version)
{
  return version >= TAPI_VERSION(1, 0) && version <= TAPI_LATEST;
}

gboolean tapi_too_new(const char *text)
{
  return tapi_parse(text) > TAPI_LATEST;
}

char *tapi_name(int version)
{
  int major = version / 1000, minor = version % 1000;

  return minor ? g_strdup_printf("tapi_%d_%d", major, minor) : g_strdup_printf("tapi_%d", major);
}
