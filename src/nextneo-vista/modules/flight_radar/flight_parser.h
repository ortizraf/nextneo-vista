#pragma once
#include "flight_types.h"
#include <ArduinoJson.h>

// Parses one aircraft object from an ADSB.lol / readsb "ac" array.
// Returns false when the entry has no usable position.
bool parseAdsbLolAircraftObject(JsonObjectConst obj, Aircraft &out);

bool parseAdsbLolAircraft(JsonArrayConst aircraftJson, Aircraft *out, uint8_t capacity,
                          uint8_t &count);
