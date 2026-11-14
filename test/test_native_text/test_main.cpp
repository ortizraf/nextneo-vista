// Native suite (computer, no board): uiSafeText replaces glyphs our fonts lack.
#include <unity.h>
#include "core/text_utils.h"
#include <string.h>

void setUp() {}
void tearDown() {}

static void test_ascii_and_latin1_text_is_returned_as_is() {
  char buf[64];
  const char *in = "S\xC3\xA3o Paulo, 24\xC2\xB0";   // "São Paulo, 24°": 1-2 byte chars only
  TEST_ASSERT_EQUAL_PTR(in, uiSafeText(in, buf, sizeof buf));   // no copy needed
}

static void test_supported_3_byte_glyphs_are_kept() {
  char buf[64];
  // "€" and "•" are 3-byte UTF-8 but exist in our fonts: copied unchanged
  TEST_ASSERT_EQUAL_STRING("10 \xE2\x82\xAC \xE2\x80\xA2 ok", uiSafeText("10 \xE2\x82\xAC \xE2\x80\xA2 ok", buf, sizeof buf));
}

static void test_typographic_marks_are_replaced() {
  char buf[64];
  // em dash, curly quotes and an ellipsis (from APIs)
  TEST_ASSERT_EQUAL_STRING("A - B 'x' \"y\"...",
    uiSafeText("A \xE2\x80\x94 B \xE2\x80\x98x\xE2\x80\x99 \xE2\x80\x9Cy\xE2\x80\x9D\xE2\x80\xA6", buf, sizeof buf));
}

static void test_unknown_glyph_and_emoji_become_question_mark() {
  char buf[64];
  TEST_ASSERT_EQUAL_STRING("ok ?", uiSafeText("ok \xF0\x9F\x98\x80", buf, sizeof buf));   // emoji
}

static void test_output_never_overflows() {
  char buf[8];
  uiSafeText("\xE2\x80\x94\xE2\x80\x94\xE2\x80\x94\xE2\x80\x94\xE2\x80\x94\xE2\x80\x94", buf, sizeof buf);
  TEST_ASSERT_TRUE(strlen(buf) < sizeof buf);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ascii_and_latin1_text_is_returned_as_is);
  RUN_TEST(test_supported_3_byte_glyphs_are_kept);
  RUN_TEST(test_typographic_marks_are_replaced);
  RUN_TEST(test_unknown_glyph_and_emoji_become_question_mark);
  RUN_TEST(test_output_never_overflows);
  return UNITY_END();
}
