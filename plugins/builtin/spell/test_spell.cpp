// The plugin's pieces, without t-code: make test builds and runs this. With hunspell and en_US
// installed, words go through hunspell for real.
#define SPELL_NO_MAIN
#include "spell.cpp"

static int failures = 0;

#define CHECK(condition)                                                                                               \
  do {                                                                                                                 \
    if (!(condition)) {                                                                                                \
      std::fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #condition);                                     \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

using namespace spell;

static std::vector<std::string> texts(const std::vector<Word> &words)
{
  std::vector<std::string> out;
  for (const Word &w : words)
    out.push_back(w.text);
  return out;
}

static void test_modes()
{
  CHECK(mode_of("/p/README.md") == Mode::MARKDOWN);
  CHECK(mode_of("/p/.git/COMMIT_EDITMSG") == Mode::PROSE);
  CHECK(mode_of("/p/src/a.c") == Mode::SLASH_COMMENTS);
  CHECK(mode_of("/p/tool.py") == Mode::HASH_COMMENTS);
  CHECK(mode_of("/p/CMakeLists.txt") == Mode::HASH_COMMENTS);
  CHECK(mode_of("/p/image.png") == Mode::NONE);
}

static void test_prose()
{
  std::string md = "Some `codde` and [a link](http://exampel.org) here\n```\nnott prose\n```\n<b>tagg</b> done\n";
  CHECK(texts(words_of(prose_of(md, Mode::MARKDOWN), md)) ==
        (std::vector<std::string>{ "Some", "and", "link", "here", "tagg", "done" })); // a tag's text is prose
  std::string c = "int x = 1; // a commment here\nchar *s = \"// nott\"; /* blockk\n end */ y = 'a';\n";
  CHECK(texts(words_of(prose_of(c, Mode::SLASH_COMMENTS), c)) ==
        (std::vector<std::string>{ "commment", "here", "blockk", "end" }));
  std::string py = "x = \"# nott\"  # reall comment\necho $# ${#arr}\n";
  CHECK(texts(words_of(prose_of(py, Mode::HASH_COMMENTS), py)) == (std::vector<std::string>{ "reall", "comment" }));
}

static void test_words()
{
  std::string text = "çok güzel “wordd”, camelCase ALLCAPS snake_case file.c v2 well-knwon don't see https://x.y/abc\n";
  std::vector<Word> words = words_of(prose_of(text, Mode::PROSE), text);
  CHECK(texts(words) == (std::vector<std::string>{ "çok", "güzel", "wordd", "well", "knwon", "don't", "see" }));
  CHECK(words[2].line == 1 && words[2].column == 12 && words[2].length == 5); // columns in characters, quotes left out
  CHECK(words[4].column == 64);
  auto [start, word] = word_at("say helo there", 7);
  CHECK(start == 5 && word == "helo");
  CHECK(word_at("say helo", 9).second == "helo"); // just after the word
  CHECK(word_at("a  b", 3).second.empty());
  CHECK(word_at("güzel olur", 3).second == "güzel");
}

static void test_dictionaries()
{
  if (!dictionary_exists("en_US"))
    return;
  CHECK(dictionaries("", "C.UTF-8") == (std::vector<std::string>{ "en_US" }));
  CHECK(dictionaries("dictionaries=en_US,xx_NOPE\n", "tr_TR.UTF-8") == (std::vector<std::string>{ "en_US" }));
  if (dictionary_exists("tr_TR"))
    CHECK(dictionaries("", "tr_TR.UTF-8") == (std::vector<std::string>{ "tr_TR", "en_US" }));
}

// hunspell for real, when it is there
static void test_hunspell()
{
  if (!dictionary_exists("en_US") || system("command -v hunspell >/dev/null") != 0) {
    std::fprintf(stderr, "test_spell: no hunspell or en_US: its part skipped\n");
    return;
  }
  auto wrong = misspelled({ "teh", "quick", "brwon", "fox" }, { "en_US" });
  CHECK(wrong && wrong->size() == 2 && wrong->count("teh") && wrong->count("brwon"));
  CHECK(wrong && !(*wrong)["brwon"].empty() && (*wrong)["brwon"][0] == "brown");
  Checker checker;
  checker.dicts = { "en_US" };
  auto items = checker.items("/p/notes.md", "The quick brwon fox.\n");
  CHECK(items && *items == "{\"range\":[1,11,1,16],\"severity\":\"info\",\"message\":\"Spelling: \xE2\x80\x9C"
                           "brwon\xE2\x80\x9D (brown)\",\"source\":\"spell\"}");
  CHECK(checker.items("/p/a.png", "brwon") == std::optional<std::string>(""));
}

int main()
{
  test_modes();
  test_prose();
  test_words();
  test_dictionaries();
  test_hunspell();
  if (failures)
    std::fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
