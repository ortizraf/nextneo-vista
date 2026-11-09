// LVGL allocator (LV_USE_STDLIB_MALLOC = LV_STDLIB_CUSTOM).
//
// With LV_STDLIB_CLIB every LVGL object/style (tens to hundreds of bytes each)
// went through malloc(), and the Arduino core keeps every allocation below
// 4 KB in internal RAM. Hundreds of widgets therefore ate the internal heap
// that mbedTLS needs for its handshakes -> "SSL - Memory allocation failed".
// Here all LVGL allocations go to PSRAM (fallback: internal RAM).
#include <lvgl.h>
#include <esp_heap_caps.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

static constexpr uint32_t kPsramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

extern "C" {

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
  LV_UNUSED(mem);
  LV_UNUSED(bytes);
  return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size) {
  void *p = heap_caps_malloc(size, kPsramCaps);
  return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void *lv_realloc_core(void *p, size_t new_size) {
  void *n = heap_caps_realloc(p, new_size, kPsramCaps);
  return n ? n : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) {
  if (!mon_p) return;
  lv_memzero(mon_p, sizeof(*mon_p));
  multi_heap_info_t info;
  heap_caps_get_info(&info, kPsramCaps);
  mon_p->total_size = info.total_free_bytes + info.total_allocated_bytes;
  mon_p->free_size = info.total_free_bytes;
  mon_p->free_biggest_size = info.largest_free_block;
  mon_p->used_cnt = info.allocated_blocks;
  mon_p->free_cnt = info.free_blocks;
  mon_p->max_used = info.total_allocated_bytes;
  mon_p->used_pct = mon_p->total_size ? (uint8_t)(100 - (100ULL * info.total_free_bytes) / mon_p->total_size) : 0;
  mon_p->frag_pct = 0;
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

}  // extern "C"

#endif
