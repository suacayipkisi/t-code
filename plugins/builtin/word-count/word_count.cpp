// Word Count for t-code, in C++17 with the standard library and POSIX sockets only.
//
// Started with the project ($1 = "start"), it keeps an item at the right of the status bar with the
// active tab's words and lines, unsaved edits included, updated as typing pauses; a tab that is not
// an editor hides it. "count" (Ctrl+Alt+W) says the count of the selection, or of the whole tab
// without one. It speaks tapi_1 over $TCODE_SOCKET (t-code's docs/plugins.md); run.sh builds it.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace word_count
{

// Counting ---------------------------------------------------------------------------------------

struct Count {
  long words = 0;
  long lines = 0;
};

// Words are runs of characters that are not ASCII white space; a last line without a line break
// counts. Bytes, not characters, are looked at: UTF-8 never puts a space byte inside a character.
inline Count count(const std::string &text)
{
  Count c;
  bool in_word = false;
  for (unsigned char ch : text) {
    bool space = ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f';
    if (ch == '\n')
      c.lines++;
    if (!space && !in_word)
      c.words++;
    in_word = !space;
  }
  if (!text.empty() && text.back() != '\n')
    c.lines++;
  return c;
}

inline std::string describe(const std::string &text)
{
  Count c = count(text);
  return std::to_string(c.words) + (c.words == 1 ? " word, " : " words, ") + std::to_string(c.lines) +
         (c.lines == 1 ? " line" : " lines");
}

// Events: one flat JSON object per line ---------------------------------------------------------

inline void put_utf8(std::string &out, unsigned long code)
{
  if (code < 0x80) {
    out += static_cast<char>(code);
  } else if (code < 0x800) {
    out += static_cast<char>(0xC0 | (code >> 6));
    out += static_cast<char>(0x80 | (code & 0x3F));
  } else if (code < 0x10000) {
    out += static_cast<char>(0xE0 | (code >> 12));
    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (code >> 18));
    out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code & 0x3F));
  }
}

// The JSON string starting at json[at] (its opening quote), unescaped; at ends past its closing
// quote. nullopt when it is not a valid string.
inline std::optional<std::string> json_string_at(const std::string &json, size_t &at)
{
  if (at >= json.size() || json[at] != '"')
    return std::nullopt;
  std::string out;
  for (at++; at < json.size(); at++) {
    char ch = json[at];
    if (ch == '"') {
      at++;
      return out;
    }
    if (ch != '\\') {
      out += ch;
      continue;
    }
    if (++at >= json.size())
      return std::nullopt;
    switch (json[at]) {
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '/': out += '/'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u': {
        auto hex4 = [&](size_t from) -> long {
          if (from + 4 > json.size())
            return -1;
          char digits[5] = { json[from], json[from + 1], json[from + 2], json[from + 3], 0 };
          char *end = nullptr;
          long value = std::strtol(digits, &end, 16);
          return *end ? -1 : value;
        };
        long code = hex4(at + 1);
        if (code < 0)
          return std::nullopt;
        at += 4;
        if (code >= 0xD800 && code < 0xDC00 && at + 6 < json.size() && json[at + 1] == '\\' && json[at + 2] == 'u') {
          long low = hex4(at + 3);
          if (low >= 0xDC00 && low < 0xE000) {
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            at += 6;
          }
        }
        put_utf8(out, static_cast<unsigned long>(code));
        break;
      }
      default: return std::nullopt;
    }
  }
  return std::nullopt;
}

// The string member called name of a JSON object on one line; "" when it is missing or not a string.
// Other values (numbers, nested objects, arrays) are skipped.
inline std::string json_member(const std::string &json, const std::string &name)
{
  auto skip_space = [&](size_t &at) {
    while (at < json.size() && (json[at] == ' ' || json[at] == '\t' || json[at] == '\r' || json[at] == '\n'))
      at++;
  };
  size_t at = 0;
  skip_space(at);
  if (at >= json.size() || json[at] != '{')
    return "";
  at++;
  while (true) {
    skip_space(at);
    auto key = json_string_at(json, at);
    if (!key)
      return "";
    skip_space(at);
    if (at >= json.size() || json[at] != ':')
      return "";
    at++;
    skip_space(at);
    if (at < json.size() && json[at] == '"') {
      auto value = json_string_at(json, at);
      if (!value)
        return "";
      if (*key == name)
        return *value;
    } else { // skip a number, true / false / null, an object or an array, strings inside included
      int depth = 0;
      for (; at < json.size(); at++) {
        char ch = json[at];
        if (ch == '"') {
          if (!json_string_at(json, at))
            return "";
          at--;
        } else if (ch == '{' || ch == '[') {
          depth++;
        } else if (ch == '}' || ch == ']') {
          if (depth == 0)
            break;
          depth--;
        } else if (ch == ',' && depth == 0) {
          break;
        }
      }
    }
    skip_space(at);
    if (at < json.size() && json[at] == ',') {
      at++;
      continue;
    }
    return "";
  }
}

// Talking to t-code --------------------------------------------------------------------------------

// An "error <code> <message>" answer, or "closed" when t-code went away.
struct Error : std::runtime_error {
  std::string code;
  Error(std::string c, const std::string &message) : std::runtime_error(c + ": " + message), code(std::move(c))
  {
  }
};

class Tcode
{
public:
  // A tapi_1 connection to the socket at path, said hello to with the plugin's token.
  Tcode(const std::string &path, const std::string &token)
  {
    fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0)
      throw std::system_error(errno, std::generic_category(), "socket");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof address.sun_path)
      throw Error("bad-argument", "socket path too long");
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (connect(fd_, reinterpret_cast<sockaddr *>(&address), sizeof address) < 0) {
      int error = errno;
      close(fd_);
      throw std::system_error(error, std::generic_category(), "connecting to t-code");
    }
    request("hello", token.empty() ? "tapi_1" : "tapi_1 " + token);
  }

  // An already connected socket (the tests'), without a hello.
  explicit Tcode(int fd) : fd_(fd)
  {
  }

  ~Tcode()
  {
    close(fd_);
  }
  Tcode(const Tcode &) = delete;
  Tcode &operator=(const Tcode &) = delete;

  // One request, ended by a NUL (an argument may hold line breaks); the value after "ok".
  std::string request(const std::string &command, const std::string &argument = "")
  {
    std::string message = command + " " + argument;
    message += '\0';
    for (size_t sent = 0; sent < message.size();) {
      ssize_t n = send(fd_, message.data() + sent, message.size() - sent, MSG_NOSIGNAL);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        throw Error("closed", "t-code closed the connection");
      sent += static_cast<size_t>(n);
    }
    auto answer = line();
    if (!answer)
      throw Error("closed", "t-code closed the connection");
    if (*answer == "ok")
      return "";
    if (answer->rfind("ok ", 0) == 0)
      return answer->substr(3);
    std::string rest = answer->rfind("error ", 0) == 0 ? answer->substr(6) : *answer;
    size_t space = rest.find(' ');
    throw Error(rest.substr(0, space), space == std::string::npos ? "" : rest.substr(space + 1));
  }

  // get-text, get-selection: "ok", then the text up to a NUL byte.
  std::string text(const std::string &command)
  {
    request(command);
    return until('\0').value_or("");
  }

  // The next line without its line break; nullopt when t-code went away.
  std::optional<std::string> line()
  {
    return until('\n');
  }

private:
  int fd_;
  std::string buffer_;

  std::optional<std::string> until(char end)
  {
    while (true) {
      size_t at = buffer_.find(end);
      if (at != std::string::npos) {
        std::string part = buffer_.substr(0, at);
        buffer_.erase(0, at + 1);
        return part;
      }
      char chunk[65536];
      ssize_t n = read(fd_, chunk, sizeof chunk);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return std::nullopt;
      buffer_.append(chunk, static_cast<size_t>(n));
    }
  }
};

// The plugin -------------------------------------------------------------------------------------

constexpr const char *ITEM = "words";

inline std::string env(const char *name)
{
  const char *value = std::getenv(name);
  return value ? value : "";
}

// The item for the active tab: its count, or no item when it is not an editor.
inline void show(Tcode &t, const std::string &path)
{
  if (!path.empty())
    t.request("status-set", std::string(ITEM) + " " + describe(t.text("get-text")));
  else
    t.request("status-clear", ITEM);
}

inline void listen(const std::string &socket_path, const std::string &token)
{
  Tcode requests(socket_path, token);
  std::string active = requests.request("get-file"); // opened before this started: its events are gone
  show(requests, active);
  Tcode events(socket_path, token);
  events.request("subscribe");
  while (auto event = events.line()) {
    std::string kind = json_member(*event, "event"), path = json_member(*event, "path");
    if (kind == "active_changed") {
      active = path;
      show(requests, active);
    } else if ((kind == "did_change" || kind == "did_open") && !path.empty() && path == active) {
      show(requests, active);
    }
  }
}

inline void say_count(const std::string &socket_path, const std::string &token)
{
  Tcode t(socket_path, token);
  std::string selection;
  try {
    selection = t.text("get-selection");
  } catch (const Error &e) {
    if (e.code != "no-editor")
      throw;
    t.request("say", "Word Count: open a file first");
    return;
  }
  if (!selection.empty())
    t.request("say", "Word Count: " + describe(selection) + " selected");
  else
    t.request("say", "Word Count: " + describe(t.text("get-text")));
}

} // namespace word_count

#ifndef WORD_COUNT_NO_MAIN
int main(int argc, char **argv)
{
  using namespace word_count;
  std::string command = argc > 1 ? argv[1] : "", socket_path = env("TCODE_SOCKET");
  if (socket_path.empty()) {
    std::fprintf(stderr, "word-count: no $TCODE_SOCKET: t-code runs this plugin\n");
    return 1;
  }
  try {
    if (command == "start")
      listen(socket_path, env("TCODE_PLUGIN_TOKEN"));
    else
      say_count(socket_path, env("TCODE_PLUGIN_TOKEN"));
  } catch (const Error &e) {
    if (e.code == "closed")
      return 0; // t-code went away or the project closed: nothing left to do
    std::fprintf(stderr, "word-count: %s\n", e.what());
    return 1;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "word-count: %s\n", e.what());
    return 1;
  }
  return 0;
}
#endif
