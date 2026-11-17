# Generative Art (tela "Arte")

Arte procedural calculada no ESP32-S3, sem rede, imagens ou nuvem.

## Arquivos

| Arquivo | Papel |
|---|---|
| `art_surface.h/.cpp` | Buffer RGB565, fade de rastro, linha/ponto aditivos, paletas, tabela de seno, RNG |
| `art_pattern.h` | Interface `ArtPattern` + `ArtParams` / `ArtContext` |
| `art_engine.h/.cpp` | `GenerativeArtEngine`: buffer, padrão ativo, parâmetros, toque, tempo |
| `pattern_waves.cpp` | Ondas (soma de senos, linhas e pontos) |
| `pattern_particles.cpp` | Partículas (campo de força, atração/repulsão ao toque, conexões) |
| `pattern_flow.cpp` | Fluxo (campo vetorial procedural seguido por traçadores) |
| `generative_art_screen.cpp` | Tela LVGL: imagem cheia, controles, painel de ajustes, modo ambiente |

Alterações fora da pasta: `app_screens.h` (`SCREEN_ART` + declarações) e `main.cpp`
(entrada em `kScreens`, ícone "Arte" no menu, `buildArtScreen`).

## Decisões técnicas

- **Renderização**: um buffer 480x320 RGB565 (300 KB) em PSRAM, mostrado por um `lv_image`
  (mesma técnica do céu do Relógio). Os padrões desenham direto no buffer; o LVGL só copia.
  O painel AXS15231B recebe sempre o quadro inteiro (~40 ms no core 0), por isso não há
  ganho em atualizar regiões parciais — a meta é **~15 fps** (timer de 66 ms).
- **Sem alocação por quadro**: buffer e estado dos padrões são alocados na entrada da tela
  (ou ao trocar de padrão / densidade) e liberados no `onExit`.
- **Thread**: tudo roda num `lv_timer` na thread do LVGL; nada bloqueia e não há locks.
  A tela não usa rede (sem `requestUpdate`).
- **Matemática barata**: seno por tabela de 256 posições, RNG xorshift, fade que pula
  palavras pretas e processa 2 pixels por leitura.
- **Limites**: Ondas 3–6, Partículas 30–140 (máx. 260 conexões/quadro), Fluxo 80–320 traçadores.
- **Brilho**: usa o mesmo `analogWrite(GFX_BL, …)` de Ajustes. O modo ambiente aplica
  10–60 % do brilho do usuário e desacelera a animação (×0,4); ao sair, o brilho volta.
- **Inatividade**: o projeto não tem gerenciador de inatividade; o modo ambiente é manual.
  `artSetAmbient(bool)` é a função pública para um gerenciador futuro.
- **Ajustes** (Preferences, chaves `ga*`) são gravados ao fechar o painel ou sair da tela,
  nunca a cada passo do slider.

## Medição

Com log detalhado ligado (página `/log`): a linha `[perf] art:` já existente mostra fps e
tempo de quadro; a linha `[art]` mostra o tempo médio do passo do padrão e a memória livre.

## Novo padrão

1. Criar `pattern_<nome>.cpp` implementando `ArtPattern` e `ArtPattern &artPattern<Nome>()`.
2. Declarar em `art_pattern.h` e adicionar em `kPatterns[]` (`art_engine.cpp`).
3. Adicionar o nome em `kPatternMap` (`generative_art_screen.cpp`).
