#include "core/I18n.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <unordered_map>

#include "core/Log.h"
#include "core/Paths.h"

namespace patina::i18n {

namespace fs = std::filesystem;

namespace {

struct SvHash {
  using is_transparent = void;
  size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
};
using StrMap = std::unordered_map<std::string, std::string, SvHash, std::equal_to<>>;
using FormsMap = std::unordered_map<std::string, std::vector<std::string>, SvHash, std::equal_to<>>;

// ---- plural expressions (the C subset gettext uses)
struct Plural {
  struct Node {
    char op;  // 'n', '#' number, '?', and the operators below (two-character ones as one letter)
    int a = -1, b = -1, c = -1;
    long v = 0;
  };
  std::vector<Node> nodes;
  int root = -1;
  int nplurals = 2;
  const char* p = nullptr;
  bool bad = false;

  void space() {
    while (*p == ' ' || *p == '\t') ++p;
  }
  int add(Node n) {
    nodes.push_back(n);
    return (int)nodes.size() - 1;
  }
  int primary() {
    space();
    if (*p == '(') {
      ++p;
      int e = ternary();
      space();
      if (*p == ')') ++p;
      else bad = true;
      return e;
    }
    if (*p == 'n') {
      ++p;
      return add({'n'});
    }
    if (std::isdigit((unsigned char)*p)) {
      long v = 0;
      while (std::isdigit((unsigned char)*p)) v = v * 10 + (*p++ - '0');
      return add({'#', -1, -1, -1, v});
    }
    bad = true;
    return add({'#'});
  }
  int unary() {
    space();
    if (*p == '!') {
      ++p;
      return add({'!', unary()});
    }
    if (*p == '-') {
      ++p;
      return add({'~', unary()});
    }
    return primary();
  }
  int binary(int level) {
    // levels: 0 ||, 1 &&, 2 == !=, 3 < <= > >=, 4 + -, 5 * / %
    if (level > 5) return unary();
    int l = binary(level + 1);
    while (true) {
      space();
      char op = 0;
      int len = 1;
      if (level == 0 && p[0] == '|' && p[1] == '|') op = 'o', len = 2;
      else if (level == 1 && p[0] == '&' && p[1] == '&') op = 'a', len = 2;
      else if (level == 2 && p[0] == '=' && p[1] == '=') op = 'e', len = 2;
      else if (level == 2 && p[0] == '!' && p[1] == '=') op = 'N', len = 2;
      else if (level == 3 && p[0] == '<' && p[1] == '=') op = 'l', len = 2;
      else if (level == 3 && p[0] == '>' && p[1] == '=') op = 'g', len = 2;
      else if (level == 3 && (p[0] == '<' || p[0] == '>')) op = p[0];
      else if (level == 4 && (p[0] == '+' || p[0] == '-')) op = p[0];
      else if (level == 5 && (p[0] == '*' || p[0] == '/' || p[0] == '%')) op = p[0];
      if (!op) return l;
      p += len;
      l = add({op, l, binary(level + 1)});
    }
  }
  int ternary() {
    int c = binary(0);
    space();
    if (*p != '?') return c;
    ++p;
    int a = ternary();
    space();
    if (*p == ':') ++p;
    else bad = true;
    int b = ternary();
    return add({'?', c, a, b});
  }
  bool compile(const std::string& forms) {
    nodes.clear(), root = -1, bad = false, nplurals = 2;
    size_t np = forms.find("nplurals");
    if (np != std::string::npos) {
      size_t eq = forms.find('=', np);
      if (eq != std::string::npos) nplurals = std::clamp(std::atoi(forms.c_str() + eq + 1), 1, 16);
    }
    size_t pl = forms.find("plural", np == std::string::npos ? 0 : np + 8);
    if (pl == std::string::npos) return false;
    size_t eq = forms.find('=', pl);
    if (eq == std::string::npos) return false;
    std::string expr = forms.substr(eq + 1);
    size_t semi = expr.find(';');
    if (semi != std::string::npos) expr.resize(semi);
    p = expr.c_str();
    root = ternary();
    space();
    if (*p) bad = true;
    p = nullptr;
    return !bad;
  }
  long eval(int i, long n) const {
    const Node& x = nodes[i];
    switch (x.op) {
      case 'n': return n;
      case '#': return x.v;
      case '!': return !eval(x.a, n);
      case '~': return -eval(x.a, n);
      case '?': return eval(x.a, n) ? eval(x.b, n) : eval(x.c, n);
      case 'o': return eval(x.a, n) || eval(x.b, n);
      case 'a': return eval(x.a, n) && eval(x.b, n);
      case 'e': return eval(x.a, n) == eval(x.b, n);
      case 'N': return eval(x.a, n) != eval(x.b, n);
      case 'l': return eval(x.a, n) <= eval(x.b, n);
      case 'g': return eval(x.a, n) >= eval(x.b, n);
      case '<': return eval(x.a, n) < eval(x.b, n);
      case '>': return eval(x.a, n) > eval(x.b, n);
      case '+': return eval(x.a, n) + eval(x.b, n);
      case '-': return eval(x.a, n) - eval(x.b, n);
      case '*': return eval(x.a, n) * eval(x.b, n);
      case '/': {
        long d = eval(x.b, n);
        return d ? eval(x.a, n) / d : 0;
      }
      case '%': {
        long d = eval(x.b, n);
        return d ? eval(x.a, n) % d : 0;
      }
    }
    return 0;
  }
  int index(long n) const {
    if (root < 0 || bad) return n == 1 ? 0 : 1;
    long v = eval(root, n);
    return (int)std::clamp(v, 0L, (long)nplurals - 1);
  }
};

// ---- the state
struct Catalog {
  StrMap strings;   // key: msgid, or msgctxt \x04 msgid
  FormsMap plurals; // key: msgid (with its context)
  StrMap labels;    // ImGui labels made from the strings (cache)
  StrMap lists;     // combo lists (cache)
  Plural plural;
  bool pseudo = false;
};

std::string g_code = "en";
std::unique_ptr<Catalog> g_cat;                  // null: English
std::vector<std::unique_ptr<Catalog>> g_old;     // earlier catalogs stay alive: their strings may still be pointed at
Stats g_stats;
uint64_t g_version = 1;
std::set<std::string, std::less<>> g_seen;       // untranslated, shown this session

const char* remember(const char* en) {
  if (g_seen.size() < 20000 && *en && g_seen.find(std::string_view(en)) == g_seen.end()) g_seen.emplace(en);
  return en;
}

std::string unescape(std::string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c != '\\' || i + 1 >= s.size()) {
      out += c;
      continue;
    }
    char e = s[++i];
    switch (e) {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case 'a': out += '\a'; break;
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '0': out += '\0'; break;
      default: out += '\\', out += e;
    }
  }
  return out;
}

std::string escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if (c == '\n') out += "\\n";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}

// the % conversions of a format string ("d", "zu", "*", ".2f" is "f"); "$" (positional) marks it unusable
std::vector<std::string> conversions(const char* s, bool* positional) {
  std::vector<std::string> out;
  for (const char* p = s; *p; ++p) {
    if (*p != '%') continue;
    ++p;
    if (*p == '%') continue;
    if (!*p) break;
    const char* q = p;
    while (std::isdigit((unsigned char)*q)) ++q;
    if (*q == '$') *positional = true;
    while (*p && std::strchr("-+ #0'", *p)) ++p;
    if (*p == '*') out.push_back("*"), ++p;
    while (std::isdigit((unsigned char)*p)) ++p;
    if (*p == '.') {
      ++p;
      if (*p == '*') out.push_back("*"), ++p;
      while (std::isdigit((unsigned char)*p)) ++p;
    }
    std::string len;
    while (*p && std::strchr("hlLqjzt", *p)) len += *p++;
    if (!*p) break;
    char conv = *p;
    if (conv == 'i') conv = 'd';
    if (conv == 'F' || conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G' || conv == 'a' || conv == 'A') conv = 'f';
    if (conv == 'X' || conv == 'o') conv = 'x';
    out.push_back(len + conv);
  }
  return out;
}

Catalog* cat() { return g_cat.get(); }

std::string keyOf(const char* context, const char* en) { return std::string(context) + '\x04' + en; }

// the translation of `key`, or nullptr (the test language makes one for everything)
const char* lookup(std::string_view key, const char* en) {
  Catalog* c = cat();
  if (!c) return nullptr;
  auto it = c->strings.find(key);
  if (it != c->strings.end()) return it->second.c_str();
  if (c->pseudo) return c->strings.emplace(std::string(key), pseudo(en)).first->second.c_str();
  return nullptr;
}

void pushUtf8(std::string& out, unsigned cp) {
  if (cp < 0x80) out += char(cp);
  else if (cp < 0x800) out += char(0xC0 | cp >> 6), out += char(0x80 | (cp & 63));
  else out += char(0xE0 | cp >> 12), out += char(0x80 | (cp >> 6 & 63)), out += char(0x80 | (cp & 63));
}

const char* pluralDefault(const std::string& code) {
  std::string lang = code.substr(0, code.find('_'));
  static const std::map<std::string, const char*> table = {
      {"ja", "nplurals=1; plural=0;"}, {"zh", "nplurals=1; plural=0;"}, {"ko", "nplurals=1; plural=0;"},
      {"vi", "nplurals=1; plural=0;"}, {"th", "nplurals=1; plural=0;"}, {"id", "nplurals=1; plural=0;"},
      {"tr", "nplurals=2; plural=(n != 1);"},
      {"fr", "nplurals=2; plural=(n > 1);"}, {"pt_BR", "nplurals=2; plural=(n > 1);"},
      {"ru", "nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);"},
      {"uk", "nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);"},
      {"pl", "nplurals=3; plural=(n==1 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);"},
      {"cs", "nplurals=3; plural=(n==1) ? 0 : (n>=2 && n<=4) ? 1 : 2;"},
      {"sk", "nplurals=3; plural=(n==1) ? 0 : (n>=2 && n<=4) ? 1 : 2;"},
  };
  auto it = table.find(code);
  if (it == table.end()) it = table.find(lang);
  return it != table.end() ? it->second : "nplurals=2; plural=(n != 1);";
}

}  // namespace

// ---------------------------------------------------------------- lookups

const char* tr(const char* en) {
  if (!g_cat || !en || !*en) return en;
  const char* t = lookup(en, en);
  return t ? t : remember(en);
}

const char* trc(const char* context, const char* en) {
  if (!g_cat || !en || !*en) return en;
  const char* t = lookup(keyOf(context, en), en);
  return t ? t : remember(en);
}

const char* trn(const char* en, const char* enPlural, long n) {
  if (!g_cat) return n == 1 ? en : enPlural;
  if (g_cat->pseudo) return lookup(n == 1 ? en : enPlural, n == 1 ? en : enPlural);
  auto it = g_cat->plurals.find(std::string_view(en));
  if (it != g_cat->plurals.end()) {
    int k = g_cat->plural.index(n);
    if (k < (int)it->second.size() && !it->second[k].empty()) return it->second[k].c_str();
  }
  remember(en);
  return n == 1 ? en : enPlural;
}

const char* label(const char* en) {
  if (!g_cat || !en || !*en) return en;
  auto hit = g_cat->labels.find(std::string_view(en));
  if (hit != g_cat->labels.end()) return hit->second.c_str();
  const char* hash = std::strstr(en, "##");
  std::string vis = hash ? std::string(en, hash) : std::string(en);
  if (vis.empty()) return en;
  const char* t = lookup(vis, vis.c_str());
  if (!t) {
    remember(vis.c_str());
    return en;  // (no translation: the label as it is)
  }
  std::string id = hash ? (std::strncmp(hash, "###", 3) == 0 ? std::string(hash + 3) : vis + hash) : vis;
  return g_cat->labels.emplace(en, std::string(t) + "###" + id).first->second.c_str();
}

const char* list(const char* en) {
  if (!g_cat || !en || !*en) return en;
  size_t len = 0;
  while (en[len] || en[len + 1]) ++len;  // up to the "\0\0" at the end
  std::string_view key(en, len + 1);
  auto hit = g_cat->lists.find(key);
  if (hit != g_cat->lists.end()) return hit->second.c_str();
  std::string out;
  for (const char* p = en; *p; p += std::strlen(p) + 1) {
    out += tr(p);
    out += '\0';
  }
  auto r = g_cat->lists.emplace(std::string(key), out);  // (std::string keeps a 0 after its last item: the list's end)
  return r.first->second.c_str();
}

std::string format(const char* en, ...) {
  const char* f = tr(en);
  va_list ap, ap2;
  va_start(ap, en);
  va_copy(ap2, ap);
  int n = std::vsnprintf(nullptr, 0, f, ap);
  std::string out(n > 0 ? size_t(n) : 0, '\0');
  if (n > 0) std::vsnprintf(out.data(), out.size() + 1, f, ap2);
  va_end(ap2);
  va_end(ap);
  return out;
}

// ---------------------------------------------------------------- the test language

std::string pseudo(const char* en) {
  // letters with accents that the default (Latin-1) font has; % codes, \n and anything after "##" stay as they are
  static const char* kMap[128] = {};
  static bool init = false;
  if (!init) {
    const char* pairs[][2] = {{"a", "å"}, {"e", "é"}, {"i", "ï"}, {"o", "ö"}, {"u", "ü"}, {"c", "ç"}, {"n", "ñ"},
                              {"y", "ý"}, {"A", "Å"}, {"E", "É"}, {"I", "Ï"}, {"O", "Ö"}, {"U", "Ü"}, {"C", "Ç"},
                              {"N", "Ñ"}, {"Y", "Ý"}, {"D", "Ð"}};
    for (auto& p : pairs) kMap[(unsigned char)p[0][0]] = p[1];
    init = true;
  }
  std::string out = "[";
  int letters = 0;
  for (const char* p = en; *p; ++p) {
    if (p[0] == '#' && p[1] == '#') {
      out += "]";
      out += p;
      return out;
    }
    if (*p == '%') {
      const char* q = p + 1;
      if (*q == '%') {
        out += "%%";
        p = q;
        continue;
      }
      while (*q && std::strchr("-+ #0'*.0123456789hlLqjzt", *q)) ++q;
      if (*q) ++q;
      out.append(p, q);
      p = q - 1;
      continue;
    }
    unsigned char c = (unsigned char)*p;
    if (c < 128 && kMap[c]) out += kMap[c];
    else out += char(c);
    letters += std::isalpha(c) ? 1 : 0;
  }
  for (int k = 0; k < std::max(1, letters / 3); ++k) pushUtf8(out, 0xB7);  // a third longer: '·'
  out += "]";
  return out;
}

// ---------------------------------------------------------------- .po files

bool formatsMatch(const char* en, const char* translated) {
  bool pe = false, pt = false;
  auto a = conversions(en, &pe), b = conversions(translated, &pt);
  if (pt) return false;
  if (b.size() > a.size()) return false;
  // the translation may leave out codes at the end (printf ignores arguments it isn't asked for), not change them
  return std::equal(b.begin(), b.end(), a.begin());
}

int pluralIndex(const std::string& pluralForms, long n) {
  Plural p;
  if (!p.compile(pluralForms)) return -1;
  return p.index(n);
}

bool parsePo(const std::string& textIn, PoFile& out, std::string* err) {
  out = PoFile();
  std::string_view text(textIn);
  if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
    text.remove_prefix(3);
  PoEntry cur;
  bool haveId = false, haveStr = false, nextFuzzy = false;
  std::string* target = nullptr;
  std::vector<PoEntry> all;
  auto flush = [&] {
    if (haveId) all.push_back(cur);
    cur = PoEntry();
    haveId = haveStr = false;
    target = nullptr;
  };
  auto quoted = [&](std::string_view s, size_t lineNo, std::string& into) {
    size_t a = s.find('"'), b = s.rfind('"');
    if (a == std::string_view::npos || b == a) {
      if (err) *err = "line " + std::to_string(lineNo) + ": a text in quotes was expected";
      return false;
    }
    into += unescape(s.substr(a + 1, b - a - 1));
    return true;
  };
  size_t lineNo = 0, pos = 0;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
    ++lineNo;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    if (line.empty()) {
      if (haveStr) flush();
      continue;
    }
    if (line.rfind("#~", 0) == 0) continue;  // obsolete entries
    if (line[0] == '#') {
      if (line.rfind("#,", 0) == 0 && line.find("fuzzy") != std::string_view::npos) {
        if (haveStr) flush();
        nextFuzzy = true;
      }
      continue;
    }
    auto starts = [&](const char* kw) {
      size_t n = std::strlen(kw);
      return line.size() > n && line.compare(0, n, kw) == 0 && (line[n] == ' ' || line[n] == '\t' || line[n] == '[');
    };
    if (starts("msgctxt")) {
      if (haveId || haveStr) flush();
      cur.fuzzy = nextFuzzy, nextFuzzy = false;
      if (!quoted(line.substr(7), lineNo, cur.context)) return false;
      target = &cur.context;
    } else if (starts("msgid_plural")) {
      if (!quoted(line.substr(12), lineNo, cur.idPlural)) return false;
      target = &cur.idPlural;
    } else if (starts("msgid")) {
      if (haveStr) flush();
      if (!haveId && cur.context.empty()) cur.fuzzy = nextFuzzy, nextFuzzy = false;
      haveId = true;
      if (!quoted(line.substr(5), lineNo, cur.id)) return false;
      target = &cur.id;
    } else if (starts("msgstr")) {
      if (!haveId) {
        if (err) *err = "line " + std::to_string(lineNo) + ": msgstr without a msgid";
        return false;
      }
      size_t k = 0;
      std::string_view rest = line.substr(6);
      if (!rest.empty() && rest[0] == '[') {
        k = (size_t)std::atoi(std::string(rest.substr(1)).c_str());
        size_t close = rest.find(']');
        rest = close == std::string_view::npos ? std::string_view() : rest.substr(close + 1);
      }
      if (k > 16) {
        if (err) *err = "line " + std::to_string(lineNo) + ": too many plural forms";
        return false;
      }
      if (cur.str.size() <= k) cur.str.resize(k + 1);
      if (!quoted(rest, lineNo, cur.str[k])) return false;
      target = &cur.str[k];
      haveStr = true;
    } else if (line[0] == '"') {
      if (!target) {
        if (err) *err = "line " + std::to_string(lineNo) + ": text without a msgid or msgstr before it";
        return false;
      }
      if (!quoted(line, lineNo, *target)) return false;
    } else {
      if (err) *err = "line " + std::to_string(lineNo) + ": not understood: " + std::string(line.substr(0, 40));
      return false;
    }
  }
  flush();
  for (auto& e : all) {
    if (e.id.empty() && e.context.empty()) {  // the header
      std::string h = e.str.empty() ? std::string() : e.str[0];
      size_t p = 0;
      while (p < h.size()) {
        size_t nl = h.find('\n', p);
        std::string l = h.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
        p = nl == std::string::npos ? h.size() : nl + 1;
        size_t c = l.find(':');
        if (c == std::string::npos) continue;
        std::string k = l.substr(0, c), v = l.substr(c + 1);
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        if (k == "Language") out.language = v;
        else if (k == "X-Patina-Language-Name") out.languageName = v;
        else if (k == "Plural-Forms") out.pluralForms = v;
        else if (k == "Last-Translator") out.author = v;
        else if (k == "X-Patina-Font") out.font = v;
        else if (k == "Content-Type") {
          size_t cs = v.find("charset=");
          if (cs != std::string::npos) out.charset = v.substr(cs + 8);
        }
      }
      continue;
    }
    out.entries.push_back(std::move(e));
  }
  return true;
}

// ---------------------------------------------------------------- languages

std::string userDir() { return (paths::userDataDir() / "languages").string(); }
std::string shippedDir() { return (paths::dataDir() / "languages").string(); }
std::string templatePath() { return (paths::dataDir() / "languages" / "patina.pot").string(); }

std::vector<Language> available() {
  std::vector<Language> out = {{"en", "English", "", false}};
  std::map<std::string, Language> files;
  for (int pass = 0; pass < 2; ++pass) {
    std::error_code ec;
    fs::path dir = pass ? fs::path(userDir()) : fs::path(shippedDir());
    for (auto& f : fs::directory_iterator(dir, ec)) {
      if (f.path().extension() != ".po") continue;
      Language l;
      l.code = f.path().stem().string();
      l.path = f.path().string();
      l.user = pass == 1;
      l.name = l.code;
      bool ok = false;
      std::string text = paths::readFile(f.path(), &ok);
      PoFile po;
      if (ok && parsePo(text, po, nullptr) && !po.languageName.empty()) l.name = po.languageName;
      files[l.code] = l;  // (the user's own file wins)
    }
  }
  for (auto& [code, l] : files)
    if (code != "en") out.push_back(l);
  out.push_back({"x-test", "Test language (accents and brackets)", "", false});
  return out;
}

const std::string& current() { return g_code; }
uint64_t version() { return g_version; }
const Stats& stats() { return g_stats; }

static bool load(const std::string& code, std::string* err) {
  if (code.empty() || code == "en") {
    if (g_cat) g_old.push_back(std::move(g_cat));
    g_code = "en";
    g_stats = Stats();
    ++g_version;
    return true;
  }
  auto c = std::make_unique<Catalog>();
  Stats st;
  if (code == "x-test") {
    c->pseudo = true;
  } else {
    std::string path;
    for (auto& l : available())
      if (l.code == code) path = l.path;
    if (path.empty()) {
      if (err) *err = "no translation file " + code + ".po in " + userDir() + " or " + shippedDir();
      return false;
    }
    bool ok = false;
    std::string text = paths::readFile(path, &ok);
    PoFile po;
    std::string perr;
    if (!ok || !parsePo(text, po, &perr)) {
      if (err) *err = fs::path(path).filename().string() + ": " + (ok ? perr : std::string("can't be read"));
      return false;
    }
    std::string cs = po.charset;
    std::transform(cs.begin(), cs.end(), cs.begin(), ::tolower);
    if (!cs.empty() && cs != "utf-8" && cs != "utf8") {
      if (err) *err = fs::path(path).filename().string() + " is saved as " + po.charset + ": save it as UTF-8";
      return false;
    }
    c->plural.compile(po.pluralForms.empty() ? pluralDefault(code) : po.pluralForms);
    st.author = po.author;
    if (!po.font.empty()) {
      fs::path f = po.font;
      if (f.is_relative()) f = fs::path(path).parent_path() / f;
      st.font = f.string();
    }
    for (auto& e : po.entries) {
      ++st.entries;
      if (e.fuzzy) {
        ++st.fuzzy;
        continue;
      }
      bool any = false;
      for (auto& s : e.str) any |= !s.empty();
      if (!any) continue;
      std::string key = e.context.empty() ? e.id : e.context + '\x04' + e.id;
      if (!e.idPlural.empty()) {
        bool good = true;
        for (size_t k = 0; k < e.str.size(); ++k)
          good &= e.str[k].empty() || formatsMatch(e.idPlural.c_str(), e.str[k].c_str()) || formatsMatch(e.id.c_str(), e.str[k].c_str());
        if (!good) {
          st.rejected.push_back(e.id);
          continue;
        }
        c->plurals[key] = e.str;
        if (!e.str[0].empty()) c->strings[key] = e.str[0];
      } else {
        if (!formatsMatch(e.id.c_str(), e.str[0].c_str())) {
          st.rejected.push_back(e.id);
          continue;
        }
        c->strings[key] = e.str[0];
      }
      ++st.translated;
      for (auto& s : e.str) st.glyphs += s;
    }
    if (!st.rejected.empty())
      PLOG_W("%s.po: %zu translations not used: their %% codes differ from the English (\"%s\"%s)", code.c_str(),
             st.rejected.size(), st.rejected[0].c_str(), st.rejected.size() > 1 ? ", ..." : "");
    PLOG_I("Language: %s, %d of %d strings translated", code.c_str(), st.translated, st.entries);
  }
  if (g_cat) g_old.push_back(std::move(g_cat));
  g_cat = std::move(c);
  g_code = code;
  g_stats = st;
  g_seen.clear();
  ++g_version;
  return true;
}

bool setLanguage(const std::string& code, std::string* err) {
  if (code == g_code) return true;
  return load(code, err);
}

bool reload(std::string* err) {
  std::string code = g_code;
  g_code = "";
  if (load(code, err)) return true;
  g_code = code;
  return false;
}

bool makeTranslation(const std::string& codeIn, const std::string& name, std::string* path, std::string* err) {
  std::string code = codeIn;
  bool good = code.size() >= 2 && code.size() <= 12;
  for (char ch : code) good &= std::isalnum((unsigned char)ch) || ch == '_' || ch == '-';
  if (!good || code == "en" || code == "x-test") {
    if (err) *err = "a language code is two or three letters, with a region if needed: fr, de, pt_BR, zh_CN";
    return false;
  }
  fs::path dst = paths::ensureDir(fs::path(userDir())) / (code + ".po");
  if (path) *path = dst.string();
  std::error_code ec;
  if (fs::exists(dst, ec)) return true;  // (already there: never overwritten)
  bool ok = false;
  std::string t = paths::readFile(templatePath(), &ok);
  if (!ok) {
    if (err) *err = "the template " + templatePath() + " is missing";
    return false;
  }
  auto setField = [&](const std::string& key, const std::string& value) {
    std::string k = "\"" + key + ":";
    size_t p = t.find(k);
    std::string line = "\"" + key + ": " + escape(value) + "\\n\"";
    if (p != std::string::npos) {
      size_t e = t.find('\n', p);
      t.replace(p, (e == std::string::npos ? t.size() : e) - p, line);
    } else {
      size_t h = t.find("msgstr \"\"\n");
      if (h != std::string::npos) t.insert(h + 10, line + "\n");
    }
  };
  setField("Language", code);
  setField("X-Patina-Language-Name", name.empty() ? code : name);
  setField("Plural-Forms", pluralDefault(code));
  // (a .po is the template with the msgstr lines to fill in; Poedit takes either)
  if (!paths::writeFileAtomic(dst, t)) {
    if (err) *err = "can't write " + dst.string();
    return false;
  }
  return true;
}

std::vector<std::string> untranslatedSeen() { return std::vector<std::string>(g_seen.begin(), g_seen.end()); }

bool writeUntranslated(const std::string& path, std::string* err) {
  std::string t = "# The text Patina showed without a translation (" + g_code + ")\n"
                  "msgid \"\"\nmsgstr \"\"\n\"Content-Type: text/plain; charset=UTF-8\\n\"\n\"Language: " + g_code + "\\n\"\n\n";
  for (auto& s : g_seen) t += "msgid \"" + escape(s) + "\"\nmsgstr \"\"\n\n";
  if (!paths::writeFileAtomic(path, t)) {
    if (err) *err = "can't write " + path;
    return false;
  }
  return true;
}

}  // namespace patina::i18n
