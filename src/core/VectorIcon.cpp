#include "core/VectorIcon.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

namespace patina::vicon {

namespace {

constexpr uint32_t kAccentRGB = 0xE8852C;
constexpr float kStep = 0.45f;  // flattening: icon units per segment (at most about a pixel at 48 px)

struct Affine {  // x' = a*x + c*y + e, y' = b*x + d*y + f
  float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  vec2 apply(vec2 p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
  Affine operator*(const Affine& o) const {
    return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d, b * o.c + d * o.d, a * o.e + c * o.f + e, b * o.e + d * o.f + f};
  }
  float scale() const { return std::sqrt(std::abs(a * d - b * c)); }
};

struct Style {
  Paint fill = Paint::Literal, stroke = Paint::None;
  uint32_t fillRGB = 0, strokeRGB = 0;
  float strokeWidth = 1.f, strokeAlpha = 1.f, fillAlpha = 1.f, alpha = 1.f;
  std::vector<float> dash;
  Affine xf;
};

struct Subpath {
  std::vector<vec2> pts;
  bool closed = false;
};

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ','; }

// numbers in path data and lists: "1.5.3" is 1.5 then .3, "-1-2" is -1 then -2
struct Numbers {
  std::string_view s;
  size_t i = 0;
  void skip() {
    while (i < s.size() && isSpace(s[i])) ++i;
  }
  bool atNumber() {
    skip();
    if (i >= s.size()) return false;
    char c = s[i];
    return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.';
  }
  bool number(float& out) {
    if (!atNumber()) return false;
    size_t j = i;
    if (s[j] == '-' || s[j] == '+') ++j;
    bool dot = false;
    while (j < s.size() && ((s[j] >= '0' && s[j] <= '9') || (s[j] == '.' && !dot))) dot |= s[j] == '.', ++j;
    if (j < s.size() && (s[j] == 'e' || s[j] == 'E')) {
      size_t k = j + 1;
      if (k < s.size() && (s[k] == '-' || s[k] == '+')) ++k;
      if (k < s.size() && s[k] >= '0' && s[k] <= '9') {
        j = k;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
      }
    }
    out = std::strtof(std::string(s.substr(i, j - i)).c_str(), nullptr);
    i = j;
    return true;
  }
  bool flag(bool& out) {  // arc flags may be written without separators ("a1 1 0 01 2 2")
    skip();
    if (i < s.size() && (s[i] == '0' || s[i] == '1')) {
      out = s[i++] == '1';
      return true;
    }
    return false;
  }
};

std::vector<float> numberList(std::string_view s) {
  Numbers n{s};
  std::vector<float> v;
  float x;
  while (n.number(x)) v.push_back(x);
  return v;
}

void addCubic(std::vector<vec2>& pts, vec2 p0, vec2 p1, vec2 p2, vec2 p3) {
  float len = glm::length(p1 - p0) + glm::length(p2 - p1) + glm::length(p3 - p2);
  int n = std::clamp(int(std::ceil(len / kStep)), 3, 48);
  for (int k = 1; k <= n; ++k) {
    float t = float(k) / n, u = 1.f - t;
    pts.push_back(u * u * u * p0 + 3.f * u * u * t * p1 + 3.f * u * t * t * p2 + t * t * t * p3);
  }
}

// SVG arc (endpoint form) to points, after the SVG spec's conversion to center form
void addArc(std::vector<vec2>& pts, vec2 p0, float rx, float ry, float phiDeg, bool large, bool sweep, vec2 p1) {
  if (glm::length(p1 - p0) < 1e-6f) return;
  rx = std::abs(rx), ry = std::abs(ry);
  if (rx < 1e-6f || ry < 1e-6f) {
    pts.push_back(p1);
    return;
  }
  float phi = glm::radians(phiDeg), cs = std::cos(phi), sn = std::sin(phi);
  vec2 h = (p0 - p1) * 0.5f;
  vec2 q(cs * h.x + sn * h.y, -sn * h.x + cs * h.y);
  float lam = q.x * q.x / (rx * rx) + q.y * q.y / (ry * ry);
  if (lam > 1.f) rx *= std::sqrt(lam), ry *= std::sqrt(lam);
  float num = rx * rx * ry * ry - rx * rx * q.y * q.y - ry * ry * q.x * q.x;
  float den = rx * rx * q.y * q.y + ry * ry * q.x * q.x;
  float k = std::sqrt(std::max(0.f, num / std::max(den, 1e-12f))) * (large == sweep ? -1.f : 1.f);
  vec2 cq(k * rx * q.y / ry, -k * ry * q.x / rx);
  vec2 c(cs * cq.x - sn * cq.y + (p0.x + p1.x) * 0.5f, sn * cq.x + cs * cq.y + (p0.y + p1.y) * 0.5f);
  auto ang = [](vec2 u, vec2 v) {
    float a = std::atan2(u.x * v.y - u.y * v.x, u.x * v.x + u.y * v.y);
    return a;
  };
  vec2 u((q.x - cq.x) / rx, (q.y - cq.y) / ry), v((-q.x - cq.x) / rx, (-q.y - cq.y) / ry);
  float t0 = std::atan2(u.y, u.x), dt = ang(u, v);
  if (!sweep && dt > 0) dt -= 2.f * kPi;
  if (sweep && dt < 0) dt += 2.f * kPi;
  int n = std::clamp(int(std::ceil(std::abs(dt) * std::max(rx, ry) / kStep)), 2, 96);
  for (int i = 1; i <= n; ++i) {
    float t = t0 + dt * float(i) / n;
    vec2 e(rx * std::cos(t), ry * std::sin(t));
    pts.push_back(i == n ? p1 : vec2(cs * e.x - sn * e.y + c.x, sn * e.x + cs * e.y + c.y));
  }
}

bool parsePath(std::string_view d, std::vector<Subpath>& out, std::string& err) {
  Numbers n{d};
  vec2 cur(0), start(0), lastCtrl(0);
  char cmd = 0, prev = 0;
  Subpath sp;
  auto finish = [&] {
    if (sp.pts.size() >= 2) out.push_back(sp);
    sp = {};
  };
  while (true) {
    n.skip();
    if (n.i >= d.size()) break;
    char c = d[n.i];
    if (std::isalpha((unsigned char)c)) cmd = c, ++n.i;
    else if (!cmd) return err = "path data starts without a command", false;
    char C = (char)std::toupper((unsigned char)cmd);
    bool rel = cmd != C;
    vec2 base = rel ? cur : vec2(0);
    auto pt = [&](vec2& p) {
      float x, y;
      if (!n.number(x) || !n.number(y)) return false;
      p = vec2(x, y) + base;
      return true;
    };
    if (C == 'Z') {
      if (!sp.pts.empty()) {
        sp.closed = true;
        finish();
      }
      cur = start;
      prev = 'Z';
      cmd = 0;
      continue;
    }
    if (C == 'M') {
      vec2 p;
      if (!pt(p)) return err = "bad M", false;
      finish();
      sp.pts.push_back(p);
      cur = start = p;
      cmd = rel ? 'l' : 'L';  // more pairs after a moveto are linetos
      prev = 'M';
      continue;
    }
    if (sp.pts.empty()) sp.pts.push_back(cur);  // drawing on after a Z
    vec2 p, c1, c2;
    float x, rx, ry, rot;
    bool large, sweep;
    switch (C) {
      case 'L':
        if (!pt(p)) return err = "bad L", false;
        sp.pts.push_back(p), cur = p;
        break;
      case 'H':
        if (!n.number(x)) return err = "bad H", false;
        cur = vec2(rel ? cur.x + x : x, cur.y), sp.pts.push_back(cur);
        break;
      case 'V':
        if (!n.number(x)) return err = "bad V", false;
        cur = vec2(cur.x, rel ? cur.y + x : x), sp.pts.push_back(cur);
        break;
      case 'C':
        if (!pt(c1) || !pt(c2) || !pt(p)) return err = "bad C", false;
        addCubic(sp.pts, cur, c1, c2, p), lastCtrl = c2, cur = p;
        break;
      case 'S':
        c1 = (prev == 'C' || prev == 'S') ? 2.f * cur - lastCtrl : cur;
        if (!pt(c2) || !pt(p)) return err = "bad S", false;
        addCubic(sp.pts, cur, c1, c2, p), lastCtrl = c2, cur = p;
        break;
      case 'Q':
        if (!pt(c1) || !pt(p)) return err = "bad Q", false;
        addCubic(sp.pts, cur, cur + (c1 - cur) * (2.f / 3.f), p + (c1 - p) * (2.f / 3.f), p), lastCtrl = c1, cur = p;
        break;
      case 'T':
        c1 = (prev == 'Q' || prev == 'T') ? 2.f * cur - lastCtrl : cur;
        if (!pt(p)) return err = "bad T", false;
        addCubic(sp.pts, cur, cur + (c1 - cur) * (2.f / 3.f), p + (c1 - p) * (2.f / 3.f), p), lastCtrl = c1, cur = p;
        break;
      case 'A':
        if (!n.number(rx) || !n.number(ry) || !n.number(rot) || !n.flag(large) || !n.flag(sweep) || !pt(p))
          return err = "bad A", false;
        addArc(sp.pts, cur, rx, ry, rot, large, sweep, p), cur = p;
        break;
      default:
        return err = std::string("path command '") + cmd + "' is not supported", false;
    }
    prev = C;
  }
  finish();
  return true;
}

Subpath ellipse(vec2 c, float rx, float ry) {
  Subpath s;
  int n = std::clamp(int(std::ceil(2.f * kPi * std::max(rx, ry) / kStep)), 12, 128);
  for (int i = 0; i < n; ++i) {
    float t = 2.f * kPi * i / n;
    s.pts.push_back(c + vec2(rx * std::cos(t), ry * std::sin(t)));
  }
  s.closed = true;
  return s;
}

Subpath roundRect(float x, float y, float w, float h, float rx, float ry) {
  rx = std::min(rx, w * 0.5f), ry = std::min(ry, h * 0.5f);
  Subpath s;
  s.closed = true;
  if (rx <= 0 || ry <= 0) {
    s.pts = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    return s;
  }
  auto corner = [&](vec2 c, float a0) {
    int n = std::clamp(int(std::ceil(0.5f * kPi * std::max(rx, ry) / kStep)), 2, 24);
    for (int i = 0; i <= n; ++i) {
      float t = glm::radians(a0) + 0.5f * kPi * i / n;
      s.pts.push_back(c + vec2(rx * std::cos(t), ry * std::sin(t)));
    }
  };
  corner({x + w - rx, y + ry}, -90), corner({x + w - rx, y + h - ry}, 0), corner({x + rx, y + h - ry}, 90),
      corner({x + rx, y + ry}, 180);
  return s;
}

bool parsePaint(std::string_view v, Paint& p, uint32_t& rgb) {
  while (!v.empty() && isSpace(v.front())) v.remove_prefix(1);
  while (!v.empty() && isSpace(v.back())) v.remove_suffix(1);
  if (v == "none") return p = Paint::None, true;
  if (v == "currentColor") return p = Paint::Current, true;
  if (v.size() == 7 && v[0] == '#') rgb = (uint32_t)std::strtoul(std::string(v.substr(1)).c_str(), nullptr, 16);
  else if (v.size() == 4 && v[0] == '#') {
    uint32_t s = (uint32_t)std::strtoul(std::string(v.substr(1)).c_str(), nullptr, 16);
    rgb = ((s >> 8 & 15) * 17) << 16 | ((s >> 4 & 15) * 17) << 8 | (s & 15) * 17;
  } else if (v == "black") rgb = 0;
  else if (v == "white") rgb = 0xFFFFFF;
  else return false;
  p = rgb == kAccentRGB ? Paint::Accent : Paint::Literal;
  return true;
}

bool parseTransform(std::string_view s, Affine& out, std::string& err) {
  Affine m;
  size_t i = 0;
  while (true) {
    while (i < s.size() && isSpace(s[i])) ++i;
    if (i >= s.size()) break;
    size_t open = s.find('(', i), close = s.find(')', i);
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) return err = "bad transform", false;
    std::string_view fn = s.substr(i, open - i);
    while (!fn.empty() && isSpace(fn.back())) fn.remove_suffix(1);
    auto a = numberList(s.substr(open + 1, close - open - 1));
    Affine t;
    if (fn == "translate" && !a.empty()) t.e = a[0], t.f = a.size() > 1 ? a[1] : 0.f;
    else if (fn == "scale" && !a.empty()) t.a = a[0], t.d = a.size() > 1 ? a[1] : a[0];
    else if (fn == "rotate" && !a.empty()) {
      float r = glm::radians(a[0]), cs = std::cos(r), sn = std::sin(r);
      Affine rot{cs, sn, -sn, cs, 0, 0};
      if (a.size() >= 3) {
        Affine to{1, 0, 0, 1, a[1], a[2]}, back{1, 0, 0, 1, -a[1], -a[2]};
        t = to * rot * back;
      } else {
        t = rot;
      }
    } else if (fn == "matrix" && a.size() == 6) {
      t = {a[0], a[1], a[2], a[3], a[4], a[5]};
    } else {
      return err = "transform '" + std::string(fn) + "' is not supported", false;
    }
    m = m * t;
    i = close + 1;
  }
  out = m;
  return true;
}

float cross(vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }

bool convex(const std::vector<vec2>& p) {
  int sign = 0;
  for (size_t i = 0; i < p.size(); ++i) {
    float c = cross(p[(i + 1) % p.size()] - p[i], p[(i + 2) % p.size()] - p[(i + 1) % p.size()]);
    if (std::abs(c) < 1e-5f) continue;
    int s = c > 0 ? 1 : -1;
    if (sign && s != sign) return false;
    sign = s;
  }
  return true;
}

// ear clipping (outlines of a few hundred points at most)
std::vector<uint16_t> triangulate(const std::vector<vec2>& p) {
  std::vector<uint16_t> out, idx;
  if (p.size() < 3) return out;
  float area = 0;
  for (size_t i = 0; i < p.size(); ++i) area += cross(p[i], p[(i + 1) % p.size()]);
  for (size_t i = 0; i < p.size(); ++i) idx.push_back(uint16_t(area > 0 ? i : p.size() - 1 - i));  // counterclockwise
  auto inside = [](vec2 q, vec2 a, vec2 b, vec2 c) {
    return cross(b - a, q - a) >= 0 && cross(c - b, q - b) >= 0 && cross(a - c, q - c) >= 0;
  };
  size_t guard = 0;
  while (idx.size() > 3 && guard++ < 100000) {
    bool clipped = false;
    for (size_t i = 0; i < idx.size(); ++i) {
      size_t ia = idx[(i + idx.size() - 1) % idx.size()], ib = idx[i], ic = idx[(i + 1) % idx.size()];
      vec2 a = p[ia], b = p[ib], c = p[ic];
      if (cross(b - a, c - b) <= 1e-7f) continue;  // reflex or flat
      bool ear = true;
      for (uint16_t j : idx)
        if (j != ia && j != ib && j != ic && inside(p[j], a, b, c)) {
          ear = false;
          break;
        }
      if (!ear) continue;
      out.insert(out.end(), {uint16_t(ia), uint16_t(ib), uint16_t(ic)});
      idx.erase(idx.begin() + long(i));
      clipped = true;
      break;
    }
    if (!clipped) break;  // (a self-touching outline: give up on the rest)
  }
  if (idx.size() == 3) out.insert(out.end(), {idx[0], idx[1], idx[2]});
  return out;
}

void cleanPoints(std::vector<vec2>& p, bool closed) {
  std::vector<vec2> q;
  for (vec2 v : p)
    if (q.empty() || glm::length(v - q.back()) > 1e-4f) q.push_back(v);
  if (closed && q.size() > 2 && glm::length(q.front() - q.back()) < 1e-4f) q.pop_back();
  p.swap(q);
}

// a stroked subpath: pieces broken at corners (each drawn with round ends), or dashes
void strokePieces(Shape& sh, std::vector<vec2> p, bool closed, const std::vector<float>& dash) {
  cleanPoints(p, closed);
  if (p.size() < 2) {
    if (p.size() == 1) sh.joints.push_back(p[0]);
    return;
  }
  if (!dash.empty()) {
    std::vector<float> pat = dash;
    if (pat.size() % 2) pat.insert(pat.end(), dash.begin(), dash.end());
    std::vector<vec2> path = p;
    if (closed) path.push_back(p.front());
    size_t k = 0;
    float left = std::max(pat[0], 1e-3f);
    bool on = true;
    std::vector<vec2> cur = {path[0]};
    for (size_t i = 0; i + 1 < path.size(); ++i) {
      vec2 a = path[i], b = path[i + 1];
      float seg = glm::length(b - a), t = 0;
      while (seg - t > left) {
        t += left;
        vec2 q = a + (b - a) * (t / seg);
        if (on) {
          cur.push_back(q);
          sh.pieces.push_back(cur), sh.pieceClosed.push_back(0);
          sh.joints.push_back(cur.front()), sh.joints.push_back(cur.back());
        }
        cur = {q};
        on = !on;
        k = (k + 1) % pat.size();
        left = std::max(pat[k], 1e-3f);
      }
      left -= seg - t;
      if (on) cur.push_back(b);
    }
    if (on && cur.size() >= 2) {
      sh.pieces.push_back(cur), sh.pieceClosed.push_back(0);
      sh.joints.push_back(cur.front()), sh.joints.push_back(cur.back());
    }
    return;
  }
  const float kCos = std::cos(glm::radians(35.f));
  size_t n = p.size();
  auto corner = [&](size_t i) {
    vec2 a = p[i] - p[(i + n - 1) % n], b = p[(i + 1) % n] - p[i];
    float la = glm::length(a), lb = glm::length(b);
    return la > 1e-6f && lb > 1e-6f && glm::dot(a, b) / (la * lb) < kCos;
  };
  std::vector<size_t> corners;
  for (size_t i = closed ? 0 : 1; i < (closed ? n : n - 1); ++i)
    if (corner(i)) corners.push_back(i);
  if (closed && corners.empty()) {
    sh.pieces.push_back(p), sh.pieceClosed.push_back(1);
    return;
  }
  std::vector<size_t> cuts;  // piece boundaries as indices along the walk
  if (closed) {
    // walk from the first corner round to it again
    std::vector<vec2> walk;
    for (size_t k = 0; k <= n; ++k) walk.push_back(p[(corners[0] + k) % n]);
    std::vector<uint8_t> isCorner(n, 0);
    for (size_t c : corners) isCorner[c] = 1;
    std::vector<vec2> cur = {walk[0]};
    for (size_t k = 1; k < walk.size(); ++k) {
      cur.push_back(walk[k]);
      if (isCorner[(corners[0] + k) % n] || k + 1 == walk.size()) {
        sh.pieces.push_back(cur), sh.pieceClosed.push_back(0);
        sh.joints.push_back(cur.back());
        cur = {walk[k]};
      }
    }
    return;
  }
  std::vector<vec2> cur = {p[0]};
  sh.joints.push_back(p[0]);
  size_t ci = 0;
  for (size_t i = 1; i < n; ++i) {
    cur.push_back(p[i]);
    bool cut = ci < corners.size() && corners[ci] == i;
    if (cut || i + 1 == n) {
      if (cut) ++ci;
      sh.pieces.push_back(cur), sh.pieceClosed.push_back(0);
      sh.joints.push_back(p[i]);
      cur = {p[i]};
    }
  }
}

struct Attr {
  std::string_view name, value;
};

}  // namespace

Icon parse(std::string_view svg) {
  Icon icon;
  std::vector<Style> stack(1);
  stack[0].fill = Paint::Literal;  // SVG's default: black fill, no stroke
  size_t i = 0;
  auto problem = [&](std::string s) {
    if (std::find(icon.problems.begin(), icon.problems.end(), s) == icon.problems.end()) icon.problems.push_back(std::move(s));
  };
  while ((i = svg.find('<', i)) != std::string_view::npos) {
    if (svg.compare(i, 4, "<!--") == 0) {
      size_t e = svg.find("-->", i);
      i = e == std::string_view::npos ? svg.size() : e + 3;
      continue;
    }
    if (i + 1 < svg.size() && (svg[i + 1] == '?' || svg[i + 1] == '!')) {
      i = svg.find('>', i);
      if (i == std::string_view::npos) break;
      continue;
    }
    size_t end = svg.find('>', i);
    if (end == std::string_view::npos) break;
    std::string_view tag = svg.substr(i + 1, end - i - 1);
    i = end + 1;
    if (!tag.empty() && tag[0] == '/') {
      std::string_view name = tag.substr(1);
      while (!name.empty() && isSpace(name.back())) name.remove_suffix(1);
      if (name == "g" && stack.size() > 1) stack.pop_back();
      continue;
    }
    bool selfClose = !tag.empty() && tag.back() == '/';
    if (selfClose) tag.remove_suffix(1);
    size_t k = 0;
    while (k < tag.size() && !isSpace(tag[k])) ++k;
    std::string_view name = tag.substr(0, k);
    std::vector<Attr> attrs;
    while (k < tag.size()) {
      while (k < tag.size() && isSpace(tag[k])) ++k;
      size_t a = k;
      while (k < tag.size() && tag[k] != '=' && !isSpace(tag[k])) ++k;
      std::string_view an = tag.substr(a, k - a);
      while (k < tag.size() && isSpace(tag[k])) ++k;
      if (k >= tag.size() || tag[k] != '=') {
        if (!an.empty()) problem("attribute '" + std::string(an) + "' has no value");
        break;
      }
      ++k;
      while (k < tag.size() && isSpace(tag[k])) ++k;
      if (k >= tag.size() || (tag[k] != '"' && tag[k] != '\'')) break;
      char q = tag[k++];
      size_t vb = k;
      while (k < tag.size() && tag[k] != q) ++k;
      attrs.push_back({an, tag.substr(vb, k - vb)});
      ++k;
    }
    Style st = stack.back();
    std::map<std::string_view, std::string_view> geo;
    for (auto& [an, av] : attrs) {
      std::string err;
      if (an == "fill") {
        if (!parsePaint(av, st.fill, st.fillRGB)) problem("fill '" + std::string(av) + "'");
      } else if (an == "stroke") {
        if (!parsePaint(av, st.stroke, st.strokeRGB)) problem("stroke '" + std::string(av) + "'");
      } else if (an == "stroke-width") {
        st.strokeWidth = std::strtof(std::string(av).c_str(), nullptr);
      } else if (an == "stroke-opacity") {
        st.strokeAlpha = std::strtof(std::string(av).c_str(), nullptr);
      } else if (an == "fill-opacity") {
        st.fillAlpha = std::strtof(std::string(av).c_str(), nullptr);
      } else if (an == "opacity") {
        st.alpha *= std::strtof(std::string(av).c_str(), nullptr);
      } else if (an == "stroke-dasharray") {
        st.dash = av == "none" ? std::vector<float>{} : numberList(av);
      } else if (an == "transform") {
        Affine t;
        if (parseTransform(av, t, err)) st.xf = st.xf * t;
        else problem(err);
      } else if (an == "stroke-linecap" || an == "stroke-linejoin") {
        if (av != "round") problem(std::string(an) + " '" + std::string(av) + "' (icons use round)");
      } else if (an == "xmlns" || an == "viewBox" || an == "width" || an == "height" || an == "class" || an == "id" ||
                 an == "aria-hidden" || an == "x" || an == "y" || an == "cx" || an == "cy" || an == "r" || an == "rx" ||
                 an == "ry" || an == "x1" || an == "y1" || an == "x2" || an == "y2" || an == "d" || an == "points") {
        geo[an] = av;
      } else {
        problem("attribute '" + std::string(an) + "'");
      }
    }
    if (name == "svg") {
      if (geo.count("viewBox") && numberList(geo["viewBox"]) != std::vector<float>{0, 0, 24, 24}) problem("viewBox is not 0 0 24 24");
      geo.erase("width"), geo.erase("height");
      stack[0] = st;
      continue;
    }
    if (name == "g") {
      if (!selfClose) stack.push_back(st);
      continue;
    }
    auto num = [&](const char* key, float def = 0.f) {
      auto it = geo.find(key);
      return it == geo.end() ? def : std::strtof(std::string(it->second).c_str(), nullptr);
    };
    std::vector<Subpath> subs;
    if (name == "path") {
      std::string err;
      if (!parsePath(geo["d"], subs, err)) problem(err);
    } else if (name == "circle") {
      subs.push_back(ellipse({num("cx"), num("cy")}, num("r"), num("r")));
    } else if (name == "ellipse") {
      subs.push_back(ellipse({num("cx"), num("cy")}, num("rx"), num("ry")));
    } else if (name == "rect") {
      float rx = num("rx", -1), ry = num("ry", -1);
      if (rx < 0) rx = ry < 0 ? 0 : ry;
      if (ry < 0) ry = rx;
      subs.push_back(roundRect(num("x"), num("y"), num("width"), num("height"), rx, ry));
    } else if (name == "line") {
      subs.push_back({{{num("x1"), num("y1")}, {num("x2"), num("y2")}}, false});
    } else if (name == "polyline" || name == "polygon") {
      auto v = numberList(geo["points"]);
      Subpath s;
      for (size_t j = 0; j + 1 < v.size(); j += 2) s.pts.push_back({v[j], v[j + 1]});
      s.closed = name == "polygon";
      subs.push_back(s);
    } else {
      problem("element <" + std::string(name) + ">");
      continue;
    }
    if (subs.empty()) continue;
    Shape sh;
    sh.stroke = st.stroke, sh.fill = st.fill, sh.strokeRGB = st.strokeRGB, sh.fillRGB = st.fillRGB;
    float sc = st.xf.scale();
    sh.strokeWidth = st.strokeWidth * sc;
    sh.strokeAlpha = st.strokeAlpha * st.alpha, sh.fillAlpha = st.fillAlpha * st.alpha;
    std::vector<float> dash = st.dash;
    for (float& v : dash) v *= sc;
    for (auto& s : subs) {
      for (auto& p : s.pts) p = st.xf.apply(p);
      if (sh.fill != Paint::None) {
        std::vector<vec2> outline = s.pts;
        cleanPoints(outline, true);
        if (outline.size() >= 3) {
          bool cv = convex(outline);
          sh.fills.push_back(outline);
          sh.fillConvex.push_back(cv);
          sh.fillTris.push_back(cv ? std::vector<uint16_t>{} : triangulate(outline));
        }
      }
      if (sh.stroke != Paint::None && sh.strokeWidth > 0) strokePieces(sh, s.pts, s.closed, dash);
    }
    icon.shapes.push_back(std::move(sh));
  }
  return icon;
}

const Icon* find(std::string_view name) {
  static const std::map<std::string, Icon, std::less<>> all = [] {
    std::map<std::string, Icon, std::less<>> m;
    for (auto& e : embedded()) m[e.name] = parse(e.svg);
    return m;
  }();
  auto it = all.find(name);
  return it == all.end() ? nullptr : &it->second;
}

}  // namespace patina::vicon
