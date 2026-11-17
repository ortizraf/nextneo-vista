// Versão e horário do build, gerados em core/build_info.cpp por tools/pio_progname.py.
#pragma once

extern const char *const kBuildVersion;   // git describe --tags --always --dirty (ex.: v1.2.0, v1.2.0-3-gabc123-dirty)
extern const char *const kBuildBranch;    // branch do git no build
extern const unsigned long kBuildEpoch;   // horário do build, Unix epoch (UTC)
