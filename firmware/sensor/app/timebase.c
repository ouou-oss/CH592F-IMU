#include "sensor.h"

__HIGH_CODE uint32_t sensor_rtc(void)
{
    uint32_t value;
    do { value = R32_RTC_CNT_32K; } while (value != R32_RTC_CNT_32K);
    return value;
}

__HIGH_CODE uint32_t sensor_elapsed(uint32_t now, uint32_t then)
{
    return now >= then ? now - then : RTC_MAX_COUNT - then + now;
}
