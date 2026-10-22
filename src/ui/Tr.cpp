#include "ui/Tr.h"

#include <cstdarg>
#include <cstdio>

namespace patina {

std::string F_(const char* fmt, ...) {
  const char* f = i18n::tr(fmt);
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = std::vsnprintf(nullptr, 0, f, ap);
  std::string out(n > 0 ? size_t(n) : 0, '\0');
  if (n > 0) std::vsnprintf(out.data(), out.size() + 1, f, ap2);
  va_end(ap2);
  va_end(ap);
  return out;
}

}  // namespace patina
