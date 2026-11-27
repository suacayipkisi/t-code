/* The plugin's pieces, without t-code: make test builds and runs this. */
#define POMODORO_NO_MAIN
#include "pomodoro.c"

static int failures;
/* the commands need t-code: here they are only compiled */
static int (*const commands[])(void) = { begin, pause_or_resume, stop };

#define CHECK(condition)                                                                                               \
  do {                                                                                                                 \
    if (!(condition)) {                                                                                                \
      fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #condition);                                          \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

static void test_state(void)
{
  struct state s = { FOCUS, 1000, 0, 3, 42 }, back;
  char text[512];
  format_state(&s, text, sizeof text);
  parse_state(text, &back);
  CHECK(back.phase == FOCUS && back.end == 1000 && back.paused == 0 && back.done == 3 && back.ticker == 42);
  parse_state("junk\nphase=long\nnope=1\n", &back);
  CHECK(back.phase == LONG_BREAK && back.end == 0 && back.done == 0);
  parse_state("", &back);
  CHECK(back.phase == IDLE);
}

static void test_settings(void)
{
  struct settings set;
  parse_settings("", &set);
  CHECK(set.focus == 25 && set.short_break == 5 && set.long_break == 15);
  parse_settings("focus = 50\nbreak=10\n# comment\nlong=0\nlong=99999\n", &set);
  CHECK(set.focus == 50 && set.short_break == 10 && set.long_break == 15); /* 0 and too long are left out */
}

static void test_text(void)
{
  char out[64];
  format_left(25 * 60, out, sizeof out);
  CHECK(!strcmp(out, "25:00"));
  format_left(59, out, sizeof out);
  CHECK(!strcmp(out, "0:59"));
  format_left(3723, out, sizeof out);
  CHECK(!strcmp(out, "1:02:03"));
  format_left(-5, out, sizeof out);
  CHECK(!strcmp(out, "0:00"));
  struct state s = { FOCUS, 1100, 0, 0, 1 };
  item_text(&s, 1000, out, sizeof out);
  CHECK(!strcmp(out, "🍅 1:40"));
  s.phase = SHORT_BREAK, s.paused = 30;
  item_text(&s, 1000, out, sizeof out);
  CHECK(!strcmp(out, "⏸ ☕ 0:30"));
}

static void test_phases(void)
{
  struct settings set = { 25, 5, 15 };
  struct state s = { FOCUS, 0, 0, 0, 1 };
  next_phase(&s, &set, 1000);
  CHECK(s.phase == SHORT_BREAK && s.end == 1000 + 300 && s.done == 1);
  next_phase(&s, &set, 2000);
  CHECK(s.phase == IDLE);
  s.phase = FOCUS, s.done = 3;
  next_phase(&s, &set, 3000);
  CHECK(s.phase == LONG_BREAK && s.end == 3000 + 900 && s.done == 4); /* every fourth */
}

/* Answers written ahead into the other end of a socket pair; the requests come out NUL-ended. */
static void test_requests(void)
{
  int fds[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  const char answers[] = "ok\nerror no-editor nope\n";
  CHECK(write(fds[1], answers, sizeof answers - 1) == (ssize_t)(sizeof answers - 1));
  struct tcode t = { .fd = fds[0] };
  CHECK(request(&t, "status-set", "pomodoro 🍅 25:00") == 0);
  CHECK(request(&t, "get-cursor", NULL) == 1);
  char sent[128] = { 0 };
  ssize_t n = read(fds[1], sent, sizeof sent);
  CHECK(n == (ssize_t)sizeof "status-set pomodoro 🍅 25:00" + (ssize_t)sizeof "get-cursor ");
  CHECK(!memcmp(sent, "status-set pomodoro 🍅 25:00\0get-cursor \0", (size_t)n));
  close(fds[1]);
  CHECK(request(&t, "say", "x") == -1); /* t-code went away */
  close(fds[0]);
}

/* The state file goes whole through its folder in $XDG_RUNTIME_DIR. */
static void test_files(void)
{
  char dir[] = "/tmp/t-code-pomodoro-test-XXXXXX";
  CHECK(mkdtemp(dir) != NULL);
  setenv("XDG_RUNTIME_DIR", dir, 1);
  struct state s = { SHORT_BREAK, 5, 0, 2, 7 }, back;
  CHECK(save_state(&s) == 0);
  load_state(&back);
  CHECK(back.phase == SHORT_BREAK && back.done == 2 && back.ticker == 7);
  CHECK(!ticker_alive(7) || 1); /* pid 7 is never ours: only that it does not crash */
  CHECK(!ticker_alive(0));

  /* the state's folder is the user's own: made 0700, a link to elsewhere refused */
  char made[300], link[300];
  snprintf(made, sizeof made, "%s/made", dir);
  snprintf(link, sizeof link, "%s/link", dir);
  CHECK(own_dir(made));
  struct stat info;
  CHECK(stat(made, &info) == 0 && (info.st_mode & 0777) == 0700);
  CHECK(symlink(made, link) == 0);
  CHECK(!own_dir(link));
  unsetenv("XDG_RUNTIME_DIR");
  char shared[300];
  state_dir(shared, sizeof shared);
  char expected[100];
  snprintf(expected, sizeof expected, "/tmp/t-code-pomodoro-%ld", (long)getuid());
  CHECK(strcmp(shared, expected) == 0);

  char cleanup[200];
  snprintf(cleanup, sizeof cleanup, "rm -rf %s", dir);
  CHECK(system(cleanup) == 0);
}

int main(void)
{
  (void)commands;
  test_state();
  test_settings();
  test_text();
  test_phases();
  test_requests();
  test_files();
  if (failures)
    fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
