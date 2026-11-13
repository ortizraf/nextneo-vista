#pragma once
// Login for the device's web page (http://nextneovista.local).
// Session cookie (random token, RAM only) that expires after 10 min without use.
// Credentials live in Preferences: user + SHA-256 of the password (default admin/admin),
// changed on the device in Ajustes. Everything runs on the loop task (web server + LVGL),
// so no locking is needed.
#include <Arduino.h>
#include <WebServer.h>

static constexpr uint32_t WEB_SESSION_MS = 10UL * 60UL * 1000UL;

void webAuthBegin(WebServer &server);       // load credentials, collect the Cookie header, /login + /logout routes
bool webAuthCheck(WebServer &server);       // valid session? (refreshes its 10 min)
void webAuthDeny(WebServer &server);        // GET -> /login?next=..., others -> 401
bool webAuthSet(const char *user, const char *pass);   // validates + saves; logs every session out
String webAuthUser();
