#pragma once
#include <cstddef>
struct FakeQueue;
using QueueHandle_t=FakeQueue *;
QueueHandle_t xQueueCreate(unsigned,size_t);
void vQueueDelete(QueueHandle_t);
int xQueueSend(QueueHandle_t,const void *,unsigned);
int xQueueReceive(QueueHandle_t,void *,unsigned);
