#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Math.h"
#include "gfx/GL.h"

namespace patina {

// A linked GLSL program. Sources come from the embedded shader table and may
// use `#include "file.glsl"`. Defines are injected after the #version line.
class Shader {
 public:
  Shader() = default;
  ~Shader();
  Shader(const Shader&) = delete;
  Shader& operator=(const Shader&) = delete;
  Shader(Shader&& o) noexcept { *this = std::move(o); }
  Shader& operator=(Shader&& o) noexcept;

  bool loadGraphics(const std::string& vs, const std::string& fs, const std::vector<std::string>& defines = {});
  bool loadCompute(const std::string& cs, const std::vector<std::string>& defines = {});

  void use() const { glUseProgram(prog_); }
  GLuint id() const { return prog_; }
  bool valid() const { return prog_ != 0; }

  GLint loc(const char* name) const;
  void set(const char* n, int v) const { glProgramUniform1i(prog_, loc(n), v); }
  void set(const char* n, float v) const { glProgramUniform1f(prog_, loc(n), v); }
  void set(const char* n, uint32_t v) const { glProgramUniform1ui(prog_, loc(n), v); }
  void set(const char* n, bool v) const { glProgramUniform1i(prog_, loc(n), v ? 1 : 0); }
  void set(const char* n, const vec2& v) const { glProgramUniform2fv(prog_, loc(n), 1, &v.x); }
  void set(const char* n, const vec3& v) const { glProgramUniform3fv(prog_, loc(n), 1, &v.x); }
  void set(const char* n, const vec4& v) const { glProgramUniform4fv(prog_, loc(n), 1, &v.x); }
  void set(const char* n, const ivec2& v) const { glProgramUniform2iv(prog_, loc(n), 1, &v.x); }
  void set(const char* n, const mat3& m) const { glProgramUniformMatrix3fv(prog_, loc(n), 1, GL_FALSE, &m[0][0]); }
  void set(const char* n, const mat4& m) const { glProgramUniformMatrix4fv(prog_, loc(n), 1, GL_FALSE, &m[0][0]); }
  void setArray(const char* n, const vec4* v, int count) const { glProgramUniform4fv(prog_, loc(n), count, &v[0].x); }
  // Bind texture to unit and set sampler uniform
  void tex(const char* n, int unit, GLuint texture, GLenum target = GL_TEXTURE_2D) const;

  static std::string preprocess(const std::string& file, const std::vector<std::string>& defines, std::string* err);

 private:
  GLuint prog_ = 0;
  mutable std::unordered_map<std::string, GLint> locs_;
};

// Process-wide cache of compiled programs keyed by "vs|fs|defines".
class ShaderCache {
 public:
  static Shader& get(const std::string& vs, const std::string& fs, const std::vector<std::string>& defines = {});
  static Shader& compute(const std::string& cs, const std::vector<std::string>& defines = {});
  static void clear();
};

namespace shaders {
const std::unordered_map<std::string, std::string_view>& all();
}

}  // namespace patina
