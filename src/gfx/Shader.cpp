#include "gfx/Shader.h"

#include <memory>
#include <sstream>

#include "core/Log.h"

namespace patina {

Shader::~Shader() {
  if (prog_) glDeleteProgram(prog_);
}

Shader& Shader::operator=(Shader&& o) noexcept {
  if (this != &o) {
    if (prog_) glDeleteProgram(prog_);
    prog_ = o.prog_;
    o.prog_ = 0;
    locs_ = std::move(o.locs_);
  }
  return *this;
}

static bool expandIncludes(const std::string& file, std::string& out, int depth, std::string* err) {
  if (depth > 8) {
    *err = "include depth exceeded at " + file;
    return false;
  }
  auto& tab = shaders::all();
  auto it = tab.find(file);
  if (it == tab.end()) {
    *err = "unknown shader file " + file;
    return false;
  }
  std::istringstream in{std::string(it->second)};
  std::string line;
  while (std::getline(in, line)) {
    auto p = line.find("#include");
    if (p != std::string::npos && line.find_first_not_of(" \t") == p) {
      auto a = line.find('"'), b = line.rfind('"');
      if (a == std::string::npos || b <= a) {
        *err = file + ": malformed include";
        return false;
      }
      if (!expandIncludes(line.substr(a + 1, b - a - 1), out, depth + 1, err)) return false;
      continue;
    }
    out += line;
    out += '\n';
  }
  return true;
}

std::string Shader::preprocess(const std::string& file, const std::vector<std::string>& defines, std::string* err) {
  std::string body;
  std::string e;
  if (!expandIncludes(file, body, 0, &e)) {
    if (err) *err = e;
    return {};
  }
  std::string header = "#version 450 core\n";
  for (auto& d : defines) header += "#define " + d + "\n";
  // strip any #version in the body
  auto v = body.find("#version");
  if (v != std::string::npos) body.erase(v, body.find('\n', v) - v);
  return header + "#line 1\n" + body;
}

static GLuint compileStage(GLenum type, const std::string& file, const std::vector<std::string>& defines) {
  std::string err;
  std::string src = Shader::preprocess(file, defines, &err);
  if (src.empty()) {
    PLOG_E("Shader %s: %s", file.c_str(), err.c_str());
    return 0;
  }
  GLuint s = glCreateShader(type);
  const char* p = src.c_str();
  glShaderSource(s, 1, &p, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[8192];
    glGetShaderInfoLog(s, sizeof log, nullptr, log);
    PLOG_E("Compile error in %s:\n%s", file.c_str(), log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static GLuint linkProgram(std::initializer_list<GLuint> stages, const std::string& name) {
  GLuint p = glCreateProgram();
  for (GLuint s : stages) glAttachShader(p, s);
  glLinkProgram(p);
  for (GLuint s : stages) glDeleteShader(s);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[8192];
    glGetProgramInfoLog(p, sizeof log, nullptr, log);
    PLOG_E("Link error in %s:\n%s", name.c_str(), log);
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

bool Shader::loadGraphics(const std::string& vs, const std::string& fs, const std::vector<std::string>& defines) {
  GLuint v = compileStage(GL_VERTEX_SHADER, vs, defines);
  GLuint f = compileStage(GL_FRAGMENT_SHADER, fs, defines);
  if (!v || !f) {
    if (v) glDeleteShader(v);
    if (f) glDeleteShader(f);
    return false;
  }
  GLuint p = linkProgram({v, f}, vs + "+" + fs);
  if (!p) return false;
  if (prog_) glDeleteProgram(prog_);
  prog_ = p;
  locs_.clear();
  return true;
}

bool Shader::loadCompute(const std::string& cs, const std::vector<std::string>& defines) {
  GLuint c = compileStage(GL_COMPUTE_SHADER, cs, defines);
  if (!c) return false;
  GLuint p = linkProgram({c}, cs);
  if (!p) return false;
  if (prog_) glDeleteProgram(prog_);
  prog_ = p;
  locs_.clear();
  return true;
}

GLint Shader::loc(const char* name) const {
  auto it = locs_.find(name);
  if (it != locs_.end()) return it->second;
  GLint l = glGetUniformLocation(prog_, name);
  locs_.emplace(name, l);
  return l;
}

void Shader::tex(const char* n, int unit, GLuint texture, GLenum target) const {
  glActiveTexture(GL_TEXTURE0 + unit);
  glBindTexture(target, texture);
  set(n, unit);
}

namespace {
std::unordered_map<std::string, std::unique_ptr<Shader>>& cache() {
  static std::unordered_map<std::string, std::unique_ptr<Shader>> c;
  return c;
}
std::string key(const std::string& a, const std::string& b, const std::vector<std::string>& d) {
  std::string k = a + "|" + b;
  for (auto& s : d) k += "|" + s;
  return k;
}
}  // namespace

Shader& ShaderCache::get(const std::string& vs, const std::string& fs, const std::vector<std::string>& defines) {
  auto k = key(vs, fs, defines);
  auto& c = cache();
  auto it = c.find(k);
  if (it != c.end()) return *it->second;
  auto s = std::make_unique<Shader>();
  s->loadGraphics(vs, fs, defines);
  return *c.emplace(k, std::move(s)).first->second;
}

Shader& ShaderCache::compute(const std::string& cs, const std::vector<std::string>& defines) {
  auto k = key(cs, "", defines);
  auto& c = cache();
  auto it = c.find(k);
  if (it != c.end()) return *it->second;
  auto s = std::make_unique<Shader>();
  s->loadCompute(cs, defines);
  return *c.emplace(k, std::move(s)).first->second;
}

void ShaderCache::clear() { cache().clear(); }

}  // namespace patina
