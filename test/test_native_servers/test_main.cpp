// Native suite (computer, no board): Servers link parsing and the retry policy that
// keeps one offline hub from blocking the others (bug fixed on 2026-11-14).
#include <unity.h>
#include "modules/servers/servers_link.h"
#include "modules/servers/servers_retry.h"

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- serversParseLink
static void test_link_with_system_id() {
  char url[64], id[24] = "old";
  TEST_ASSERT_TRUE(serversParseLink("  hub.local:8090/system/abc123/  ", url, sizeof url, id, sizeof id));
  TEST_ASSERT_EQUAL_STRING("http://hub.local:8090", url);
  TEST_ASSERT_EQUAL_STRING("abc123", id);
}

static void test_link_keeps_https_and_drops_query() {
  char url[64], id[24] = "";
  TEST_ASSERT_TRUE(serversParseLink("https://beszel.example.com/system/xyz?tab=cpu", url, sizeof url, id, sizeof id));
  TEST_ASSERT_EQUAL_STRING("https://beszel.example.com", url);
  TEST_ASSERT_EQUAL_STRING("xyz", id);
}

static void test_link_without_system_keeps_id() {
  char url[64], id[24] = "keep";
  TEST_ASSERT_TRUE(serversParseLink("http://hub.local:8090///", url, sizeof url, id, sizeof id));
  TEST_ASSERT_EQUAL_STRING("http://hub.local:8090", url);
  TEST_ASSERT_EQUAL_STRING("keep", id);
}

static void test_empty_link_is_rejected() {
  char url[64] = "", id[24] = "";
  TEST_ASSERT_FALSE(serversParseLink("   ", url, sizeof url, id, sizeof id));
  TEST_ASSERT_FALSE(serversParseLink(nullptr, url, sizeof url, id, sizeof id));
}

// ---------------------------------------------------------------- ServerRetry
static void test_all_healthy_checked_in_order() {
  ServerRetry r(2, 60000);
  uint8_t order[ServerRetry::kMax];
  TEST_ASSERT_EQUAL(2, r.plan(1000, order));
  TEST_ASSERT_EQUAL(0, order[0]);
  TEST_ASSERT_EQUAL(1, order[1]);
}

// The bug: server 0 offline used to be checked first, delaying server 1 by ~15 s.
static void test_offline_server_does_not_go_first() {
  ServerRetry r(2, 60000);
  uint8_t order[ServerRetry::kMax];
  r.record(0, false, 1000);
  TEST_ASSERT_EQUAL(1, r.plan(2000, order));       // only the healthy one this cycle
  TEST_ASSERT_EQUAL(1, order[0]);
  TEST_ASSERT_EQUAL(2, r.plan(61000, order));      // 60 s later: retried, after the healthy one
  TEST_ASSERT_EQUAL(1, order[0]);
  TEST_ASSERT_EQUAL(0, order[1]);
}

static void test_recovered_server_is_healthy_again() {
  ServerRetry r(2, 60000);
  uint8_t order[ServerRetry::kMax];
  r.record(0, false, 1000);
  r.record(0, true, 61000);
  TEST_ASSERT_FALSE(r.failed(0));
  TEST_ASSERT_EQUAL(2, r.plan(62000, order));
  TEST_ASSERT_EQUAL(0, order[0]);
}

static void test_both_offline_still_retried() {
  ServerRetry r(2, 60000);
  uint8_t order[ServerRetry::kMax];
  r.record(0, false, 1000);
  r.record(1, false, 1500);
  TEST_ASSERT_EQUAL(0, r.plan(30000, order));
  TEST_ASSERT_EQUAL(2, r.plan(70000, order));
}

static void test_config_change_retries_at_once() {
  ServerRetry r(2, 60000);
  uint8_t order[ServerRetry::kMax];
  r.record(0, false, 1000);
  r.reset(0);
  TEST_ASSERT_EQUAL(2, r.plan(1500, order));
}

static void test_retry_survives_millis_wraparound() {
  ServerRetry r(1, 60000);
  uint8_t order[ServerRetry::kMax];
  r.record(0, false, 0xFFFFFF00u);                 // ~49.7 days of uptime
  TEST_ASSERT_EQUAL(0, r.plan(0x00000100u, order)); // 512 ms later (after the wrap)
  TEST_ASSERT_EQUAL(1, r.plan(0x0000EB00u, order)); // > 60 s later
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_link_with_system_id);
  RUN_TEST(test_link_keeps_https_and_drops_query);
  RUN_TEST(test_link_without_system_keeps_id);
  RUN_TEST(test_empty_link_is_rejected);
  RUN_TEST(test_all_healthy_checked_in_order);
  RUN_TEST(test_offline_server_does_not_go_first);
  RUN_TEST(test_recovered_server_is_healthy_again);
  RUN_TEST(test_both_offline_still_retried);
  RUN_TEST(test_config_change_retries_at_once);
  RUN_TEST(test_retry_survives_millis_wraparound);
  return UNITY_END();
}
