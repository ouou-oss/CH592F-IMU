#include "sensor.h"
#include <assert.h>
#include <stdio.h>

sensor_config_t sensor_config = {.node=1};
sensor_stats_t sensor_stats;
uint32_t test_gpio_out;
static uint32_t now;
static unsigned reset_calls, reconfigure_calls, flush_calls, stream_calls, sync_calls;
static uint8_t power_result;

uint32_t sensor_rtc(void) { return now; }
uint32_t sensor_elapsed(uint32_t a, uint32_t b) { return a-b; }
void SYS_ResetExecute(void) { reset_calls++; }
void sampler_reconfigure(void) { reconfigure_calls++; }
void packer_flush(void) { flush_calls++; }
void sensor_apply_streaming_mode(uint8_t streaming) { sensor_stats.streaming = streaming; stream_calls++; }
void sensor_sync_timers(void) { sync_calls++; }
uint8_t LL_SetTxPowerLevel(uint8_t power) { (void)power; return power_result; }

static void reboot_is_rejected(void)
{
    uint8_t ack[15] = {0x18, 1, 1, 8, PROTO_CMD_REBOOT, 0};
    sensor_stats.cmd_ack = 7;
    for (unsigned i=0; i<100; ++i) command_apply(ack);
    assert(reset_calls == 0 && sensor_stats.cmd_ack == 7);
    sensor_stats.cmd_ack = 0; /* The same stale command after a new boot is harmless. */
    command_apply(ack);
    assert(reset_calls == 0 && sensor_stats.cmd_ack == 0);
}

static void configuration_is_idempotent(void)
{
    uint8_t ack[15] = {0x18, 1, 2, 9, PROTO_CMD_STREAM_CTRL, 1, 1};
    command_apply(ack); assert(!stream_calls);
    ack[2] = 1; command_apply(ack);
    assert(stream_calls == 1 && sensor_stats.streaming == 1 && sensor_stats.cmd_ack == 9);
    command_apply(ack); assert(stream_calls == 1);
    ack[3] = 10; ack[4] = PROTO_CMD_SET_ACC_CFG; ack[5] = 2;
    command_apply(ack); assert(sensor_stats.cmd_ack == 9);
    ack[5] = 3; ack[6] = 13; ack[7] = 3; ack[8] = 1; command_apply(ack);
    assert(sensor_stats.cmd_ack == 10 && sensor_config.odr == 13 && sensor_config.range == 3);
    assert(reconfigure_calls == 1 && flush_calls == 1);
    ack[3] = 11; ack[4] = PROTO_CMD_SET_MODE; ack[5] = 1; ack[6] = 0;
    command_apply(ack); assert(sensor_stats.cmd_ack == 10);
    ack[4] = PROTO_CMD_SET_TX_POWER; ack[6] = 0x15; power_result = 1;
    command_apply(ack); assert(sensor_stats.cmd_ack == 10);
    power_result = 0; command_apply(ack); assert(sensor_stats.cmd_ack == 11);
}

static void identify_has_bounded_duration(void)
{
    uint8_t ack[15] = {0x18, 1, 1, 12, PROTO_CMD_IDENTIFY, 1, 1};
    command_apply(ack); assert(sensor_stats.cmd_ack == 12 && command_identify_active());
    for (unsigned i=0; i<10; ++i) command_poll();
    assert(test_gpio_out & GPIO_Pin_6);
    now = 32768; command_poll(); assert(!(test_gpio_out & GPIO_Pin_6) && !command_identify_active());
}

int main(void)
{
    reboot_is_rejected(); configuration_is_idempotent(); identify_has_bounded_duration();
    puts("sensor command tests passed"); return 0;
}
