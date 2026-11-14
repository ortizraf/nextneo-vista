#pragma once
#include <WiFiClientSecure.h>
#include "psram_alloc.h"

// Minimum largest-free-block of internal RAM needed to try a TLS handshake.
// Below this the handshake would fail with -32512 (MBEDTLS_ERR_SSL_ALLOC_FAILED)
// anyway, so we skip the attempt and report a clear error instead.
static constexpr size_t TLS_MIN_INTERNAL_BLOCK = 22 * 1024;

// A reused keep-alive session must answer within this time; otherwise it is treated as
// closed by the server and the request is repeated once on a new connection.
static constexpr uint32_t REUSED_TIMEOUT_MS = 4000;

// GET (or POST when postBody != nullptr) into a PSRAM buffer.
// Handles chunked transfer encoding. Returns the HTTP status code (>0) or a
// negative HTTPClient error. `reuse` keeps the TLS session open afterwards.
struct HttpsFetchOptions {
  const char *userAgent = "NextNeoVista/1.0 (ESP32-S3)";
  bool noCache = false;                 // adds Cache-Control/Pragma: no-cache
  uint32_t *retryAfterSeconds = nullptr; // filled from Retry-After (429/503)
  bool followRedirects = false;         // e.g. image CDNs that answer with a 301/302
};

int httpsFetch(WiFiClientSecure &client, const char *url, PsramBuffer &body,
               uint32_t timeoutMs, bool reuse, const char *postBody = nullptr,
               const char *contentType = "application/json",
               const HttpsFetchOptions &options = HttpsFetchOptions());

bool tlsMemoryAvailable(const char *tag);
