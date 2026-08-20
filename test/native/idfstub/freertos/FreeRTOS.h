#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define configTICK_RATE_HZ 100
#define portMAX_DELAY ((TickType_t)0xffffffff)
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(((uint32_t)(ms) * (uint32_t)configTICK_RATE_HZ) / 1000U))
