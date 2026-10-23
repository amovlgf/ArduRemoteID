#pragma once
#include "FreeRTOS.h"
using SemaphoreHandle_t = int *;
SemaphoreHandle_t xSemaphoreCreateMutex();
int xSemaphoreTake(SemaphoreHandle_t, uint32_t);
int xSemaphoreGive(SemaphoreHandle_t);
