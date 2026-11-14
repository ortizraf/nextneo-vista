// Weather network jobs (network task only): Open-Meteo forecast and city geocoding.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "app_screens.h"
#include "modules/weather/weather.h"
#include "services/https_fetch.h"
#include "services/psram_alloc.h"
#include "services/app_log.h"

static String urlEncode(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    const uint8_t c = (uint8_t)s[i];
    // Encode UTF-8 bytes individually; only ASCII URL-unreserved bytes pass through.
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { char b[4]; snprintf(b, sizeof b, "%%%02X", (unsigned char)c); o += b; }
  }
  return o;
}

static bool httpGetJson(const String &url, JsonDocument &doc, String &error) {
  static PsramBuffer body(256 * 1024);   // response kept in PSRAM, not in an internal String
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!screenJobStillCurrent()) { error = "cancelado (tela inativa)"; return false; }  // no retry off-screen
    WiFiClientSecure client;
    client.setInsecure();  // public read-only weather data; no secrets are sent
    client.setHandshakeTimeout(12);
    const int code = httpsFetch(client, url.c_str(), body, 10000, false);
    client.stop();
    if (code == HTTP_CODE_OK) {
      // The user may have left the screen while the download was running:
      // skip the JSON parsing entirely in that case.
      if (!screenJobStillCurrent()) { error = "cancelado (tela inativa)"; return false; }
      DeserializationError jsonError = deserializeJson(doc, body.c_str(), body.length());
      if (!jsonError) return true;
      error = String("JSON inválido: ") + jsonError.c_str();
      Log.printf("Weather: %s\n", error.c_str());
      return false;
    }
    error = code <= 0 ? String("erro de conexão: ") + HTTPClient::errorToString(code)
                      : String("HTTP ") + code;
    Log.printf("Weather: %s (tentativa %d/2)\n", error.c_str(), attempt + 1);
    if (code <= 0) logInternalHeap("weather");
    // Retry only transient transport/server errors; client errors such as 429
    // are handled by the worker's slower retry schedule.
    if (attempt == 0 && (code <= 0 || code >= HTTP_CODE_INTERNAL_SERVER_ERROR)) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    return false;
  }
  return false;
}

// Open-Meteo's CURRENT weather_code comes from the forecast model and often says "garoa"
// while the sun is out (it follows the rain chance of the coming hours). Seen: code 51 with
// UV 5.8, humidity 50 %, cloud cover 35 %, 0.1 mm. For drizzle / rain (51-67) trust it only
// when the sky is mostly covered (>= 60 %) AND it is really raining (>= 0.2 mm in 15 min);
// otherwise show the sky from the cloud cover. Showers / storms (80+) are kept as sent.
// Hourly / daily codes are forecasts and are not touched.
static constexpr int OVERCAST_PCT = 71;   // total cloud cover from which "now" is shown as overcast

static int checkCurrentCode(int code, float precipMm, int cloudPct) {
  if (cloudPct < 0) return code;
  // Clear / partly cloudy with lots of cloud: the model often misses low clouds -> overcast.
  if (code <= 2) return cloudPct >= OVERCAST_PCT ? 3 : code;
  if (code < 51 || code > 67) return code;   // fog, snow, showers, storms: as the API says
  // Drizzle / rain (51-67): kept only if it is really raining; otherwise use the cloud cover.
  if (cloudPct >= 60 && precipMm >= 0.2f) return code;
  if (cloudPct < 20) return 0;          // céu limpo
  if (cloudPct < 50) return 1;          // poucas nuvens
  if (cloudPct < OVERCAST_PCT) return 2;   // parcialmente nublado
  return 3;                             // nublado
}

bool weatherFetch() {
  if (WiFi.status() != WL_CONNECTED) return false;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(cfg.lat, 4) +
               "&longitude=" + String(cfg.lon, 4) +
               "&current=temperature_2m,weather_code,relative_humidity_2m,wind_speed_10m,apparent_temperature,uv_index,precipitation,cloud_cover,is_day"
               "&hourly=temperature_2m,weather_code,precipitation_probability,is_day"
               "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,"
               "precipitation_sum,precipitation_hours,cloud_cover_mean"
               "&timezone=auto&forecast_days=7&forecast_hours=24";
  JsonDocument doc(SpiRamJsonAllocator::instance());  // PSRAM: keep internal RAM for TLS
  String error;
  if (!httpGetJson(url, doc, error)) {
    if (screenJobStillCurrent()) setStatus(String("Falha clima: ") + error);
    return false;
  }
  JsonArray hourlyTimes = doc["hourly"]["time"].as<JsonArray>();
  JsonArray hourlyTemps = doc["hourly"]["temperature_2m"].as<JsonArray>();
  JsonArray hourlyCodes = doc["hourly"]["weather_code"].as<JsonArray>();
  JsonArray hourlyRain = doc["hourly"]["precipitation_probability"].as<JsonArray>();
  JsonArray hourlyIsDay = doc["hourly"]["is_day"].as<JsonArray>();
  JsonArray dailyTimes = doc["daily"]["time"].as<JsonArray>();
  JsonArray dailyMax = doc["daily"]["temperature_2m_max"].as<JsonArray>();
  JsonArray dailyMin = doc["daily"]["temperature_2m_min"].as<JsonArray>();
  JsonArray dailyCodes = doc["daily"]["weather_code"].as<JsonArray>();
  if (doc["current"]["temperature_2m"].isNull() || hourlyTimes.isNull() ||
      hourlyTimes.size() == 0 || hourlyTemps.size() != hourlyTimes.size() ||
      hourlyCodes.size() != hourlyTimes.size() || hourlyRain.size() != hourlyTimes.size() ||
      dailyTimes.isNull() || dailyTimes.size() < 7 || dailyMax.size() < 7 ||
      dailyMin.size() < 7 || dailyCodes.size() < 7) {
    setStatus("Falha clima: resposta incompleta da API");
    Log.println("Weather: resposta incompleta da API");
    return false;
  }

  int off = doc["utc_offset_seconds"] | cfg.utcOffset;
  if (off != cfg.utcOffset) {
    cfg.utcOffset = off;
    prefs.putInt("tz", off);
    applyTime();
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  sh.temp = doc["current"]["temperature_2m"] | 0.0f;
  {
    const int apiCode = doc["current"]["weather_code"] | 0;
    const float precip = doc["current"]["precipitation"] | 0.0f;
    const int cloud = doc["current"]["cloud_cover"] | -1;
    sh.code = checkCurrentCode(apiCode, precip, cloud);
    sh.isDay = (doc["current"]["is_day"] | 1) != 0;
    if (sh.code != apiCode)
      Log.printf("Clima: API disse %d (%s), mas nuvens %d%% e %.1f mm -> mostrando %d\n",
                 apiCode, weatherText(apiCode), cloud, precip, sh.code);
  }
  sh.humidity = doc["current"]["relative_humidity_2m"] | 0;
  sh.wind = doc["current"]["wind_speed_10m"] | 0.0f;
  sh.feels = doc["current"]["apparent_temperature"] | sh.temp;
  sh.uv = doc["current"]["uv_index"] | -1.0f;
  sh.hourCount = 0;
  JsonArray times = hourlyTimes;
  JsonArray hourlyTemp = hourlyTemps;
  JsonArray hourlyCode = hourlyCodes;
  String currentHour = String(doc["current"]["time"] | "");
  currentHour = currentHour.substring(0, 13);
  int first = 0;
  for (size_t i = 0; i < times.size(); i++) {
    String candidate = String(times[i] | "");
    if (candidate.startsWith(currentHour)) { first = i; break; }
  }
  for (int j = 0; j < 8; j++) {
    int i = first + j * 3;
    if (i >= (int)times.size()) break;
    String hour = String(times[i] | "");
    if (hour.length() >= 13) {
      sh.hourLabel[j][0] = hour[11]; sh.hourLabel[j][1] = hour[12];
      sh.hourLabel[j][2] = 0;
    }
    sh.hourTemp[j] = hourlyTemp[i] | 0.0f;
    sh.hourCode[j] = hourlyCode[i] | 0;
    sh.hourRain[j] = hourlyRain[i] | 0;
    sh.hourIsDay[j] = (hourlyIsDay.isNull() ? 1 : (hourlyIsDay[i] | 1)) != 0;
    sh.hourCount++;
  }
  // O card "Agora" deve mostrar o mesmo valor do bloco principal (current),
  // e não a previsão da hora cheia (hh:00), que pode diferir 1-2 °C.
  if (sh.hourCount > 0) {
    sh.hourTemp[0] = sh.temp;
    sh.hourCode[0] = sh.code;
    sh.hourIsDay[0] = sh.isDay;
  }
  for (int i = 0; i < 7; i++) {
    sh.tmax[i] = doc["daily"]["temperature_2m_max"][i] | 0.0f;
    sh.tmin[i] = doc["daily"]["temperature_2m_min"][i] | 0.0f;
    sh.dcode[i] = doc["daily"]["weather_code"][i] | 0;
    // Open-Meteo's daily code is the WORST hour of the day: one hour of drizzle turns the
    // whole day into "rain". Keep rain only if it really rains (>= 1 mm and >= 2 h);
    // otherwise show the day's mean cloud cover instead.
    {
      const int dc = sh.dcode[i];
      const bool wet = (dc >= 51 && dc <= 67) || (dc >= 80 && dc <= 82) || dc >= 95;
      const float mm = doc["daily"]["precipitation_sum"][i] | -1.0f;
      const float hrs = doc["daily"]["precipitation_hours"][i] | -1.0f;
      if (wet && mm >= 0 && hrs >= 0 && (mm < 1.0f || hrs < 2.0f)) {
        const int cc = doc["daily"]["cloud_cover_mean"][i] | 50;
        sh.dcode[i] = cc < 25 ? 1 : cc < 65 ? 2 : 3;
        LogV.printf("Weather: dia %d código %d -> %d (chuva %.1f mm em %.0f h, nuvens %d%%)\n", i, dc, sh.dcode[i], mm, hrs, cc);
      }
    }
    sh.dayRain[i] = doc["daily"]["precipitation_probability_max"][i] | 0;
    const char *d = doc["daily"]["time"][i] | "";
    int y, m, dd;
    struct tm t = {};
    if (sscanf(d, "%d-%d-%d", &y, &m, &dd) == 3) {
      t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = dd; t.tm_hour = 12;
      mktime(&t);
      static const char *weekdays[] = {"Dom", "Seg", "Ter", "Qua", "Qui", "Sex", "Sáb"};
      snprintf(sh.dayName[i], sizeof sh.dayName[i], "%s", weekdays[t.tm_wday]);
    }
  }
  sh.haveWeather = true;
  sh.weatherVersion++;
  sh.status = "Previsão atualizada";
  xSemaphoreGive(mtx);
  return true;
}

// Aplica "Nome @ lat,lon" sem usar a rede. Grava também o texto digitado
// ("cityRaw") para o formulário mostrar exatamente o que o usuário salvou.
bool applyManualLocation(const String &input) {
  int at = input.indexOf('@');
  if (at < 0) return false;
  float lat, lon;
  String coords = input.substring(at + 1);
  coords.replace(" ", "");
  if (sscanf(coords.c_str(), "%f,%f", &lat, &lon) != 2 ||
      lat < -90 || lat > 90 || lon < -180 || lon > 180) return false;
  String name = input.substring(0, at);
  name.trim();
  if (name.isEmpty()) name = "Local";
  cfg.lat = lat;
  cfg.lon = lon;
  prefs.putFloat("lat", cfg.lat);
  prefs.putFloat("lon", cfg.lon);
  prefs.putString("city", name);
  prefs.putString("cityRaw", input);
  xSemaphoreTake(mtx, portMAX_DELAY);
  sh.cityName = name;
  sh.weatherVersion++;
  xSemaphoreGive(mtx);
  Log.println("Local manual salvo");   // no name/coordinates in the log (privacy)
  setStatus("Local salvo: " + name);
  return true;
}

bool weatherGeocodeFetch() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  String city = sh.pendingCity;
  xSemaphoreGive(mtx);
  if (city.isEmpty() || WiFi.status() != WL_CONNECTED) { setStatus("Conecte ao Wi-Fi primeiro"); return false; }
  // Coordenadas manuais: "Nome @ lat,lon" (ex.: "Minha casa @ -xx.xxx,-xx.xxx").
  if (city.indexOf('@') >= 0) {
    if (applyManualLocation(city)) return true;
    setStatus("Coordenadas inválidas (use Nome @ lat,lon)");
    return false;
  }
  String countryCode;
  int comma = city.lastIndexOf(',');
  if (comma >= 0) {
    countryCode = city.substring(comma + 1);
    countryCode.trim();
    city = city.substring(0, comma);
    city.trim();
    countryCode.toUpperCase();
  }
  String lookup = "https://geocoding-api.open-meteo.com/v1/search?count=1&name=" + urlEncode(city);
  if (countryCode.length() == 2) lookup += "&countryCode=" + countryCode;
  JsonDocument doc(SpiRamJsonAllocator::instance());  // PSRAM: keep internal RAM for TLS
  String error;
  if (!httpGetJson(lookup, doc, error)) {
    setStatus(String("Falha geocodificação: ") + error);
    return false;
  }
  if (doc["results"].isNull()) {
    setStatus("Cidade não encontrada");
    return false;
  }
  cfg.lat = doc["results"][0]["latitude"] | cfg.lat;
  cfg.lon = doc["results"][0]["longitude"] | cfg.lon;
  String name = doc["results"][0]["name"] | city.c_str();
  String resolvedCountry = doc["results"][0]["country_code"] | countryCode.c_str();
  if (resolvedCountry.length()) name += ", " + resolvedCountry;
  prefs.putFloat("lat", cfg.lat);
  prefs.putFloat("lon", cfg.lon);
  prefs.putString("city", name);
  prefs.remove("cityRaw");
  xSemaphoreTake(mtx, portMAX_DELAY);
  sh.cityName = name;
  sh.weatherVersion++;
  xSemaphoreGive(mtx);
  setStatus("Local salvo: " + name);
  Log.println("Cidade encontrada (geocodificação ok)");   // no name/coordinates in the log (privacy)
  return true;
}
