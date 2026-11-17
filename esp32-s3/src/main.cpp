// NextNeo Vista na placa ESP32-S3 3,5". O app inteiro (boot, Wi-Fi, telas, rede) está em
// ../shared (core/app_main.cpp); esta placa fornece o contrato core/board.h (core/board.cpp)
// e as suas telas (modules/, core/screen_registry.cpp).
#include <Arduino.h>
#include "core/app_core.h"

// LVGL renders on the Arduino loop task. Deep widget trees (Weather tabs over the
// sky photo) overflowed the default 8 KB stack ("Stack canary watchpoint (loopTask)").
SET_LOOP_TASK_STACK_SIZE(24 * 1024);

// The on-device tests (test/, PIO_UNIT_TESTING) boot the real app from their own setup().
#ifndef PIO_UNIT_TESTING
void setup() { appSetup(); }
void loop() { appLoop(); }
#endif
