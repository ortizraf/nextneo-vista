#pragma once
// Servers module: Beszel hub status for the homelab + proxy.
#include <Arduino.h>
#include "modules/servers/servers_link.h"

static constexpr uint8_t SERVERS_MAX = 2;
void loadServersConfig();
bool serversFetch();                                 // network task only
bool serversAlert();                                 // any server offline on last check
void serversSetServer(uint8_t i, const char *label, const char *url, const char *id);
void serversSetCredentials(const char *user, const char *pass);   // empty pass = keep
void serversGetConfig(uint8_t i, char *label, char *url, char *id, size_t n);
String serversUser();
bool serversHasPassword();
String serversStatusText(uint8_t i);
