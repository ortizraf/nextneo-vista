#include "app_log.h"
#include "https_fetch.h"
#include <HTTPClient.h>

bool tlsMemoryAvailable(const char *tag) {
  const size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  if (block >= TLS_MIN_INTERNAL_BLOCK) return true;
  logInternalHeap(tag);
  Log.printf("[tls] %s: memória interna insuficiente para handshake (%u < %u)\n", tag,
                (unsigned)block, (unsigned)TLS_MIN_INTERNAL_BLOCK);
  return false;
}

int httpsFetch(WiFiClientSecure &client, const char *url, PsramBuffer &body,
               uint32_t timeoutMs, bool reuse, const char *postBody, const char *contentType,
               const HttpsFetchOptions &options) {
  body.clear();
  if (!client.connected() && !tlsMemoryAvailable(url)) return HTTPC_ERROR_TOO_LESS_RAM;
  HTTPClient http;
  http.setTimeout(timeoutMs);
  http.setConnectTimeout(timeoutMs);
  http.setReuse(reuse);
  http.useHTTP10(false);
  http.setUserAgent(options.userAgent);
  if (!http.begin(client, url)) return HTTPC_ERROR_CONNECTION_REFUSED;
  const char *responseHeaders[] = {"Retry-After"};
  if (options.retryAfterSeconds) { *options.retryAfterSeconds = 0; http.collectHeaders(responseHeaders, 1); }
  if (options.noCache) {
    http.addHeader("Cache-Control", "no-cache, no-store, max-age=0");
    http.addHeader("Pragma", "no-cache");
  }
  if (reuse) http.addHeader("Connection", "keep-alive");
  http.addHeader("Accept-Encoding", "identity");
  int status;
  if (postBody) {
    http.addHeader("Content-Type", contentType);
    status = http.POST((uint8_t *)postBody, strlen(postBody));
  } else {
    status = http.GET();
  }
  if (status == HTTP_CODE_OK || status == HTTP_CODE_NOT_FOUND) {
    const int size = http.getSize();
    if (size > 0) body.reserve((size_t)size);
    const int written = http.writeToStream(&body);
    if (written < 0 || body.overflowed()) {
      status = written < 0 ? written : HTTPC_ERROR_STREAM_WRITE;
    }
  }
  if (options.retryAfterSeconds && status > 0) *options.retryAfterSeconds = (uint32_t)http.header("Retry-After").toInt();
  http.end();
  // Never keep a broken/one-shot session, nor one with an unread error body.
  if (status <= 0 || !reuse || (status != HTTP_CODE_OK && status != HTTP_CODE_NOT_FOUND)) client.stop();
  return status;
}
