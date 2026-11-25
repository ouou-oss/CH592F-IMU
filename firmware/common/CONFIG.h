#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* 厂家配置保持原样；构建参数覆盖硬件与协议栈默认值。 */
#include "../sdk/BLE/HAL/include/CONFIG.h"
#include "HAL.h"
#include "RTC.h"

#ifndef APP_RF_FREQUENCY
#define APP_RF_FREQUENCY 2450000UL
#endif
#ifndef APP_RF_ACCESS
#define APP_RF_ACCESS 0x71764129UL
#endif
#ifndef APP_RF_CRC
#define APP_RF_CRC 0x123456UL
#endif

#endif
