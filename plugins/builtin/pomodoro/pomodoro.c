/* Pomodoro for t-code, in C (C11 and POSIX only).
 *
 *   begin  starts a focus session (25 minutes); the process that runs it stays and keeps an item at
 *          the right of the status bar ("🍅 24:59"), then a break ("☕ 4:59", a long one after every
 *          fourth session); a desktop notification (notify-send) says when each ends
 *   pause  pauses the running session, or resumes it
 *   stop   ends it; the item goes
 *
 * The commands share a small state file in $XDG_RUNTIME_DIR/t-code-pomodoro/; the process counting
 * down reads it each second. Durations: ~/.config/t-code-pomodoro/settings, lines like "focus=50",
 * "break=10", "long=20" (minutes). It speaks tapi_1 over $TCODE_SOCKET (t-code's docs/plugins.md);
 * run.sh builds it. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The state ------------------------------------------------------------------------------------ */

enum phase { IDLE, FOCUS, SHORT_BREAK, LONG_BREAK };

struct state {
  enum phase phase;
  long end;    /* when the phase ends (seconds since the epoch), while it runs */
  long paused; /* seconds left when paused, else 0 */
  int done;    /* focus sessions finished */
  long ticker; /* the process counting down */
};

struct settings {
  int focus, short_break, long_break; /* minutes */
};

static const char *phase_name(enum phase p)
{
  return p == FOCUS ? "focus" : p == SHORT_BREAK ? "short" : p == LONG_BREAK ? "long" : "idle";
}

static enum phase phase_of(const char *name)
{
  return !strcmp(name, "focus")   ? FOCUS
         : !strcmp(name, "short") ? SHORT_BREAK
         : !strcmp(name, "long")  ? LONG_BREAK
                                  : IDLE;
}

/* "key=value" lines; unknown keys and bad lines are skipped. */
static void parse_state(const char *text, struct state *s)
{
  memset(s, 0, sizeof *s);
  for (const char *line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
    char key[16] = "", value[32] = "";
    if (sscanf(line, "%15[a-z]=%31[^\n]", key, value) != 2)
      continue;
    if (!strcmp(key, "phase"))
      s->phase = phase_of(value);
    else if (!strcmp(key, "end"))
      s->end = atol(value);
    else if (!strcmp(key, "paused"))
      s->paused = atol(value);
    else if (!strcmp(key, "done"))
      s->done = atoi(value);
    else if (!strcmp(key, "ticker"))
      s->ticker = atol(value);
  }
}

static void format_state(const struct state *s, char *out, size_t size)
{
  snprintf(out, size, "phase=%s\nend=%ld\npaused=%ld\ndone=%d\nticker=%ld\n", phase_name(s->phase), s->end, s->paused,
           s->done, s->ticker);
}

static void parse_settings(const char *text, struct settings *s)
{
  s->focus = 25, s->short_break = 5, s->long_break = 15;
  for (const char *line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
    char key[16] = "";
    int minutes = 0;
    if (sscanf(line, " %15[a-z] = %d", key, &minutes) != 2 || minutes < 1 || minutes > 600)
      continue;
    if (!strcmp(key, "focus"))
      s->focus = minutes;
    else if (!strcmp(key, "break"))
      s->short_break = minutes;
    else if (!strcmp(key, "long"))
      s->long_break = minutes;
  }
}

/* "24:59", "1:02:03" past an hour. */
static void format_left(long seconds, char *out, size_t size)
{
  if (seconds < 0)
    seconds = 0;
  if (seconds >= 3600)
    snprintf(out, size, "%ld:%02ld:%02ld", seconds / 3600, seconds / 60 % 60, seconds % 60);
  else
    snprintf(out, size, "%ld:%02ld", seconds / 60, seconds % 60);
}

/* The status bar item's text for s at now. */
static void item_text(const struct state *s, long now, char *out, size_t size)
{
  char left[32];
  const char *icon = s->phase == FOCUS ? "🍅" : "☕";
  format_left(s->paused ? s->paused : s->end - now, left, sizeof left);
  snprintf(out, size, "%s%s %s", s->paused ? "⏸ " : "", icon, left);
}

/* What comes after s's phase ends at now: a break after focus (long after every fourth), idle after
 * a break. Returns the message for the user. */
static const char *next_phase(struct state *s, const struct settings *set, long now)
{
  if (s->phase == FOCUS) {
    s->done++;
    s->phase = s->done % 4 == 0 ? LONG_BREAK : SHORT_BREAK;
    s->end = now + 60L * (s->phase == LONG_BREAK ? set->long_break : set->short_break);
    return s->phase == LONG_BREAK ? "Focus done: a long break now" : "Focus done: take a break";
  }
  s->phase = IDLE;
  s->end = 0;
  return "Break over: Pomodoro: Start when you are ready";
}

/* Files ---------------------------------------------------------------------------------------- */

static char *read_file(const char *path)
{
  FILE *f = fopen(path, "r");
  if (!f)
    return NULL;
  char *text = calloc(1, 4096);
  if (text)
    fread(text, 1, 4095, f);
  fclose(f);
  return text;
}

/* In the user's runtime folder, else in /tmp under a name of the user's own (another user could have made a
 * shared one first). */
static void state_dir(char *out, size_t size)
{
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  if (runtime && *runtime)
    snprintf(out, size, "%s/t-code-pomodoro", runtime);
  else
    snprintf(out, size, "/tmp/t-code-pomodoro-%ld", (long)getuid());
}

/* Whether dir is a folder of the user's own, no link (made 0700 when it is not there). */
static int own_dir(const char *dir)
{
  struct stat info;
  if (mkdir(dir, 0700) != 0 && errno != EEXIST)
    return 0;
  return lstat(dir, &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == getuid() && chmod(dir, 0700) == 0;
}

static void load_state(struct state *s)
{
  char path[4096];
  state_dir(path, sizeof path);
  strncat(path, "/state", sizeof path - strlen(path) - 1);
  char *text = read_file(path);
  parse_state(text ? text : "", s);
  free(text);
}

/* Written whole under another name, then renamed: a reader never sees half of it. */
static int save_state(const struct state *s)
{
  char dir[4000], path[4096], temporary[4096], text[512];
  state_dir(dir, sizeof dir);
  if (!own_dir(dir))
    return -1;
  snprintf(path, sizeof path, "%s/state", dir);
  snprintf(temporary, sizeof temporary, "%s/state.%ld", dir, (long)getpid());
  format_state(s, text, sizeof text);
  FILE *f = fopen(temporary, "w");
  if (!f)
    return -1;
  fputs(text, f);
  if (fclose(f) != 0)
    return -1;
  return rename(temporary, path);
}

static void load_settings(struct settings *s)
{
  char path[4096];
  const char *config = getenv("XDG_CONFIG_HOME");
  if (config && *config)
    snprintf(path, sizeof path, "%s/t-code-pomodoro/settings", config);
  else
    snprintf(path, sizeof path, "%s/.config/t-code-pomodoro/settings", getenv("HOME") ? getenv("HOME") : "");
  char *text = read_file(path);
  parse_settings(text ? text : "", s);
  free(text);
}

/* Whether pid is a running Pomodoro (not another program that got the number since). */
static int ticker_alive(long pid)
{
  if (pid <= 0 || kill((pid_t)pid, 0) != 0)
    return 0;
  char path[64];
  snprintf(path, sizeof path, "/proc/%ld/cmdline", pid);
  char *cmdline = read_file(path);
  int ours = cmdline && strstr(cmdline, "pomodoro") != NULL;
  free(cmdline);
  return ours;
}

/* Talking to t-code ----------------------------------------------------------------------------- */

struct tcode {
  int fd;
  char buffer[4096];
  size_t length;
};

/* The next line from t-code, without its line break; -1 when it went away. */
static int read_line(struct tcode *t, char *line, size_t size)
{
  while (1) {
    char *end = memchr(t->buffer, '\n', t->length);
    if (end) {
      size_t n = (size_t)(end - t->buffer);
      snprintf(line, size, "%.*s", (int)n, t->buffer);
      memmove(t->buffer, end + 1, t->length - n - 1);
      t->length -= n + 1;
      return 0;
    }
    if (t->length == sizeof t->buffer)
      t->length = 0; /* a line longer than the buffer: none of ours */
    ssize_t got = read(t->fd, t->buffer + t->length, sizeof t->buffer - t->length);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0)
      return -1;
    t->length += (size_t)got;
  }
}

/* One request, ended by a NUL; 0 on "ok ...", 1 on "error ...", -1 when t-code went away. */
static int request(struct tcode *t, const char *command, const char *argument)
{
  char message[1024], answer[1024];
  int n = snprintf(message, sizeof message, "%s %s", command, argument ? argument : "");
  if (n < 0 || (size_t)n >= sizeof message)
    return 1;
  for (size_t sent = 0; sent < (size_t)n + 1;) { /* the NUL too */
    ssize_t w = send(t->fd, message + sent, (size_t)n + 1 - sent, MSG_NOSIGNAL);
    if (w < 0 && errno == EINTR)
      continue;
    if (w <= 0)
      return -1;
    sent += (size_t)w;
  }
  if (read_line(t, answer, sizeof answer) < 0)
    return -1;
  return strncmp(answer, "ok", 2) == 0 ? 0 : 1;
}

static int tcode_connect(struct tcode *t)
{
  const char *path = getenv("TCODE_SOCKET");
  struct sockaddr_un address = { .sun_family = AF_UNIX };
  char hello[512];

  memset(t, 0, sizeof *t);
  if (!path || strlen(path) >= sizeof address.sun_path) {
    fprintf(stderr, "pomodoro: no usable $TCODE_SOCKET: t-code runs this plugin\n");
    return -1;
  }
  strcpy(address.sun_path, path);
  t->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (t->fd < 0 || connect(t->fd, (struct sockaddr *)&address, sizeof address) < 0) {
    fprintf(stderr, "pomodoro: cannot reach t-code: %s\n", strerror(errno));
    return -1;
  }
  const char *token = getenv("TCODE_PLUGIN_TOKEN");
  snprintf(hello, sizeof hello, "tapi_1%s%s", token && *token ? " " : "", token ? token : "");
  return request(t, "hello", hello) == 0 ? 0 : -1;
}

/* A desktop notification, when notify-send is there; never waited for long. */
static void notify(const char *text)
{
  pid_t child = fork();
  if (child == 0) {
    int null = open("/dev/null", 0);
    dup2(null, 1);
    dup2(null, 2);
    execlp("notify-send", "notify-send", "--app-name=t-code", "Pomodoro", text, (char *)NULL);
    _exit(127);
  }
  if (child > 0)
    waitpid(child, NULL, 0);
}

/* Counting down ---------------------------------------------------------------------------------- */

/* Counts down until the state goes idle or t-code goes away: the item each second, the next phase
 * when one ends. */
static int tick(struct tcode *t, const struct settings *set)
{
  char shown[64] = "";
  while (1) {
    struct state s;
    load_state(&s);
    if (s.phase == IDLE || s.ticker != (long)getpid()) { /* stopped, or another one took over */
      request(t, "status-clear", "pomodoro");
      return 0;
    }
    long now = (long)time(NULL);
    if (!s.paused && now >= s.end) {
      const char *message = next_phase(&s, set, now);
      save_state(&s);
      char said[256];
      snprintf(said, sizeof said, "Pomodoro: %s", message);
      if (request(t, "say", said) < 0)
        return 0;
      notify(message);
      continue;
    }
    char text[64], argument[96];
    item_text(&s, now, text, sizeof text);
    if (strcmp(text, shown) != 0) {
      snprintf(argument, sizeof argument, "pomodoro %s", text);
      if (request(t, "status-set", argument) < 0)
        return 0; /* t-code went away */
      snprintf(shown, sizeof shown, "%s", text);
    }
    usleep(250 * 1000);
  }
}

static int begin(void)
{
  struct settings set;
  struct state s;
  struct tcode t;
  load_settings(&set);
  load_state(&s);
  if (tcode_connect(&t) < 0)
    return 1;
  int running = s.phase != IDLE && ticker_alive(s.ticker);
  if (running && s.phase == FOCUS && !s.paused) {
    char left[32], said[96];
    format_left(s.end - (long)time(NULL), left, sizeof left);
    snprintf(said, sizeof said, "Pomodoro: already focusing, %s to go", left);
    request(&t, "say", said);
    return 0;
  }
  s.phase = FOCUS;
  s.end = (long)time(NULL) + 60L * set.focus;
  s.paused = 0;
  if (!running)
    s.ticker = (long)getpid();
  if (save_state(&s) != 0) {
    fprintf(stderr, "pomodoro: cannot write its state: %s\n", strerror(errno));
    return 1;
  }
  char said[96];
  snprintf(said, sizeof said, "Pomodoro: %d minutes of focus", set.focus);
  request(&t, "say", said);
  return running ? 0 : tick(&t, &set);
}

static int pause_or_resume(void)
{
  struct state s;
  struct tcode t;
  load_state(&s);
  if (tcode_connect(&t) < 0)
    return 1;
  if (s.phase == IDLE || !ticker_alive(s.ticker)) {
    request(&t, "say", "Pomodoro: nothing running: Pomodoro: Start begins a session");
    return 0;
  }
  long now = (long)time(NULL);
  if (s.paused) {
    s.end = now + s.paused;
    s.paused = 0;
  } else {
    s.paused = s.end > now ? s.end - now : 1;
  }
  save_state(&s);
  request(&t, "say", s.paused ? "Pomodoro: paused" : "Pomodoro: going again");
  return 0;
}

static int stop(void)
{
  struct state s;
  struct tcode t;
  load_state(&s);
  if (tcode_connect(&t) < 0)
    return 1;
  int done = s.done;
  memset(&s, 0, sizeof s);
  s.done = done;
  save_state(&s); /* the counting process sees it and takes its item away */
  request(&t, "status-clear", "pomodoro");
  request(&t, "say", "Pomodoro: stopped");
  return 0;
}

#ifndef POMODORO_NO_MAIN
int main(int argc, char **argv)
{
  const char *command = argc > 1 ? argv[1] : "";
  if (!strcmp(command, "begin"))
    return begin();
  if (!strcmp(command, "pause"))
    return pause_or_resume();
  if (!strcmp(command, "stop"))
    return stop();
  fprintf(stderr, "pomodoro: unknown command \"%s\"\n", command);
  return 1;
}
#endif
