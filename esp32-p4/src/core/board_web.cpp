// Cartão da página web específico da P4 (kBoardWeb, core/web_ui.h). Por enquanto só mostra
// a placa e os recursos; ajustes de hardware (brilho, áudio...) entram aqui depois.
#include <Arduino.h>
#include <WebServer.h>
#include "core/app_core.h"
#include "core/board.h"
#include "core/web_ui.h"

static void boardCard(String &html) {
  auto yes = [](bool b) { return b ? T("sim", "yes") : T("não", "no"); };
  html += String("<div class=\"card\"><h2>") + T("Placa", "Board") + "</h2><div class=\"muted\">" +
          webEscape(kBoard.name) + " · " + kBoard.width + "x" + kBoard.height + " · " + kBoard.dpi + " dpi<br>" +
          T("Áudio: ", "Audio: ") + yes(kBoard.hasAudio) + " · TF: " + yes(kBoard.hasSdCard) + " · " +
          T("Câmera: ", "Camera: ") + yes(kBoard.hasCamera) + " · " + T("Bateria: ", "Battery: ") + yes(kBoard.hasBattery) +
          "</div></div>\n";
}
static void boardRoutes(WebServer &) {}
const WebSection kBoardWeb = {"board", boardCard, boardRoutes};
