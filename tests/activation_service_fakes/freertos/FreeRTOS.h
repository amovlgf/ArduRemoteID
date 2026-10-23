#pragma once
#include <cstdint>
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
constexpr uint32_t portMAX_DELAY=UINT32_MAX;
