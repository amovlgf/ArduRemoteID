#pragma once
#include <cstdint>
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mutex) ((void)(mutex))
#define portEXIT_CRITICAL(mutex) ((void)(mutex))
constexpr int pdTRUE=1;
