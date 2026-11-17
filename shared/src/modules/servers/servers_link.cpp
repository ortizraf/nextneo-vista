// Beszel link parsing. Pure C++ (no Arduino): unit-tested on the computer.
#include "modules/servers/servers_link.h"
#include <ctype.h>
#include <stdio.h>
#include <string>

bool serversParseLink(const char *link, char *url, size_t urlSize, char *id, size_t idSize) {
  std::string s = link ? link : "";
  size_t a = 0, b = s.size();
  while (a < b && isspace((unsigned char)s[a])) ++a;
  while (b > a && isspace((unsigned char)s[b - 1])) --b;
  s = s.substr(a, b - a);
  if (s.empty()) return false;
  if (s.rfind("http://", 0) != 0 && s.rfind("https://", 0) != 0) s = "http://" + s;
  const size_t p = s.find("/system/");
  if (p != std::string::npos && p > 0) {
    std::string sid = s.substr(p + 8);
    size_t q = sid.find('/'); if (q != std::string::npos) sid = sid.substr(0, q);
    q = sid.find('?'); if (q != std::string::npos) sid = sid.substr(0, q);
    if (id && !sid.empty()) snprintf(id, idSize, "%s", sid.c_str());
    s = s.substr(0, p);
  }
  while (!s.empty() && s.back() == '/') s.pop_back();
  snprintf(url, urlSize, "%s", s.c_str());
  return true;
}
