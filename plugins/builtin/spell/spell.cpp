// Spell check for t-code, in C++17 with the standard library and POSIX only; the checking itself is
// hunspell's ("hunspell -a").
//
// Started with the project ($1 = "start"), it checks a file when it opens and when it is saved:
// all of a Markdown, text or reST file (code blocks, inline code, links and tags left out), only
// the comments of a source file. A word hunspell does not know becomes an "info" diagnostic with
// its suggestions. camelCase, ALLCAPS, words with digits or underscores, paths and URLs are left
// alone.
//
//   fix    the word at the cursor: its suggestions in the picker, the chosen one goes in
//   add    adds the word at the cursor to your dictionary (~/.config/t-code-spell/words)
//   check  checks the active file now, unsaved text included
//
// Dictionaries: ~/.config/t-code-spell/settings ("dictionaries=en_US,tr_TR"), else the language of
// $LANG with en_US. It speaks tapi_1 over $TCODE_SOCKET (t-code's docs/plugins.md); run.sh builds it.

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace spell
{

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

// A JSON string.
inline std::string quote(const std::string &text)
{
  std::string out = "\"";
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c == '\n') {
      out += "\\n";
    } else if (c < 0x20) {
      char escaped[8];
      std::snprintf(escaped, sizeof escaped, "\\u%04x", c);
      out += escaped;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + "\"";
}

// Which parts of a file are prose ------------------------------------------------------------------

enum class Mode { NONE, PROSE, MARKDOWN, SLASH_COMMENTS, HASH_COMMENTS };

inline std::string lower(std::string s)
{
  for (char &c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

inline Mode mode_of(const std::string &path)
{
  std::string base = path.substr(path.find_last_of('/') + 1);
  size_t dot = base.find_last_of('.');
  std::string ext = dot == std::string::npos ? "" : lower(base.substr(dot));
  if (base == "CMakeLists.txt" || base == "Makefile" || base == "makefile" || base == "Dockerfile")
    return Mode::HASH_COMMENTS; // before the extensions: CMakeLists.txt is no text
  if (base == "COMMIT_EDITMSG" || ext == ".txt" || ext == ".rst" || ext == ".adoc")
    return Mode::PROSE;
  if (ext == ".md" || ext == ".markdown")
    return Mode::MARKDOWN;
  for (const char *e :
       { ".c",   ".h",  ".cc", ".cpp", ".cxx",   ".hpp", ".hh",    ".java", ".js",  ".mjs", ".ts",  ".tsx",
         ".jsx", ".go", ".rs", ".cs",  ".swift", ".kt",  ".scala", ".dart", ".zig", ".css", ".scss" })
    if (ext == e)
      return Mode::SLASH_COMMENTS;
  for (const char *e :
       { ".py", ".sh", ".bash", ".zsh", ".rb", ".pl", ".pm", ".yaml", ".yml", ".toml", ".cmake", ".r", ".jl" })
    if (ext == e)
      return Mode::HASH_COMMENTS;
  return Mode::NONE;
}

inline void blank(std::string &out, size_t from, size_t to)
{
  for (size_t i = from; i < to && i < out.size(); i++)
    if (out[i] != '\n')
      out[i] = ' ';
}

// URLs ("http://...", "www...." up to a space) blanked in out, where text has them.
inline void blank_urls(const std::string &text, std::string &out)
{
  for (const char *start : { "http://", "https://", "www.", "file://", "mailto:" }) {
    for (size_t at = text.find(start); at != std::string::npos; at = text.find(start, at + 1)) {
      size_t end = at;
      while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])))
        end++;
      blank(out, at, end);
    }
  }
}

// text with what is not prose blanked (line breaks and byte offsets kept, so columns stay right).
inline std::string prose_of(const std::string &text, Mode mode)
{
  std::string out = text;
  if (mode == Mode::NONE)
    return std::string(text.size(), ' ');
  if (mode == Mode::MARKDOWN) {
    bool fenced = false;
    size_t line_start = 0;
    while (line_start <= text.size()) {
      size_t line_end = text.find('\n', line_start);
      if (line_end == std::string::npos)
        line_end = text.size();
      std::string line = text.substr(line_start, line_end - line_start);
      size_t first = line.find_first_not_of(" \t");
      bool fence =
          first != std::string::npos && (line.compare(first, 3, "```") == 0 || line.compare(first, 3, "~~~") == 0);
      if (fence || fenced) {
        blank(out, line_start, line_end);
        if (fence)
          fenced = !fenced;
      } else {
        // `code`, <tags> and <autolinks>, and a link's target: "](...)"
        for (size_t i = 0; i < line.size(); i++) {
          char closing = line[i] == '`' ? '`' : line[i] == '<' ? '>' : 0;
          if (line[i] == ']' && i + 1 < line.size() && line[i + 1] == '(') {
            closing = ')';
            i++;
          }
          if (!closing)
            continue;
          size_t end = line.find(closing, i + 1);
          if (end == std::string::npos)
            break;
          blank(out, line_start + i, line_start + end + 1);
          i = end;
        }
      }
      line_start = line_end + 1;
    }
  } else if (mode == Mode::SLASH_COMMENTS) {
    // keep only // and /* */ comments; strings are skipped so a "//" in one is no comment
    enum { CODE, LINE, BLOCK, STRING } state = CODE;
    std::string keep(text.size(), 0);
    for (size_t i = 0; i < text.size(); i++) {
      char c = text[i], next = i + 1 < text.size() ? text[i + 1] : 0;
      if (state == CODE) {
        if (c == '/' && next == '/')
          state = LINE, i++;
        else if (c == '/' && next == '*')
          state = BLOCK, i++;
        else if (c == '"')
          state = STRING;
        else if (c == '\'' && (next == '\\' || (i + 2 < text.size() && text[i + 2] == '\'')))
          i += next == '\\' ? 3 : 2; // a character literal, not a Rust lifetime
      } else if (state == LINE) {
        if (c == '\n')
          state = CODE;
        else
          keep[i] = 1;
      } else if (state == BLOCK) {
        if (c == '*' && next == '/')
          state = CODE, i++;
        else
          keep[i] = 1;
      } else if (c == '\\') {
        i++;
      } else if (c == '"' || c == '\n') {
        state = CODE;
      }
    }
    for (size_t i = 0; i < text.size(); i++)
      if (!keep[i] && out[i] != '\n')
        out[i] = ' ';
  } else if (mode == Mode::HASH_COMMENTS) {
    size_t line_start = 0;
    while (line_start < text.size()) {
      size_t line_end = text.find('\n', line_start);
      if (line_end == std::string::npos)
        line_end = text.size();
      size_t comment = line_end;
      char in_string = 0;
      for (size_t i = line_start; i < line_end; i++) {
        char c = text[i];
        if (in_string) {
          if (c == '\\')
            i++;
          else if (c == in_string)
            in_string = 0;
        } else if (c == '"' || c == '\'') {
          in_string = c;
        } else if (c == '#' && (i == line_start || (text[i - 1] != '$' && text[i - 1] != '{'))) {
          comment = i + 1;
          break;
        }
      }
      blank(out, line_start, comment);
      line_start = line_end + 1;
    }
  }
  blank_urls(text, out);
  return out;
}

// Words ---------------------------------------------------------------------------------------------

struct Word {
  long line, column; // 1-based, the column in characters
  long length;       // in characters
  std::string text;
};

inline bool is_letter_byte(unsigned char c)
{
  return std::isalpha(c) || c >= 0x80;
}

inline long chars(const std::string &s, size_t from, size_t to)
{
  long n = 0;
  for (size_t i = from; i < to && i < s.size(); i++)
    if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80)
      n++;
  return n;
}

// Typographic quotes and dashes around a word, in UTF-8.
inline size_t punctuation_at(const std::string &s, size_t at)
{
  for (const char *mark : { "\xE2\x80\x9C", "\xE2\x80\x9D", "\xE2\x80\x98", "\xE2\x80\x99", "\xC2\xAB", "\xC2\xBB",
                            "\xE2\x80\x94", "\xE2\x80\x93", "\xE2\x80\xA6" })
    if (s.compare(at, std::strlen(mark), mark) == 0)
      return std::strlen(mark);
  unsigned char c = static_cast<unsigned char>(s[at]);
  return c < 0x80 && !std::isalpha(c) && !std::isdigit(c) && c != '_' ? 1 : 0;
}

// Whether part (a hyphen-free piece of a word) is worth asking hunspell about.
inline bool checkable(const std::string &part)
{
  if (chars(part, 0, part.size()) < 3)
    return false;
  for (size_t i = 0; i < part.size(); i++) {
    unsigned char c = static_cast<unsigned char>(part[i]);
    if (i > 0 && std::isupper(c))
      return false; // camelCase, ALLCAPS, McName
    if (!is_letter_byte(c) && c != '\'')
      return false;
  }
  return true;
}

// The words of the prose (prose_of's text), with places from the original text.
inline std::vector<Word> words_of(const std::string &prose, const std::string &text)
{
  std::vector<Word> words;
  long line = 1;
  size_t line_start = 0;
  for (size_t i = 0; i < prose.size();) {
    if (prose[i] == '\n') {
      line++, line_start = ++i;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(prose[i]))) {
      i++;
      continue;
    }
    size_t end = i;
    while (end < prose.size() && !std::isspace(static_cast<unsigned char>(prose[end])))
      end++;
    size_t from = i, to = end;
    for (size_t n; from < to && (n = punctuation_at(prose, from)) > 0;)
      from += n;
    while (to > from) { // trailing punctuation: step back over one character at a time
      size_t last = to - 1;
      while (last > from && (static_cast<unsigned char>(prose[last]) & 0xC0) == 0x80)
        last--;
      if (punctuation_at(prose, last) != to - last)
        break;
      to = last;
    }
    std::string token = prose.substr(from, to - from);
    bool plain = !token.empty();
    for (unsigned char c : token)
      if (!is_letter_byte(c) && c != '\'' && c != '-')
        plain = false; // a path, a number, an identifier, a.b
    for (size_t at = 0; plain && at <= token.size();) {
      size_t hyphen = token.find('-', at);
      if (hyphen == std::string::npos)
        hyphen = token.size();
      std::string part = token.substr(at, hyphen - at);
      while (!part.empty() && part.back() == '\'')
        part.pop_back();
      if (checkable(part))
        words.push_back({ line, chars(text, line_start, from + at) + 1, chars(part, 0, part.size()), part });
      at = hyphen + 1;
    }
    i = end;
  }
  return words;
}

// hunspell -------------------------------------------------------------------------------------------

inline std::string env(const char *name)
{
  const char *value = std::getenv(name);
  return value ? value : "";
}

inline std::string config_dir()
{
  std::string base = env("XDG_CONFIG_HOME");
  return (base.empty() ? env("HOME") + "/.config" : base) + "/t-code-spell";
}

inline std::string read_file(const std::string &path)
{
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

inline bool dictionary_exists(const std::string &name)
{
  for (const std::string &dir :
       std::vector<std::string>{ "/usr/share/hunspell", "/usr/share/myspell", "/usr/share/myspell/dicts",
                                 env("HOME") + "/.local/share/hunspell" })
    if (access((dir + "/" + name + ".dic").c_str(), R_OK) == 0)
      return true;
  return false;
}

// "dictionaries=..." from the settings, else $LANG's language and en_US, those installed.
inline std::vector<std::string> dictionaries(const std::string &settings, const std::string &lang)
{
  std::vector<std::string> wanted;
  std::istringstream lines(settings);
  for (std::string line; std::getline(lines, line);) {
    if (line.rfind("dictionaries=", 0) == 0) {
      std::istringstream names(line.substr(13));
      for (std::string name; std::getline(names, name, ',');)
        if (!name.empty())
          wanted.push_back(name);
    }
  }
  if (wanted.empty()) {
    std::string language = lang.substr(0, lang.find_first_of(".@"));
    if (language.size() >= 2 && language != "C" && language != "POSIX" && language != "en_US")
      wanted.push_back(language);
    wanted.push_back("en_US");
  }
  std::vector<std::string> found;
  for (const std::string &name : wanted)
    if (dictionary_exists(name))
      found.push_back(name);
  return found;
}

inline std::set<std::string> personal_words()
{
  std::set<std::string> words;
  std::istringstream lines(read_file(config_dir() + "/words"));
  for (std::string line; std::getline(lines, line);)
    if (!line.empty())
      words.insert(line);
  return words;
}

// hunspell -a's answer for words (one per line, each line's answer then an empty line): the ones
// it does not know, with up to five suggestions. nullopt when hunspell cannot run.
inline std::optional<std::map<std::string, std::vector<std::string>>> misspelled(const std::set<std::string> &words,
                                                                                 const std::vector<std::string> &dicts)
{
  std::map<std::string, std::vector<std::string>> wrong;
  if (words.empty())
    return wrong;
  char input[] = "/tmp/t-code-spell-XXXXXX";
  int in = mkstemp(input);
  if (in < 0)
    return std::nullopt;
  std::string lines;
  for (const std::string &w : words)
    lines += "^" + w + "\n"; // "^": the line is text, never one of -a's commands
  bool written = write(in, lines.data(), lines.size()) == static_cast<ssize_t>(lines.size());
  lseek(in, 0, SEEK_SET);
  unlink(input);
  int out[2];
  if (!written || pipe(out) < 0) {
    close(in);
    return std::nullopt;
  }
  std::string dict_list;
  for (const std::string &d : dicts)
    dict_list += (dict_list.empty() ? "" : ",") + d;
  pid_t child = fork();
  if (child == 0) {
    dup2(in, 0);
    dup2(out[1], 1);
    int null = open("/dev/null", O_WRONLY);
    dup2(null, 2);
    close(out[0]);
    execlp("hunspell", "hunspell", "-a", "-i", "utf-8", "-d", dict_list.c_str(), static_cast<char *>(nullptr));
    _exit(127);
  }
  close(in);
  close(out[1]);
  std::string answer;
  char chunk[65536];
  for (ssize_t n; (n = read(out[0], chunk, sizeof chunk)) != 0;) {
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0)
      break;
    answer.append(chunk, static_cast<size_t>(n));
  }
  close(out[0]);
  int status = 0;
  waitpid(child, &status, 0);
  if (child < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return std::nullopt;
  std::istringstream reply(answer);
  std::string line;
  std::getline(reply, line); // "@(#) International Ispell Version ..."
  for (const std::string &w : words) {
    std::string result;
    while (std::getline(reply, line) && !line.empty())
      if (result.empty())
        result = line;
    if (result.empty() || (result[0] != '&' && result[0] != '#'))
      continue;
    std::vector<std::string> suggestions;
    size_t colon = result.find(": ");
    if (result[0] == '&' && colon != std::string::npos) {
      std::istringstream list(result.substr(colon + 2));
      for (std::string s; std::getline(list, s, ',') && suggestions.size() < 5;) {
        size_t first = s.find_first_not_of(' ');
        if (first != std::string::npos)
          suggestions.push_back(s.substr(first));
      }
    }
    wrong[w] = suggestions;
  }
  return wrong;
}

// The plugin -------------------------------------------------------------------------------------

struct Checker {
  std::vector<std::string> dicts = dictionaries(read_file(config_dir() + "/settings"), env("LANG"));
  bool told = false; // said once that hunspell or a dictionary is missing

  // The diagnostics items (JSON) for text of path; nullopt when hunspell cannot run.
  std::optional<std::string> items(const std::string &path, const std::string &text)
  {
    Mode mode = mode_of(path);
    if (mode == Mode::NONE || text.size() > 2 * 1024 * 1024)
      return std::string();
    std::vector<Word> words = words_of(prose_of(text, mode), text);
    if (words.size() > 20000)
      words.resize(20000);
    std::set<std::string> unique, mine = personal_words();
    for (const Word &w : words)
      if (!mine.count(w.text))
        unique.insert(w.text);
    auto wrong = misspelled(unique, dicts);
    if (!wrong)
      return std::nullopt;
    std::string out;
    for (const Word &w : words) {
      auto found = wrong->find(w.text);
      if (found == wrong->end())
        continue;
      std::string message = "Spelling: \xE2\x80\x9C" + w.text + "\xE2\x80\x9D";
      for (size_t i = 0; i < found->second.size(); i++)
        message += (i == 0 ? " (" : ", ") + found->second[i] + (i + 1 == found->second.size() ? ")" : "");
      out += (out.empty() ? "" : ",") + std::string("{\"range\":[") + std::to_string(w.line) + "," +
             std::to_string(w.column) + "," + std::to_string(w.line) + "," + std::to_string(w.column + w.length) +
             "],\"severity\":\"info\",\"message\":" + quote(message) + ",\"source\":\"spell\"}";
    }
    return out;
  }

  // Checks text of path and sends its diagnostics; false when hunspell cannot run.
  bool check(Tcode &t, const std::string &path, const std::string &text)
  {
    std::optional<std::string> found = dicts.empty() ? std::nullopt : items(path, text);
    if (!found) {
      if (!told && mode_of(path) != Mode::NONE)
        t.request("say", dicts.empty() ? "Spell: no hunspell dictionary found (install hunspell-en or your language's)"
                                       : "Spell: install hunspell to check spelling");
      told = true;
      return false;
    }
    t.request("diagnostics-set", "{\"path\":" + quote(path) + ",\"items\":[" + *found + "]}");
    return true;
  }
};

inline void listen(const std::string &socket_path, const std::string &token)
{
  Checker checker;
  Tcode requests(socket_path, token);
  std::string active = requests.request("get-file"); // opened before this started: its did_open is gone
  if (!active.empty())
    checker.check(requests, active, read_file(active));
  Tcode events(socket_path, token);
  events.request("subscribe");
  while (auto event = events.line()) {
    std::string kind = json_member(*event, "event"), path = json_member(*event, "path");
    if (path.empty())
      continue;
    if ((kind == "did_open" || kind == "did_save") && mode_of(path) != Mode::NONE)
      checker.check(requests, path, read_file(path));
    else if (kind == "did_close")
      requests.request("diagnostics-clear", path);
  }
}

// The word around column (1-based, characters) of line: (its start column, the word); empty when
// the cursor is not on one.
inline std::pair<long, std::string> word_at(const std::string &line, long column)
{
  std::vector<size_t> starts; // byte offset of each character
  for (size_t i = 0; i < line.size(); i++)
    if ((static_cast<unsigned char>(line[i]) & 0xC0) != 0x80)
      starts.push_back(i);
  starts.push_back(line.size());
  auto letter = [&](size_t index) {
    if (index + 1 >= starts.size())
      return false;
    unsigned char c = static_cast<unsigned char>(line[starts[index]]);
    return is_letter_byte(c) && punctuation_at(line, starts[index]) == 0;
  };
  long at = std::max(0L, column - 1);
  if (at >= static_cast<long>(starts.size()) - 1 || !letter(static_cast<size_t>(at))) {
    if (at > 0 && letter(static_cast<size_t>(at - 1)))
      at--; // just after a word
    else
      return { 0, "" };
  }
  long from = at, to = at;
  while (from > 0 && letter(static_cast<size_t>(from - 1)))
    from--;
  while (letter(static_cast<size_t>(to + 1)))
    to++;
  return { from + 1, line.substr(starts[static_cast<size_t>(from)],
                                 starts[static_cast<size_t>(to + 1)] - starts[static_cast<size_t>(from)]) };
}

// The cursor's line, its word and where it starts; the status bar says why when there is none.
inline bool cursor_word(Tcode &t, long &line_number, long &start, std::string &word)
{
  std::string cursor = t.request("get-cursor");
  long column = 1;
  if (std::sscanf(cursor.c_str(), "%ld %ld", &line_number, &column) != 2)
    return false;
  std::istringstream text(t.text("get-text"));
  std::string line;
  for (long n = 0; n < line_number && std::getline(text, line); n++) {
  }
  std::tie(start, word) = word_at(line, column);
  if (word.empty()) {
    t.request("say", "Spell: put the cursor on a word first");
    return false;
  }
  return true;
}

inline void add_word(Tcode &t, const std::string &word)
{
  std::string dir = config_dir();
  std::string parent = dir.substr(0, dir.find_last_of('/'));
  mkdir(parent.c_str(), 0755);
  mkdir(dir.c_str(), 0755);
  std::ofstream(dir + "/words", std::ios::app) << word << "\n";
  Checker checker;
  std::string path = t.request("get-file");
  checker.check(t, path, t.text("get-text"));
  t.request("say", "Spell: \xE2\x80\x9C" + word + "\xE2\x80\x9D added to your dictionary");
}

inline void fix(const std::string &socket_path, const std::string &token, bool add_only)
{
  Tcode t(socket_path, token);
  long line = 0, start = 0;
  std::string word;
  try {
    if (!cursor_word(t, line, start, word))
      return;
  } catch (const Error &e) {
    if (e.code != "no-editor")
      throw;
    t.request("say", "Spell: open a file first");
    return;
  }
  if (add_only)
    return add_word(t, word);
  Checker checker;
  auto wrong = misspelled({ word }, checker.dicts);
  if (!wrong) {
    t.request("say", "Spell: install hunspell and a dictionary to check spelling");
    return;
  }
  if (!wrong->count(word) || personal_words().count(word)) {
    t.request("say", "Spell: \xE2\x80\x9C" + word + "\xE2\x80\x9D looks right");
    return;
  }
  const std::vector<std::string> &suggestions = (*wrong)[word];
  std::string items;
  for (const std::string &s : suggestions)
    items += quote(s) + ",";
  items += "{\"label\":" + quote("Add \xE2\x80\x9C" + word + "\xE2\x80\x9D to my dictionary") + "}";
  std::string answer;
  try {
    answer = t.request("pick", "{\"title\":" + quote("Spelling of " + word) + ",\"items\":[" + items + "]}");
  } catch (const Error &e) {
    if (e.code == "canceled")
      return;
    throw;
  }
  size_t index = std::strtoul(answer.c_str(), nullptr, 10);
  if (index >= suggestions.size())
    return add_word(t, word);
  long length = static_cast<long>(chars(word, 0, word.size()));
  t.request("edit", "[{\"range\":[" + std::to_string(line) + "," + std::to_string(start) + "," + std::to_string(line) +
                        "," + std::to_string(start + length) + "],\"text\":" + quote(suggestions[index]) + "}]");
}

inline void check_now(const std::string &socket_path, const std::string &token)
{
  Tcode t(socket_path, token);
  std::string path = t.request("get-file");
  if (path.empty() || mode_of(path) == Mode::NONE) {
    t.request("say", "Spell: this file has no prose to check (Markdown, text, or a source file's comments)");
    return;
  }
  Checker checker;
  if (checker.check(t, path, t.text("get-text")))
    t.request("say", "Spell: checked; anything odd is underlined");
}

} // namespace spell

#ifndef SPELL_NO_MAIN
int main(int argc, char **argv)
{
  using namespace spell;
  std::string command = argc > 1 ? argv[1] : "", socket_path = env("TCODE_SOCKET"), token = env("TCODE_PLUGIN_TOKEN");
  if (socket_path.empty()) {
    std::fprintf(stderr, "spell: no $TCODE_SOCKET: t-code runs this plugin\n");
    return 1;
  }
  try {
    if (command == "start")
      listen(socket_path, token);
    else if (command == "fix" || command == "add")
      fix(socket_path, token, command == "add");
    else if (command == "check")
      check_now(socket_path, token);
    else
      throw std::runtime_error("unknown command \"" + command + "\"");
  } catch (const Error &e) {
    if (e.code == "closed")
      return 0;
    std::fprintf(stderr, "spell: %s: %s\n", command.c_str(), e.what());
    return 1;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "spell: %s: %s\n", command.c_str(), e.what());
    return 1;
  }
  return 0;
}
#endif
