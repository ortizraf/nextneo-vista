#pragma once
// Futebol module: ESPN public API, only while the screen is active.
#include <Arduino.h>

bool footballFetch();                                // network task
void footballReleaseConnection();                    // network task: drop the keep-alive TLS session
void footballSetFavorites(const char *list);         // "FLA,PAL" (ESPN abbreviations)
String footballFavorites();
