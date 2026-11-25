#include "sensor.h"
#include <string.h>

static uint8_t tx_buffer[248] __attribute__((aligned(4)));
static volatile uint8_t done, ack_length, ack_bytes[39], finish_seen;
static volatile int8_t ack_rssi;
static volatile uint32_t finish_rtc, finish_count;
static uint16_t exchange_count;
static uint8_t failures;
static uint8_t startup_polls = 2;
static uint8_t tx_scheduled;
static uint8_t radio_reinit_needed;
static uint8_t standby_draining;
static uint32_t standby_drain_started;
static uint8_t standby_attempt;
static int8_t rssi_history[8];
static int16_t rssi_sum;
static uint8_t rssi_count, rssi_next;

__HIGH_CODE static void radio_callback(uint8_t state, uint8_t crc, uint8_t *rx)
{
    if (state == TX_MODE_TX_FINISH) {
        finish_rtc = sensor_rtc(); finish_seen = 1; finish_count++;
        return;
    }
    if (state == TX_MODE_RX_DATA) {
        if (crc == 0 && rx && rx[1] >= 15 && rx[1] <= 39) {
            ack_rssi = (int8_t)rx[0];
            for (unsigned i = 0; i < rx[1]; ++i) ack_bytes[i] = rx[i + 2];
            ack_length = rx[1];
        }
        done = 1;
    }
    if (state == TX_MODE_TX_FAIL || state == TX_MODE_RX_TIMEOUT) done = 1;
}

/* RTC access and every callee in this wait path reside in .highcode. */
__HIGH_CODE __attribute__((noinline)) static void radio_wait(uint32_t started)
{
    while (!done && sensor_elapsed(sensor_rtc(), started) < 164u) { __nop(); }
}

/* Do not return through Flash while RF is active, even between Tx and wait. */
__HIGH_CODE __attribute__((noinline)) static uint8_t radio_exchange(uint8_t length,
                                                                  uint32_t *started)
{
    RF_Shut(); done = 0; ack_length = 0; finish_seen = 0;
    *started = sensor_rtc();
    GPIOA_SetBits(GPIO_Pin_5);
    uint8_t result = RF_Tx(tx_buffer, length, 0xff, 0xff);
    if (!result) radio_wait(*started);
    RF_Shut(); GPIOA_ResetBits(GPIO_Pin_5);
    return result;
}

void radio_init(void)
{
    rfConfig_t config;
    memset(&config, 0, sizeof(config));
    config.LLEMode = LLE_MODE_AUTO | LLE_MODE_PHY_1M | LLE_MODE_EX_CHANNEL;
    config.Frequency = sensor_config.frequency; config.accessAddress = SENSOR_RF_ACCESS;
    config.CRCInit = SENSOR_RF_CRC; config.RxMaxlen = 251; config.TxMaxlen = 251;
    config.rfStatusCB = radio_callback;
    if (RF_Config(&config)) {
#if SENSOR_DEBUG
        PRINT("RF_Config error\n");
#endif
        while (1) { __nop(); }
    }
    LL_SetTxPowerLevel(sensor_config.power);
}

static uint8_t read_ack(uint8_t *ack)
{
    uint8_t length = ack_length;
    if (length < 15 || length > 39) return 0;
    for (unsigned i = 0; i < length; ++i) ack[i] = ack_bytes[i];
    return ack[0] == 0x18 && ack[5] <= 8 && ack[14] <= 8 &&
           length == 15u + 3u * ack[14];
}

static void apply_confirmation(const uint8_t *ack, uint8_t allow_command)
{
    for (unsigned i = 0; i < ack[14]; ++i)
        packet_ring_confirm(&sensor_ring, ack[15u + i * 3u],
                            proto_get_u16(ack + 16u + i * 3u));
    packet_ring_age(&sensor_ring, exchange_count);
    sensor_stats.gateway = ack[1];
    if (rssi_count == 8) rssi_sum -= rssi_history[rssi_next];
    else rssi_count++;
    rssi_history[rssi_next] = ack_rssi; rssi_sum += ack_rssi;
    rssi_next = (uint8_t)((rssi_next + 1u) & 7u);
    sensor_stats.ack_rssi = (int8_t)(rssi_sum / rssi_count);
    if (allow_command && !sensor_stats.motion_sleep) command_apply(ack);
}

static uint8_t standby_heartbeat_mode(void)
{
    return !sensor_stats.streaming && !standby_draining;
}

static void clear_standby_status(void)
{
    for (unsigned i = 0; i < SENSOR_RING_SLOTS; ++i) {
        packet_slot_t *s = &sensor_ring.slots[i];
        if (s->state && (s->data[0] & 15u) == 2u) {
            s->state = SLOT_FREE;
            if (sensor_ring.used) sensor_ring.used--;
        }
    }
}

static void finish_standby_drain(void)
{
    standby_draining = 0;
    failures = 0;
    standby_attempt = 0;
}

static uint8_t prepare_packet(packet_slot_t **selected)
{
    *selected = !startup_polls && packet_ring_waiting(&sensor_ring) < 8 ?
                packet_ring_pick(&sensor_ring) : 0;
    if (*selected) {
        packet_slot_t *slot = *selected;
        memcpy(tx_buffer, slot->data, slot->len);
        if (!slot->attempt) sensor_stats.tx_first_attempts++;
        if (slot->attempt < 255) slot->attempt++;
        if ((tx_buffer[0] & 15u) == 1) {
            tx_buffer[20] = slot->attempt; tx_buffer[21] = sensor_stats.cmd_ack;
            proto_put_u32(tx_buffer + 30, sensor_stats.sync_rtc);
            tx_buffer[34] = sensor_stats.sync_seq; tx_buffer[35] = sensor_stats.sync_attempt;
        }
        proto_put_u32(tx_buffer + 10, packet_ring_oldest(&sensor_ring));
        return slot->len;
    }
    if (!sensor_ring.used && !startup_polls) return 0;
    memset(tx_buffer, 0, 20); packet_header(tx_buffer, 5);
    proto_put_u32(tx_buffer + 14, sensor_stats.sync_rtc);
    tx_buffer[18] = sensor_stats.sync_seq; tx_buffer[19] = sensor_stats.sync_attempt;
    return 20;
}

static uint16_t radio_transmit(void)
{
    if (standby_draining && sensor_ring.used &&
        sensor_elapsed(sensor_rtc(), standby_drain_started) >= 32768u) {
        packet_ring_drop_all(&sensor_ring);
        finish_standby_drain();
        return UINT16_MAX;
    }
    packet_slot_t *selected;
    uint8_t length = prepare_packet(&selected), ack[39];
    if (!length) {
        if (standby_draining) finish_standby_drain();
        return UINT16_MAX;
    }
    if (radio_reinit_needed) {
        radio_reinit_needed = 0;
        radio_init();
    }
    uint32_t started;
    uint8_t result = radio_exchange(length, &started);
    sensor_stats.tx_finish = finish_count;
    if (!result && read_ack(ack)) {
        uint8_t was_startup = startup_polls != 0;
        uint8_t heartbeat = standby_heartbeat_mode();
        exchange_count++; failures = 0; standby_attempt = 0; sensor_stats.tx_ok++;
        if (startup_polls) startup_polls--;
        if (selected) {
            if (selected->attempt == 1) sensor_stats.tx_first_ok++;
            if (heartbeat && (tx_buffer[0] & 15u) == 2u) {
                /* Standby STATUS heartbeat needs only one valid ACK exchange. */
                selected->state = SLOT_FREE;
                if (sensor_ring.used) sensor_ring.used--;
            } else {
                selected->state = SLOT_WAIT_CONF; selected->exchange = exchange_count;
            }
            /* POLL reuses next_seq and cannot be uniquely paired; report DATA only. */
            if ((tx_buffer[0] & 15u) == 1) {
                sensor_stats.sync_rtc = finish_seen ? finish_rtc : started;
                sensor_stats.sync_seq = (uint8_t)proto_get_u32(tx_buffer + 6);
                sensor_stats.sync_attempt = tx_buffer[20];
            }
        }
        apply_confirmation(ack, !was_startup);
        if (was_startup && !startup_polls && !sensor_stats.streaming && !sensor_ring.used)
            status_emit();
        if (standby_draining && !sensor_ring.used) {
            finish_standby_drain();
            return UINT16_MAX;
        }
        return sensor_ring.used || startup_polls ? 1 : UINT16_MAX;
    }
    sensor_stats.tx_retry++;
    if (standby_heartbeat_mode()) {
        if (++standby_attempt < 2) return 2;
        standby_attempt = 0;
        clear_standby_status();
        return UINT16_MAX;
    }
    if (standby_draining &&
        sensor_elapsed(sensor_rtc(), standby_drain_started) >= 32768u) {
        packet_ring_drop_all(&sensor_ring);
        finish_standby_drain();
        return UINT16_MAX;
    }
    if (failures < 6) failures++;
    if (failures <= 2) return 0;
    sensor_stats.tx_backoff++;
    uint16_t backoff = failures < 5 ? (uint16_t)(1u << failures) : 32;
    return backoff + (uint16_t)(sensor_rtc() & 3u);
}

uint8_t radio_tx_busy(void)
{
    return tx_scheduled || standby_draining || sensor_ring.used != 0;
}

void radio_mark_reinit(void)
{
    radio_reinit_needed = 1;
}

void radio_enter_standby_drain(void)
{
    if (sensor_stats.streaming) {
        standby_draining = 0;
        standby_attempt = 0;
        return;
    }
    standby_draining = 1;
    standby_drain_started = sensor_rtc();
    standby_attempt = 0;
}

void radio_request_tx(void)
{
    if (!tx_scheduled) tx_scheduled = tmos_set_event(sensor_task, TX_EVT) == SUCCESS;
}

void radio_process(void)
{
    /* Keep the request occupied through command_apply(), which can flush DATA. */
    uint16_t delay = radio_transmit();
    tx_scheduled = 0;
    if (delay == UINT16_MAX) {
        sensor_sync_timers();
        return;
    }
    if (delay) tx_scheduled = tmos_start_task(sensor_task, TX_EVT, delay);
    else radio_request_tx();
}
