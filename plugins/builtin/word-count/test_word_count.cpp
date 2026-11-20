// The plugin's pieces, without t-code: make test builds and runs this (t-code's test_window runs the
// plugin itself against a window).
#define WORD_COUNT_NO_MAIN
#include "word_count.cpp"

#include <sys/socket.h>

#include <cstdio>

static int failures = 0;

#define CHECK(condition)                                                                                               \
  do {                                                                                                                 \
    if (!(condition)) {                                                                                                \
      std::fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #condition);                                     \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

using namespace word_count;

static void test_count()
{
  CHECK(count("").words == 0 && count("").lines == 0);
  CHECK(count("one two\nthree\n").words == 3 && count("one two\nthree\n").lines == 2);
  CHECK(count("no line break at the end").words == 6 && count("no line break at the end").lines == 1);
  CHECK(count("\n\n  \t\n").words == 0 && count("\n\n  \t\n").lines == 3);
  CHECK(count("çağrı işi\n").words == 2);
  CHECK(describe("word\n") == "1 word, 1 line");
  CHECK(describe("a b\nc\n") == "3 words, 2 lines");
}

static void test_events()
{
  const std::string event = R"({"event":"did_change","path":"/p/a \"b\".c","by":"plugin"})";
  CHECK(json_member(event, "event") == "did_change");
  CHECK(json_member(event, "path") == "/p/a \"b\".c");
  CHECK(json_member(event, "missing").empty());
  CHECK(json_member(R"({"n": 3, "nested": {"path": "inner"}, "list": ["x", "}"], "path": "outer"})", "path") ==
        "outer");
  CHECK(json_member(R"({"path":"ç🙂\n"})", "path") == "ç🙂\n");
  CHECK(json_member("not json", "path").empty());
  CHECK(json_member(R"({"path":"unterminated)", "path").empty());
}

// Answers written ahead into the other end of a socket pair: values, a text with its NUL, an error.
static void test_answers()
{
  int fds[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  const char answers[] = "ok /p/a.c\nok\nline one\nline two\0ok\nerror no-editor nothing open here\n";
  CHECK(write(fds[1], answers, sizeof answers - 1) == static_cast<ssize_t>(sizeof answers - 1));
  {
    Tcode t(fds[0]);
    CHECK(t.request("get-file") == "/p/a.c");
    CHECK(t.text("get-text") == "line one\nline two");
    CHECK(t.request("say", "hi") == "");
    try {
      t.text("get-selection");
      CHECK(false);
    } catch (const Error &e) {
      CHECK(e.code == "no-editor");
    }
    char sent[256] = { 0 };
    ssize_t n = read(fds[1], sent, sizeof sent - 1);
    CHECK(n > 0 && std::string(sent, static_cast<size_t>(n)) ==
                       std::string("get-file \0get-text \0say hi\0get-selection \0", 42));
  }
  close(fds[1]);
}

int main()
{
  test_count();
  test_events();
  test_answers();
  if (failures)
    std::fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
