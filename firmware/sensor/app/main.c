#include "sensor.h"
#include "HAL.h"
#include "stack_monitor.h"

__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];
#if BLE_MAC
const uint8_t MacAddr[6] = {0x84, 0xc2, 0xe4, 0x03, 0x02, 0x02};
#endif
sensor_stats_t sensor_stats;
packet_ring_t sensor_ring;
uint8_t sensor_task;

static uint8_t poll_running;

static uint8_t sensor_needs_poll(void)
{
    return !HAL_SLEEP || !SENSOR_STANDBY_SLEEP || sensor_stats.streaming ||
           radio_tx_busy() || command_identify_active();
}

void sensor_sync_timers(void)
{
    uint8_t need_poll = sensor_needs_poll();
    if (need_poll && !poll_running) {
        tmos_start_task(sensor_task, POLL_EVT, SENSOR_POLL_TICKS);
        poll_running = 1;
    } else if (!need_poll && poll_running) {
        tmos_stop_task(sensor_task, POLL_EVT);
        poll_running = 0;
    }
    if (!sensor_stats.streaming && !command_identify_active())
        GPIOA_ResetBits(GPIO_Pin_6);
    if (!sensor_stats.streaming) board_activity_led_set(0);
}

void sensor_apply_streaming_mode(uint8_t streaming)
{
    if (sensor_stats.streaming == streaming) {
        status_emit();
        return;
    }
    sensor_stats.streaming = streaming;
    board_activity_led_set(streaming);
    sampler_reconfigure();
    radio_enter_standby_drain();
    status_emit();
    tmos_start_task(sensor_task, STATUS_EVT,
                    streaming ? SENSOR_ACTIVE_STATUS_TICKS : SENSOR_STANDBY_STATUS_TICKS);
    sensor_sync_timers();
}

#if SENSOR_MOTION_SLEEP
static void sensor_try_motion_sleep(void)
{
    if (!sensor_stats.streaming || command_identify_active() || !sampler_inactive()) return;
    if (!sampler_arm_motion()) return;
    sensor_stats.streaming = 0;
    sensor_stats.motion_sleep = 1;
    board_motion_interrupt_enable();
    tmos_stop_task(sensor_task, STATUS_EVT);
    radio_enter_standby_drain();
    status_emit();
    sensor_sync_timers();
}

__attribute__((noinline)) static void sensor_service_motion_sleep(void)
{
    if (!sensor_stats.motion_sleep || radio_tx_busy()) return;
    if (!GPIOA_ReadPortPin(GPIO_Pin_4)) sensor_motion_wait();
    if (!GPIOA_ReadPortPin(GPIO_Pin_4)) return;
    board_fifo_interrupt_disable();
    if (!bmi_motion_detected()) {
        board_motion_interrupt_enable();
        return;
    }
    sensor_stats.motion_sleep = 0;
    sensor_apply_streaming_mode(1);
}
#endif

static uint16_t sensor_process(uint8_t task, uint16_t events)
{
    if (events & SYS_EVENT_MSG) {
        uint8_t *message = tmos_msg_receive(task);
        if (message) tmos_msg_deallocate(message);
        return events ^ SYS_EVENT_MSG;
    }
    if (events & FIFO_EVT) {
        sampler_poll(); return events ^ FIFO_EVT;
    }
    if (events & POLL_EVT) {
        sampler_poll(); config_uart_poll(); command_poll(); WWDG_SetCounter(0);
#if SENSOR_MOTION_SLEEP
        sensor_try_motion_sleep();
#endif
        if (sensor_needs_poll())
            tmos_start_task(sensor_task, POLL_EVT, SENSOR_POLL_TICKS);
        else
            poll_running = 0;
        return events ^ POLL_EVT;
    }
    if (events & STATUS_EVT) {
        if (sensor_stats.motion_sleep) return events ^ STATUS_EVT;
        WWDG_SetCounter(0);
        config_uart_poll();
        sampler_health(); status_emit();
        if (sensor_stats.streaming) {
            GPIOA_InverseBits(GPIO_Pin_6);
            board_activity_led_toggle();
        }
        else if (!command_identify_active()) GPIOA_ResetBits(GPIO_Pin_6);
        tmos_start_task(sensor_task, STATUS_EVT,
                        sensor_stats.streaming ? SENSOR_ACTIVE_STATUS_TICKS : SENSOR_STANDBY_STATUS_TICKS);
        return events ^ STATUS_EVT;
    }
    if (events & TX_EVT) {
        radio_process();
        return events ^ TX_EVT;
    }
    return 0;
}

__HIGH_CODE __attribute__((noinline)) static void main_loop(void)
{
    while (1) {
#if SENSOR_MOTION_SLEEP
        sensor_service_motion_sleep();
#endif
#if HAL_SLEEP && SENSOR_STANDBY_SLEEP
        /* 待机未能进入真睡眠时，短 Idle 或忙循环也要持续喂狗。 */
        if (!poll_running) WWDG_SetCounter(0);
#endif
#if HAL_SLEEP
        sensor_idle_prepare();
#endif
        TMOS_SystemProcess();
    }
}

int main(void)
{
#if SENSOR_DEBUG
    uint8_t reset_status = SYS_GetLastResetSta();
#endif
#if DCDC_ENABLE
    PWR_DCDCCfg(ENABLE);
#endif
    SetSysClock(CLK_SOURCE_PLL_60MHz);
    /* 配置/boot 日志和 RF 初始化的失败等待都必须受看门狗保护。 */
    WWDG_SetCounter(0); WWDG_ResetCfg(ENABLE);
    board_init(); config_load();
    WWDG_SetCounter(0);
    CH59x_BLEInit();
    WWDG_SetCounter(0);
    HAL_Init(); RF_RoleInit();
    WWDG_SetCounter(0);
    sensor_task = TMOS_ProcessEventRegister(sensor_process);
    sensor_stats.streaming = SENSOR_AUTO_START;
    board_activity_led_set(sensor_stats.streaming);
    radio_init(); sampler_init();
    if (sensor_stats.streaming) board_fifo_interrupt_enable();
    app_stack_measure();
    WWDG_SetCounter(0);
    tmos_start_task(sensor_task, STATUS_EVT,
                    sensor_stats.streaming ? SENSOR_ACTIVE_STATUS_TICKS : SENSOR_STANDBY_STATUS_TICKS);
    radio_request_tx();
    sensor_sync_timers();
#if SENSOR_DEBUG
    PRINT("BMI323 A v%u.%u node=%u boot=%u reset=%u auto=%u RF=%lu RTCmod=%lu\n",
          SENSOR_FW_MAJOR, SENSOR_FW_MINOR, sensor_config.node, sensor_config.boot,
          reset_status, sensor_stats.streaming,
          (unsigned long)sensor_config.frequency, (unsigned long)RTC_MAX_COUNT);
#endif
    main_loop();
    return 0;
}
