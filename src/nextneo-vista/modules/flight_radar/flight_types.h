#pragma once

#include <stddef.h>
#include <stdint.h>

// Aircraft kept per poll (nearest first). Buffers live in PSRAM, so this can be
// generous without touching the internal heap that mbedTLS needs.
static constexpr uint8_t FLIGHT_RADAR_MAX_AIRCRAFT = 64;

struct Aircraft {
  char icao24[7] = {};
  char callsign[9] = {};
  char type[9] = {};
  char registration[9] = {};
  char category[3] = {};      // ADS-B emitter category: A1 light ... A5 heavy, A7 rotorcraft
  char squawk[5] = {};
  double latitude = 0;
  double longitude = 0;
  float altitudeFeet = 0;
  float speedKnots = 0;
  float headingDegrees = 0;
  float verticalRateFpm = 0;
  float positionAgeSeconds = 0;
  float distanceNm = 0;
  bool hasAltitude = false;
  bool hasSpeed = false;
  bool hasHeading = false;
  bool hasVerticalRate = false;
  bool hasPositionAge = false;
  bool onGround = false;
  bool emergency = false;
};

enum class FlightRadarStatus : uint8_t { LOADING, ONLINE, STALE, OFFLINE, EMPTY };

struct FlightRadarSnapshot {
  Aircraft aircraft[FLIGHT_RADAR_MAX_AIRCRAFT];
  uint8_t count = 0;
  FlightRadarStatus status = FlightRadarStatus::LOADING;
  uint32_t lastSuccessMs = 0;
  uint32_t version = 0;
  char airportCode[5] = "CGH";
  char airportName[24] = "Congonhas";
  double centerLat = -23.626111;
  double centerLon = -46.656389;
  float radiusKm = 10.0f;
  char message[48] = "Aguardando consulta";
};

// Extra data for the selected aircraft (route, airline, model) from adsbdb.com.
enum class DetailsState : uint8_t { NONE, LOADING, READY, NOT_FOUND, FAILED };

struct AircraftDetails {
  char icao24[7] = {};
  char callsign[9] = {};
  char model[40] = {};        // "Airbus A320 214"
  char owner[40] = {};        // registered owner / operator
  char airline[40] = {};
  char originCode[5] = {};    // IATA (or ICAO when IATA is missing)
  char destCode[5] = {};
  char originCity[28] = {};
  char destCity[28] = {};
  DetailsState state = DetailsState::NONE;
  uint32_t requestedMs = 0;
};
