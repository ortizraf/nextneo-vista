#pragma once
// Settings web page (http://nextneovista.local): the API a module uses to add itself.
// Each module owns a WebSection: its card on the page (HTML) and its routes. The core
// (core/web_config.cpp) lists the sections, builds the page in that order, and keeps
// only what is not a screen: header card, log page and login. The board adds kBoardWeb.
#include <Arduino.h>
#include <WebServer.h>
#include "services/web_auth.h"

struct WebSection {
  const char *name;
  void (*card)(String &html);            // appends this module's card to the settings page
  void (*routes)(WebServer &server);     // registers this module's routes (forms, files...)
};

String webEscape(const String &value);                          // HTML-escape user text
void webSendSettings(WebServer &server, const String &notice);  // answer a form: settings page + notice

// Every page / action needs a login session (services/web_auth). First line of a handler:
#define WEB_REQUIRE_LOGIN(server) if (!webAuthCheck(server)) { webAuthDeny(server); return; }

// ---- card of the board (display options...): each board defines it (core/board_web.cpp)
extern const WebSection kBoardWeb;

// ---- sections, defined by their modules (<module>_web.cpp)
extern const WebSection kFootballWeb;
extern const WebSection kWeatherWeb;
extern const WebSection kRadarWeb;
extern const WebSection kFinanceWeb;
extern const WebSection kPhotosWeb;
extern const WebSection kServersWeb;
extern const WebSection kCalendarWeb;
