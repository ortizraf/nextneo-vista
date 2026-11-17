#pragma once
// Servers module internals: data side (servers_data.cpp: config + Beszel fetch on the
// network task) shared with the screen (servers_screen.cpp). Guarded by mtx.
#include <Arduino.h>
#include "modules/servers/servers.h"

namespace servers {

extern SemaphoreHandle_t srvMtx;             // guards gCfg / gData (not the app-wide mtx)

static constexpr int HIST = 60;

struct ServerCfg { char label[24]; char url[64]; char id[24]; };
struct ServerData {
  bool known = false, reachable = false, up = false;
  char name[32] = "", host[32] = "", kernel[32] = "", cpuModel[48] = "", agent[12] = "", error[48] = "";
  uint8_t cores = 0;
  float cpu = 0, ram = 0, disk = 0, temp = -1, load[3] = {-1, -1, -1};
  float netIn = 0, netOut = 0;     // bytes/s
  uint32_t uptime = 0, updatedEpoch = 0;
  float hCpu[HIST], hRam[HIST], hDisk[HIST], hNet[HIST];
  uint8_t hCount = 0;
  uint32_t histMs = 0;             // millis() of the last history download
};

extern ServerCfg gCfg[SERVERS_MAX];
extern ServerData gData[SERVERS_MAX];
extern uint32_t gDataVersion;             // bumped on every new result / config change
extern const char *const DEF_ROLE[SERVERS_MAX];

}  // namespace servers
