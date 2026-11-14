#pragma once
// Beszel link parsing. Pure C++ (no Arduino): unit-tested on the computer.
#include <stddef.h>

// "nextneo.local:8090/system/abc123/" -> url "http://nextneo.local:8090", id "abc123".
// A link without "/system/<id>" only sets the url (id untouched). False if empty.
bool serversParseLink(const char *link, char *url, size_t urlSize, char *id, size_t idSize);
