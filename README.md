# NextNeo Vista

Firmware do sistema NextNeo Vista para a placa Guition JC3248W535C. O nome identifica o projeto e não é exibido na interface do dispositivo.

> **Full product specification:** [`docs/index.html`](docs/index.html) (single self-contained page: hardware, pin map, firmware catalog, hub architecture, verification status). Open it locally in a browser, or enable GitHub Pages on the `/docs` folder to host it.

Setup and spec notes for the Guition **JC3248W535C_I_Y** — ESP32-S3 3.5" capacitive touch display module.

## Specs

| Item | Value |
|---|---|
| MCU | ESP32-S3 (QFN56, rev v0.2), dual-core, WiFi + BLE |
| Flash | 16MB quad (QIO) |
| PSRAM | 8MB embedded |
| Crystal | 40MHz |
| USB | Native USB-Serial/JTAG (VID:PID 303A:1001), no CH340/CP210x |
| Display | 3.5" IPS, 320x480, AXS15231B controller, QSPI |
| Touch | Capacitive, I2C address 0x3B (integrated in AXS15231B) |

## Pin mapping (unverified against schematic — confirm on hardware)

| Function | GPIO |
|---|---|
| LCD CS | 45 |
| LCD SCK | 47 |
| LCD D0 / D1 / D2 / D3 | 21 / 48 / 40 / 39 |
| Backlight | 1 |
| Touch SDA / SCL | 4 / 8 |
| Touch INT | 3 |

## Game hub

`examples/hub` is a launcher with a tile for each game. Every game stays its own app in its own flash slot
(`hub_partitions.csv`: `ota_0` = launcher, `ota_1..ota_8` = games), so the games are unchanged apart from a shared
`examples/common/hub_home.h`. Tapping a tile sets that slot as the boot partition and restarts.

- **Tiles show** each game's saved best score/record (read from the game's own NVS namespace), and the last-played game
  has a white outline; launching plays a grow-to-fullscreen animation.
- **Back to the hub:** hold the top-left corner for 1.5 s, or hold the BOOT button for 0.7 s. Paint uses an on-screen
  HOME button instead (tap twice), since its top-left corner is drawing area. Games also point the next boot back at the hub as soon as they start, so a reset or power
  cycle always lands on the hub.
- **Install everything:** `tools/flash_hub.sh` builds and flashes the launcher, bootloader, partition table and all games
  (auto-detects the `/dev/cu.usbmodem*` port).
- **Add a game:** add an env in `platformio.ini`, a tile in `examples/hub/main.cpp`, its name in `tools/flash_hub.sh`,
  and include `hub_home.h` (wrap `readTouch` as the other games do). Slot offsets are `0x1B0000 + (N-1) * 0x100000`.
- Flashing a single sample on its own (`pio run -e <name> -t upload`) overwrites the launcher in `ota_0`; run
  `tools/flash_hub.sh` to restore the hub. The launcher slot is 1.6 MB, so any one sample fits.
- esptool needs PlatformIO's own Python (`~/.platformio/penv/bin/python`); the system Python lacks its dependencies.

## Samples

| Sample | What it is |
|---|---|
| [`examples/weather-clock`](examples/weather-clock/main.cpp) | Wi-Fi clock + weather dashboard (NTP time, Open-Meteo weather, hourly and 7-day forecasts). Settings has Wi-Fi scan/connect, city search, 12/24h switch and brightness. Wi-Fi credentials stay on the board. |
| [`examples/media-player`](examples/media-player/main.cpp) | Photo frame + MJPEG video player from a microSD card. Reads `/pic/*.jpg` and `/mjpeg/*.mjpeg` (also `/photos`, `/video`). Landscape media (e.g. 480x320) is rotated 90° to fill the portrait screen. Tap = pause, swipe = next/prev, long-press = switch photos/video. |
| [`examples/music-player`](examples/music-player/main.cpp) | MP3/WAV/FLAC/AAC player from `/music` on the SD card with an LVGL track list, prev/play/next, volume and progress bar; auto-advances. **Audio output UNVERIFIED** (no speaker was connected when tested; UI and track scan work). |
| [`examples/paint`](examples/paint/main.cpp) | Finger-paint app: 8 colours incl. eraser, brush size, clear, and SAVE to `/paint/paintN.bmp` on the SD card (16-bit RGB565 BMP, opens on Mac/PC). Screen display/touch verified; BMP save not yet checked on a computer. |
| [`examples/tictactoe`](examples/tictactoe/main.cpp) | Tic-tac-toe with 5 modes: CPU Hard (unbeatable minimax), CPU Easy, 2 Players, **Forever 2P** and **Forever CPU** (max 3 marks per player - the oldest vanishes on a 4th placement, shown faded - plus a 4 s per-move countdown, `TURN_MS`). Scores saved in flash. |
| [`examples/game2048`](examples/game2048/main.cpp) | 2048: swipe to slide/merge tiles, score + best, one-step UNDO, win screen with keep-going. Board, score and best are saved in flash and resume after power-off. |
| [`examples/snake`](examples/snake/main.cpp) | Snake on a 20x20 field: on-screen arrow pad (instant turns; swiping also works), PAUSE/NEW, speed ramps from 200 ms to 90 ms per step, best score saved in flash. |
| [`examples/connect4`](examples/connect4/main.cpp) | Connect Four (7x6): tap a column, falling-disc animation, win highlight. Modes: CPU Hard (negamax + alpha-beta, depth 7), CPU Easy, 2 Players. Scores saved in flash. |
| [`examples/tetris`](examples/tetris/main.cpp) | Tetris: 10x20 field, 7-bag randomiser, ghost piece, next preview, levels, on-screen buttons with hold-to-repeat, best score in flash. Touch is debounced (needs a full lift between presses). |
| [`examples/minesweeper`](examples/minesweeper/main.cpp) | Minesweeper: Easy 8x10 / Medium 10x12 / Hard 12x14, safe first tap, flood-fill, chording, long-press or DIG/FLAG toggle to flag, timer, best time per level saved in flash. |
| [`examples/memory`](examples/memory/main.cpp) | Memory Match: 12 drawn symbols, Easy 3x4 / Medium 4x5 / Hard 4x6, flip animation, moves + timer, fewest moves per level saved in flash. |
| [`examples/lvgl-basic`](examples/lvgl-basic/main.cpp) | Minimal LVGL 9 demo: button counter, slider, switch, arc. Good starting point for new UIs. |

## Build

PlatformIO with the pioarduino platform (Arduino core 3.x is required by
GFX Library for Arduino; the stock `espressif32` platform only has core 2.x).
Each sample is a PlatformIO environment:

```
pio run -e weather-clock -t upload --upload-port /dev/cu.usbmodem101
pio run -e media-player  -t upload --upload-port /dev/cu.usbmodem101
pio run -e music-player  -t upload --upload-port /dev/cu.usbmodem101
pio run -e paint         -t upload --upload-port /dev/cu.usbmodem101
pio run -e tictactoe     -t upload --upload-port /dev/cu.usbmodem101
pio run -e game2048      -t upload --upload-port /dev/cu.usbmodem101
pio run -e snake         -t upload --upload-port /dev/cu.usbmodem101
pio run -e connect4      -t upload --upload-port /dev/cu.usbmodem101
pio run -e tetris        -t upload --upload-port /dev/cu.usbmodem101
pio run -e minesweeper   -t upload --upload-port /dev/cu.usbmodem101
pio run -e memory        -t upload --upload-port /dev/cu.usbmodem101
pio run -e lvgl-basic    -t upload --upload-port /dev/cu.usbmodem101
pio device monitor
```

To add a sample, create `examples/<name>/main.cpp` and an `[env:<name>]` entry
in `platformio.ini` with `build_src_filter = -<*> +<<name>/>`.

## Status

Verified on hardware: display, touch and pin mapping all work; touch response is fast.

## microSD

Verified pins (own SPI3/HSPI bus, separate from the display's SPI2): CS=10, MOSI=11, SCK=12, MISO=13. Use `SPIClass sdSpi(HSPI)`.
Video files are raw concatenated JPEG frames (`.mjpeg`), e.g. made with
`ffmpeg -i in.mp4 -vf "scale=480:320" -q:v 6 -r 20 out.mjpeg`.

## Audio (unverified)

The board has **no built-in speaker**: it has a 2-pin JST 1.25 connector for an external 4-8 ohm speaker, driven by an NS4168 mono I2S amp.
Pins used (from an ESPHome config for this board, not yet confirmed on hardware): BCLK=42, LRC=2, DOUT=41.
Uses `schreibfaul1/ESP32-audioI2S`, which needs `-DCORE_DEBUG_LEVEL=0` in PlatformIO. The audio loop runs on its own core so UI redraws do not cause dropouts.
The UI font has no CJK glyphs, so non-ASCII track names are shown as "Track N".

## LVGL demo

Both samples use LVGL 9 and are verified on hardware (touch works, keyboard is usable).

- Config is passed as build flags in `platformio.ini` (`-DLV_CONF_SKIP` plus a few `LV_*` defines), so there is no `lv_conf.h`.
- LVGL renders in PARTIAL mode into a 40-line internal-RAM buffer; `flush_cb` copies each area into the `Arduino_Canvas` and calls `gfx->flush()` once on the last area of a frame.
- On-screen keyboard: make it tall (320px) with a 20pt key font, or keys are too small to hit.
- Games: swipe-only controls are unreliable on this screen (a full-frame redraw can miss part of a swipe); on-screen buttons that act on press feel much better.
- Touch: the controller drops out briefly while a finger is down. For button UIs, require ~6 empty reads before treating it as a release, and a full lift between presses, or one tap registers as several.
- Touch is exposed to LVGL as a pointer indev via `touch_cb`.

## Notes / gotchas

- Colour constants are `RGB565_BLACK`, `RGB565_RED`, etc. in current GFX Library versions.
- Drawing directly to the panel (no canvas) garbles the picture on this controller. Use `Arduino_Canvas` and `flush()`.
- Start the bus at 40MHz: `gfx->begin(40000000)`.
- Flush at most every ~30ms and only when something changed; flushing on every touch event is slow.
- Touch read: write `B5 AB A5 5A 00 00 00 08` to 0x3B, read 8 bytes; `b[1]` = touch count, x = `(b[2]&0x0F)<<8 | b[3]`, y = `(b[4]&0x0F)<<8 | b[5]`.
- If upload won't start: hold BOOT, tap RESET, release BOOT.

## Rafael's board: weather UI work and current install status

The attached board and manual identify a Guition JC3248W535C family board: ESP32-S3, 16 MB flash,
8 MB PSRAM, 3.5-inch capacitive display. The unit is used in landscape orientation (480x320).
The downloaded listing/manual establishes the processor and display specs, but does not prove that
all sample apps or audio output work on this particular unit.

`examples/weather-clock/main.cpp` has a locally customized weather dashboard: dark, condition-colored
backgrounds, Portuguese labels, current conditions, 3-hour forecast, and a 7-day summary that scrolls
horizontally. The original “Agora” weather illustration stays intact; the hourly and daily cards use
the same condition-based artwork scaled down to fit. The app opens on a recreated main menu matching the supplied factory screenshot: two
rows of eight tiles (**MP3, Photo, Video, Setting, Timer, Sound, Weather, Clock**). Only **Setting**
and **Weather** are active. Setting opens Wi-Fi, city, time format and brightness controls; Weather
opens the **Agora/Horas/Dias** tabs.
The weather tabs no longer contain an Ajustes tab. Both screens have a Menu return button. Wi-Fi is
checked every 60 seconds and the app retries the saved network credentials automatically when offline.
The menu Wi-Fi icon and dashboard indicator both reflect the current connection state. Weather views
use shorter titles and compact values with reduced card heights. Unselected weather tabs use brighter
text and visible borders for contrast. The Weather tile uses a droplet symbol.
Weather is fetched from Open-Meteo, with city search/geocoding. Portuguese text and accented city
names use UTF-8; the weather app includes extended Montserrat fonts for Latin accented characters.
The web form accepts UTF-8 city names and can save the city and 24-hour clock preference.

### Simple use guide

1. On the device, tap **Setting**, tap **Buscar**, choose your Wi-Fi network, enter its password, then
   tap **Conectar**. Wait until the Wi-Fi indicator turns green.
2. To configure from a computer or phone, connect it to the same Wi-Fi. On the device, open **Setting**
   and read the IP shown in the status (or check your router's device list). Type `http://IP-DO-APARELHO`
   into the browser, replacing that text with the displayed address. The page lets you set the city and
   24-hour clock option. Wi-Fi credentials are entered on the device itself.
3. The device retries saved Wi-Fi credentials after a disconnect; it checks periodically (every minute).

### Update guide for non-technical users

The latest firmware build, including the UTF-8 font fix, has been installed on this board. To repeat
the update from a Mac with PlatformIO, connect the board by USB and open Terminal in the
`guition-jc3248w535c` folder. Copy and run these commands one at a time:

```sh
pio run -e weather-clock
~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 --port /dev/cu.usbmodem1201 --baud 921600 write-flash --flash-size 16MB 0x10000 .pio/build/weather-clock/firmware.bin
```

Use the USB port shown for the board if it differs from `/dev/cu.usbmodem1201`. This writes only the app
partition at `0x10000`; do not use the ordinary PlatformIO `-t upload` command on this device because it
can overwrite boot and partition data. Wait for `Hash of data verified` before unplugging the board.

The app compiles with `pio run -e weather-clock`. The first version compiled to about 1.68 MB on
2026-10-28. Its first landscape build was written
to app0 and verified by esptool, but showed a black screen. A small LVGL display diagnostic was then
written and the user confirmed that the `JC3248W535C` screen with text and colors appeared; this
confirmed the display, basic driver and backlight. The weather build was corrected to leave the panel
in native portrait mode and rotate the canvas instead, and the corrected build was written to app0
with hash verification. The user then confirmed that the weather screen appeared with text and colors,
but reported that weather values were absent. The new app uses its own Preferences namespace (`clock`)
and did not inherit the factory app's saved Wi-Fi settings. Connect from **Ajustes → Buscar → select
SSID → enter password → Conectar**. On success, the app stores credentials, starts NTP, fetches Open-Meteo
data without an API key, and serves the local page at the displayed IP. Live data, touch behavior and
web access still need confirmation.

### Factory firmware and backup notes

The board was previously reachable at `192.168.66.183`; its factory web UI showed city `São Paulo, BR`
and provided app switching plus PC Monitor, Weather, Theme Clock, MJPEG, Album, Music and Spectrum
controls. A snapshot of that page and status response are in `../backups/board-index.html` and
`../backups/board-status.json`.

The complete 16 MB flash read failed with serial CRC errors. The only flash dump is
`../backups/esp32s3-flash-0-1MiB-partial-2026-10-28.bin`; it is a partial dump, **not a restorable full
backup**. The attempted incompatible Bouffalo/BL616 toolchain was not flashed to this board.

Reading the partition table over USB showed NVS at `0x9000`, OTA metadata at `0xE000`, two 2 MB app
slots at `0x10000` and `0x210000`, and a separate FAT filesystem at `0x410000`. The factory firmware
was selected from the first OTA slot. The second app slot was read and found erased (`0xFF`), so there
was no firmware recovery copy there. With the user's approval, the weather app replaced app0. The
partition table, NVS, OTA metadata and FAT filesystem were left intact. The original app0 contents are
not recoverable from the available partial flash dump. Do not run the sample's ordinary
`pio run -e weather-clock -t upload` on the user's board; it may rewrite boot and partition data in
addition to the app. For this board, use esptool to write only the verified app image to `0x10000`.

### Rebuild and latest install (2026-11-01)

From this repository, with PlatformIO installed and its dependencies available:

```sh
pio run -e weather-clock
```

The updated firmware compiled successfully with PlatformIO 6.2.0, pioarduino/ESP32-S3, LVGL 9.6.0,
GFX Library for Arduino 1.6.9, and ArduinoJson 7.4.3. The resulting app image is 1,679,328 bytes
(SHA-256 `32ab447f7d4567440fa06d194d7505286a4e82b292e974ff760d49e4f3063a28`). It was installed on
`/dev/cu.usbmodem1201` (ESP32-S3 MAC `cc:68:c7:83:fe:98`) at `0x10000`; esptool reported
`Hash of data verified` and reset the board. Only the application partition was written. This build
includes the eight-tile factory-style menu, live Wi-Fi indicator colors, seven-day forecast, and
brighter unselected tab labels/borders. This is the previously installed build; it predates the
UTF-8 font fix below. On-device appearance has not yet been independently checked after reset.

For this device only, after checking the
partition table and confirming that the app fits in 2 MB, the app image was installed with:

```sh
python esptool.py --chip esp32s3 --port /dev/cu.usbmodem1201 --baud 921600 \
  write_flash --flash_size 16MB 0x10000 .pio/build/weather-clock/firmware.bin
```

The write must finish with `Hash of data verified`. Never describe the partial dump above as a full
backup. A genuine full backup requires a successful read of all `0x1000000` bytes and a check that
the read completed without CRC errors.

### UTF-8 and accented text (2026-11-01)

The earlier “S□o Paulo” display was a missing-font-glyph problem. City and web-form text were already
stored and transferred as UTF-8, and LVGL already decoded UTF-8, but the bundled Montserrat fonts only
contained basic ASCII (plus a few symbols). Any accented character could therefore appear as a box or
be omitted. The same limitation affected other Portuguese labels and accented Wi-Fi network names.

The weather app now uses generated Montserrat 14 px and 20 px fonts that include Latin through
U+017F, the euro sign, and the bullet. Each custom font falls back to LVGL's built-in Montserrat font
for symbols and characters outside that range. These fonts are used by the weather labels, city field,
Wi-Fi list and keyboard. LVGL uses its UTF-8 text mode; the city search URL encoder percent-encodes
UTF-8 bytes and passes only ASCII unreserved characters through. The browser form declares UTF-8
both in its page metadata and form.
The old default `Sao Paulo` preference is migrated to `São Paulo` on startup.

Generated font sources are checked into `examples/weather-clock/lv_font_weather_14.c` and
`examples/weather-clock/lv_font_weather_20.c`; normal builds do not need the font converter or source
TTF. Portuguese labels now retain their accents, including `São Paulo`, `Previsão`, `Próximos`,
`Relógio`, and `Sáb`.

The first generated-font build used compressed glyph data and left the display without text. The board
was recovered with its original built-in fonts, then the custom fonts were regenerated without
compression, matching the bitmap format of LVGL's bundled fonts. The current build compiled with
`pio run -e weather-clock` and is 1,742,064 bytes (SHA-256
`eba21f237cc5ef98db1b7e5c7b36a28818d611f35863f6c2472ec36b2a746c61`), within the board's 2 MB app
partition. It was written to the ESP32-S3 (`/dev/cu.usbmodem1201`, MAC `cc:68:c7:83:fe:98`) at
`0x10000`; esptool verified the hash and reset the board. Only the app partition was written. The
screen still needs a visual check after this latest update.

### Atualizar o aplicativo de clima (macOS)

Para atualizar o ESP32 sem mexer no bootloader, tabela de partições, configurações Wi-Fi ou arquivos do usuário:

1. Instale o **PlatformIO Core** (`pio`) e conecte o dispositivo por USB.
2. No Terminal, entre na pasta `guition-jc3248w535c`.
3. Execute:

```sh
./tools/update_weather.sh
```

O script compila `weather-clock`, detecta a porta `/dev/cu.usbmodem*` e grava **apenas** `.pio/build/weather-clock/firmware.bin` no offset `0x10000`. Se houver mais de uma porta USB ou a detecção falhar, informe a porta manualmente:

```sh
./tools/update_weather.sh /dev/cu.usbmodem1201
```

Ao final, confirme a mensagem `Hash of data verified`. Não use `pio run -e weather-clock -t upload` neste dispositivo: esse comando pode gravar também dados de boot e partições. O script depende do Python e do esptool instalados pelo PlatformIO em `~/.platformio`; a primeira compilação pode baixar a plataforma ESP32 e bibliotecas e demorar alguns minutos. O build cache em `.pio/` pode ser apagado: será recriado na próxima compilação.

**Conteúdo do projeto:** fontes e documentação estão neste repositório; as fontes Latin UTF-8 geradas estão em `examples/weather-clock/lv_font_weather_14.c` e `lv_font_weather_20.c`. Elas já estão incluídas no firmware e não precisam ser regeneradas. A pasta original `JC3248W535EN`, `backups`, `desktop-ornaments`, `toolchain_gcc_t-head_macos` e `tools-vendor` não são dependências do build atual. O dump parcial antigo em `backups` não é recuperável como firmware completo.

**Componentes do build do clima:** PlatformIO Core 6.x, plataforma pioarduino ESP32-S3, Arduino-ESP32 3.x, LVGL 9.x, GFX Library for Arduino, ArduinoJson e JPEGDEC; as versões efetivamente resolvidas ficam registradas no log do PlatformIO. O aplicativo consulta o Open-Meteo sem chave, sincroniza hora via NTP e atualiza a previsão a cada 15 minutos; a reconexão Wi-Fi é verificada a cada minuto.
