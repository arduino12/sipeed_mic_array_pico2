#pragma once
// Line-based command console on the CDC port. Every command answers with
// exactly one JSON line: {"ok":"<cmd>",...} or {"err":"..."}. Telemetry lines
// carry "az" (JSON format) or "src" (ODAS format) and never "ok"/"err".
#include <stdint.h>

namespace console {

void poll();
void telemetry(uint32_t now_ms);

}  // namespace console
