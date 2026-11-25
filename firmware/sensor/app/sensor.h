#ifndef SENSOR_APP_H
#define SENSOR_APP_H
#include "CONFIG.h"
#include "proto.h"
#include "sample_clock.h"
#include "pkt_ring.h"
#define SENSOR_FW_MAJOR 1
#define SENSOR_FW_MINOR 7
#ifndef SENSOR_DEBUG
#define SENSOR_DEBUG APP_LOG_ENABLE
#endif
#ifndef SENSOR_AUTO_START
#define SENSOR_AUTO_START 0
#endif
#ifndef SENSOR_STANDBY_SLEEP
#define SENSOR_STANDBY_SLEEP 1
#endif
#ifndef SENSOR_PA8_LED
#define SENSOR_PA8_LED 1
#endif
#ifndef SENSOR_LED_ACTIVE_LOW
#define SENSOR_LED_ACTIVE_LOW 1
#endif
#define SENSOR_MOTION_SLEEP (HAL_SLEEP && SENSOR_STANDBY_SLEEP)
#define SENSOR_INACTIVE_TICKS (10u * 32768u)
#ifndef SENSOR_STANDBY_STATUS_TICKS
#define SENSOR_STANDBY_STATUS_TICKS 3200
#endif
#ifndef SENSOR_RF_FREQUENCY
#define SENSOR_RF_FREQUENCY APP_RF_FREQUENCY
#endif
#ifndef SENSOR_RF_ACCESS
#define SENSOR_RF_ACCESS APP_RF_ACCESS
#endif
#ifndef SENSOR_RF_CRC
#define SENSOR_RF_CRC APP_RF_CRC
#endif
enum { FIFO_EVT = 1, POLL_EVT = 2, TX_EVT = 4, STATUS_EVT = 8 };
enum { SENSOR_POLL_TICKS = 32, SENSOR_ACTIVE_STATUS_TICKS = 1600 }; /* TMOS ticks (625 us) */
typedef struct {
    uint8_t node, odr, range, bandwidth, power;
    uint16_t boot;
    uint32_t frequency;
} sensor_config_t;
typedef struct {
    uint32_t samples, tx_ok, tx_first_ok, tx_first_attempts, tx_retry, tx_backoff, tx_finish;
    volatile uint32_t irq_count;
    uint32_t fifo_gap_samples, fifo_unknown, sync_rtc;
    uint16_t fifo_full, bmi_error;
    uint8_t reinit, gateway, cmd_ack, sync_seq, sync_attempt, streaming, motion_sleep;
    int8_t ack_rssi;
} sensor_stats_t;
extern sensor_config_t sensor_config;
extern sensor_stats_t sensor_stats;
extern packet_ring_t sensor_ring;
extern uint8_t sensor_task;

uint32_t sensor_rtc(void);
uint32_t sensor_elapsed(uint32_t now, uint32_t then);
#if HAL_SLEEP
void sensor_idle_prepare(void);
#endif
#if SENSOR_MOTION_SLEEP
void sensor_motion_wait(void);
uint8_t sampler_inactive(void);
uint8_t sampler_arm_motion(void);
uint8_t bmi_motion_detected(void);
void board_motion_interrupt_enable(void);
#endif
void board_init(void);
void board_activity_led_set(uint8_t on);
void board_activity_led_toggle(void);
void board_fifo_interrupt_enable(void);
void board_fifo_interrupt_disable(void);
void config_load(void);
uint8_t config_save(void);
void config_uart_poll(void);
void sampler_init(void);
void sampler_poll(void);
void sampler_reconfigure(void);
void sampler_health(void);
uint32_t sampler_index(void);
void packer_sample(int16_t x, int16_t y, int16_t z, sample_stamp_t stamp);
void packer_flush(void);
void packer_expire(void);
void packet_header(uint8_t *data, uint8_t type);
void radio_init(void);
void radio_request_tx(void);
void radio_process(void);
uint8_t radio_tx_busy(void);
void radio_mark_reinit(void);
void radio_enter_standby_drain(void);
void status_emit(void);
void command_apply(const uint8_t *ack);
void command_poll(void);
uint8_t command_identify_active(void);
void sensor_apply_streaming_mode(uint8_t streaming);
void sensor_sync_timers(void);
#endif
