#include "sensor.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>

#define BLE_MEMHEAP_SIZE 64
#define SYS_EVENT_MSG 0x8000
#define ENABLE 1
#define CLK_SOURCE_PLL_60MHz 1
static jmp_buf finished;
static unsigned scenario, watchdog, fed, configured, radio_ready, fifo_ready, tx_requested;
static unsigned timers, dcdc, led_on;
#if SENSOR_MOTION_SLEEP
static unsigned quiet, arm_ok, motion_pin, sleep_waits, status_sent, motion_irqs;
#endif
sensor_config_t sensor_config;
uint32_t test_gpio_out;
void PWR_DCDCCfg(int enable) { assert(enable); dcdc++; }
void SetSysClock(int clock) { assert(clock == CLK_SOURCE_PLL_60MHz && dcdc == DCDC_ENABLE); }
void WWDG_SetCounter(int count) { assert(count == 0); ++fed; }
void WWDG_ResetCfg(int enable) { assert(enable && fed); watchdog = 1; }
void board_init(void) { assert(watchdog); }
void board_activity_led_set(uint8_t on) { led_on = on; }
void board_activity_led_toggle(void) { led_on ^= 1; }
void config_load(void)
{
    assert(watchdog);
    if (scenario == 1) longjmp(finished, 1);
    configured = 1;
}
void CH59x_BLEInit(void) { assert(watchdog && configured); }
void HAL_Init(void) { assert(watchdog); }
uint8_t RF_RoleInit(void) { assert(watchdog); return 0; }
uint8_t TMOS_ProcessEventRegister(uint16_t (*process)(uint8_t, uint16_t))
{ assert(process); return 1; }
uint8_t *tmos_msg_receive(uint8_t task) { (void)task; return NULL; }
void tmos_msg_deallocate(uint8_t *message) { (void)message; }
uint8_t tmos_start_task(uint8_t task, uint16_t event, uint32_t delay)
{
    assert(task == 1 && radio_ready);
    if (event == POLL_EVT) assert(delay == SENSOR_POLL_TICKS);
    else assert(event == STATUS_EVT &&
                delay == (sensor_stats.streaming ? SENSOR_ACTIVE_STATUS_TICKS : SENSOR_STANDBY_STATUS_TICKS));
    timers |= event;
    return 1;
}
uint8_t tmos_stop_task(uint8_t task, uint16_t event)
{
    assert(task == 1);
    timers &= ~event;
    return 0;
}
uint8_t tmos_set_event(uint8_t task, uint16_t event) { (void)task; (void)event; return 0; }
#if HAL_SLEEP
void sensor_idle_prepare(void) { assert(!fifo_ready && tx_requested && timers == (POLL_EVT | STATUS_EVT)); }
#endif
void TMOS_SystemProcess(void) { longjmp(finished, 1); }
void radio_init(void)
{
    assert(watchdog && configured);
    if (scenario == 2) longjmp(finished, 1);
    radio_ready = 1;
}
void sampler_init(void) { assert(watchdog && radio_ready && !sensor_stats.streaming); }
void sampler_reconfigure(void) {}
void board_fifo_interrupt_enable(void) { assert(radio_ready); fifo_ready = 1; }
void board_fifo_interrupt_disable(void) { fifo_ready = 0; }
#if SENSOR_MOTION_SLEEP
uint8_t sampler_inactive(void) { return quiet; }
uint8_t sampler_arm_motion(void) { return arm_ok; }
void board_motion_interrupt_enable(void) { assert(sensor_stats.motion_sleep); motion_irqs++; }
void sensor_motion_wait(void) { sleep_waits++; }
uint32_t GPIOA_ReadPortPin(uint32_t mask) { assert(mask == GPIO_Pin_4); return motion_pin; }
uint8_t bmi_motion_detected(void) { return motion_pin != 0; }
#endif
void app_stack_measure(void) { assert(radio_ready); }
void sampler_poll(void) {}
void config_uart_poll(void) {}
void command_poll(void) {}
uint8_t command_identify_active(void) { return 0; }
void sampler_health(void) {}
void status_emit(void)
{
#if SENSOR_MOTION_SLEEP
    status_sent++;
    tx_requested = 1;
#endif
}
void radio_request_tx(void) { assert(radio_ready); tx_requested = 1; }
void radio_process(void) {}
uint8_t radio_tx_busy(void) { return tx_requested != 0; }
void radio_enter_standby_drain(void) {}

#define main sensor_main
#include "../../firmware/sensor/app/main.c"
#undef main

int main(void)
{
    for (scenario = 0; scenario < 3; ++scenario) {
        watchdog = fed = configured = radio_ready = fifo_ready = tx_requested = timers = dcdc = 0;
        if (!setjmp(finished)) sensor_main();
        assert(watchdog && fed);
        if (!scenario) assert(radio_ready);
    }
    tx_requested = 0;
    poll_running = 0;
    radio_ready = 1;
    sensor_task = 1;
    sensor_sync_timers();
    assert((timers & POLL_EVT) == ((!HAL_SLEEP || !SENSOR_STANDBY_SLEEP) ? POLL_EVT : 0));
    timers &= ~POLL_EVT; /* Time out the scheduled event before dispatch. */
    unsigned before = fed;
    assert(sensor_process(sensor_task, POLL_EVT) == 0 && fed == before + 1);
    assert((timers & POLL_EVT) == ((!HAL_SLEEP || !SENSOR_STANDBY_SLEEP) ? POLL_EVT : 0));
#if SENSOR_MOTION_SLEEP
    timers = STATUS_EVT;
    sensor_stats.streaming = 1;
    assert(sensor_process(sensor_task, POLL_EVT) == 0 && !status_sent);
    quiet = 1;
    assert(sensor_process(sensor_task, POLL_EVT) == 0 && sensor_stats.streaming && !status_sent);
    arm_ok = 1;
    led_on = 1;
    assert(sensor_process(sensor_task, POLL_EVT) == 0 && sensor_stats.motion_sleep && !sensor_stats.streaming);
    assert(status_sent == 1 && motion_irqs == 1 && !(timers & STATUS_EVT) && !led_on);
    assert(sensor_process(sensor_task, STATUS_EVT) == 0 && status_sent == 1);
    sensor_service_motion_sleep();
    assert(!sleep_waits); /* Do not sleep before the RF queue settles. */
    tx_requested = 0;
    sensor_service_motion_sleep();
    assert(sleep_waits == 1 && sensor_stats.motion_sleep);
    motion_pin = GPIO_Pin_4;
    sensor_service_motion_sleep();
    assert(!sensor_stats.motion_sleep && sensor_stats.streaming && status_sent == 2 && (timers & STATUS_EVT) && led_on);
    assert(sensor_process(sensor_task, STATUS_EVT) == 0 && !led_on);
#endif
    puts("sensor startup watchdog ordering tests passed");
    return 0;
}
