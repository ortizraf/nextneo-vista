#pragma once
#include <stdint.h>
void displayBegin();                         // panel, touch, LVGL display + input device
uint32_t displayQspiHz();                    // panel bus clock in use (web page)
// Multi-touch (pinch on the Airport screen): number of fingers and the 2nd finger
// in landscape coordinates. LVGL itself only gets the first finger.
extern volatile uint8_t gTouchCount;
extern volatile int16_t gTouch2X, gTouch2Y;
