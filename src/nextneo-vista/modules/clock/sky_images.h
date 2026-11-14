#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
enum SkyId { SKY_DAY_CLEAR, SKY_DAY_PARTLY, SKY_OVERCAST_DAY, SKY_RAIN, SKY_STORM, SKY_FOG,
             SKY_DAWN, SKY_DUSK, SKY_NIGHT_CLEAR, SKY_NIGHT_PARTLY, SKY_NIGHT_OVERCAST, SKY_COUNT };
typedef struct { const uint8_t *data; size_t size; } SkyImage;
extern const SkyImage kSkyImages[SKY_COUNT];
#ifdef __cplusplus
}
#endif
