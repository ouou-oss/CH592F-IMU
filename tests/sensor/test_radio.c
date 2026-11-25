#include "sensor.h"
#include "bmi323.h"
#include <assert.h>
#include <stdio.h>

sensor_config_t sensor_config = {.node=1, .frequency=APP_RF_FREQUENCY};
sensor_stats_t sensor_stats;
packet_ring_t sensor_ring;
uint8_t sensor_task;
static void (*callback)(uint8_t, uint8_t, uint8_t *);
static uint32_t now;
static uint8_t last_type, last_attempt, fail, confirmation;
static uint16_t confirm_seq;
uint32_t test_gpio_out;
static uint8_t active;
static int8_t receive_rssi = -45;
static uint8_t bad_ack, flush_on_ack;
static uint8_t tx_ready, timer_armed;
static uint32_t ticks, timer_due, exchanges;
static unsigned command_calls, sync_calls, rf_config_calls;
static uint8_t ack_cmd_node, last_tx_flags, last_tx_mode;
static uint16_t last_tx_temp;

uint32_t sensor_rtc(void) { return now++; }
uint32_t sensor_elapsed(uint32_t a, uint32_t b) { return a-b; }
void RF_Shut(void) { active = 0; }
void sensor_sync_timers(void) { sync_calls++; }
void command_apply(const uint8_t *ack)
{
    (void)ack;
    command_calls++;
    if (flush_on_ack) { flush_on_ack = 0; packer_flush(); }
}
uint32_t sampler_index(void) { return 0; }
uint16_t bmi_read(uint8_t address) { assert(address == 0x09 && sensor_stats.streaming); return 0x0100; }
void app_stack_measure(void) {}
uint8_t LL_SetTxPowerLevel(uint8_t power) { (void)power; return 0; }
uint8_t RF_Config(rfConfig_t *config)
{
    assert(config->Frequency == APP_RF_FREQUENCY && config->CRCInit == APP_RF_CRC);
    assert(config->RxMaxlen == 251 && config->TxMaxlen == 251);
    rf_config_calls++;
    callback = config->rfStatusCB; return 0;
}
uint8_t tmos_set_event(uint8_t task, uint16_t event)
{
    assert(task == sensor_task && event == TX_EVT);
    tx_ready = 1;
    return SUCCESS;
}
uint8_t tmos_start_task(uint8_t task, uint16_t event, uint32_t delay)
{
    assert(task == sensor_task && event == TX_EVT && delay && !timer_armed);
    timer_due = ticks + delay;
    timer_armed = 1;
    return 1;
}
static uint8_t process_ready(void)
{
    if (timer_armed && ticks >= timer_due) {
        timer_armed = 0;
        tx_ready = 1;
    }
    if (!tx_ready) return 0;
    tx_ready = 0;
    radio_process();
    return 1;
}
static uint16_t next_exchange(void)
{
    if (timer_armed && !tx_ready) ticks = timer_due;
    process_ready();
    if (tx_ready) return 0;
    return timer_armed ? (uint16_t)(timer_due - ticks) : UINT16_MAX;
}
uint8_t RF_Tx(uint8_t *data, uint8_t length, uint8_t tx_type, uint8_t rx_type)
{
    assert(tx_type == 255 && rx_type == 255 && length >= 20);
    assert(test_gpio_out & GPIO_Pin_5); active = 1; exchanges++;
    last_type = data[0] & 15; last_attempt = last_type == 1 ? data[20] : 0;
    last_tx_flags = data[2];
    last_tx_temp = last_type == 2 ? proto_get_u16(data + 20) : 0;
    last_tx_mode = last_type == 2 ? data[58] : 0;
    if (fail) { callback(TX_MODE_TX_FAIL, 0, 0); return 0; }
    uint8_t ack[41] = {0}; ack[0] = (uint8_t)receive_rssi; ack[1] = 15;
    ack[2] = 0x18; ack[3] = 1; ack[4] = ack_cmd_node;
    if (confirmation) {
        ack[1] = 18; ack[16] = 1; ack[17] = 1; proto_put_u16(ack+18, confirm_seq);
    }
    if (bad_ack == 1) ack[2] = 0x11;
    if (bad_ack == 2) ack[1] = 14;
    callback(TX_MODE_TX_FINISH, 0, 0);
    callback(TX_MODE_RX_DATA, bad_ack == 3 ? 1 : 0, ack); return 0;
}

static void rssi_window_test(void)
{
    unsigned attempts = sensor_stats.tx_first_attempts, first_ok = sensor_stats.tx_first_ok;
    status_emit(); confirmation = 0;
    assert(next_exchange() == 1 && last_type == 2);
    assert(sensor_stats.tx_first_attempts == attempts+1 && sensor_stats.tx_first_ok == first_ok+1);
    const int8_t values[8] = {127, -128, 7, -8, 10, -11, 50, -51};
    for (unsigned i=0; i<8; ++i) {
        receive_rssi = values[i]; assert(next_exchange() == 1);
    }
    assert(sensor_stats.ack_rssi == 0); /* -4 / 8 truncates toward zero. */
    receive_rssi = -80; assert(next_exchange() == 1);
    assert(sensor_stats.ack_rssi == -26); /* The oldest +127 is evicted. */
    unsigned tx_ok = sensor_stats.tx_ok;
    receive_rssi = 127;
    for (bad_ack=1; bad_ack<=3; ++bad_ack) {
        next_exchange();
        assert(sensor_stats.ack_rssi == -26 && sensor_stats.tx_ok == tx_ok);
    }
    bad_ack = 0;
    assert(sensor_stats.tx_first_attempts == attempts+1); /* Poll/retry never grows denominator. */
    confirmation = 1; confirm_seq = (uint16_t)(sensor_ring.next_seq - 1u);
    assert(next_exchange() == UINT16_MAX && !sensor_ring.used);
}

static void backoff_with_new_data_and_status(void)
{
    status_emit();
    uint16_t first = (uint16_t)(sensor_ring.next_seq - 1u);
    fail = 1;
    assert(next_exchange() == 0 && next_exchange() == 0);
    uint16_t delay = next_exchange();
    assert(delay >= 8 && delay <= 11 && timer_armed);
    uint32_t due = timer_due, sent = exchanges, index = 0;
    for (++ticks; ticks < due; ++ticks) {
        if (ticks == due - 2) status_emit();
        if (ticks % 3 == 0) {
            for (unsigned i = 0; i < 35; ++i)
                packer_sample(1, 2, 3, (sample_stamp_t){.index=index++, .rtc=now});
        }
        assert(!process_ready() && timer_due == due && exchanges == sent);
    }
    assert(sensor_ring.used >= 4);

    /* ACK can flush a partial DATA packet while the TX event is running. */
    packer_sample(1, 2, 3, (sample_stamp_t){.index=index, .rtc=now});
    flush_on_ack = 1; fail = 0; confirmation = 1; confirm_seq = first;
    uint32_t seq = sensor_ring.next_seq;
    assert(process_ready() && exchanges == sent + 1 && last_type == 2);
    assert(sensor_ring.next_seq == seq + 1 && !flush_on_ack);
    assert(!process_ready() && timer_armed && timer_due == due + 1u);
    assert(next_exchange() == 1 && exchanges == sent + 2 && last_type == 1);
}

static void standby_heartbeat_and_drain_test(void)
{
    /* Drain leftover ring first. */
    packet_ring_drop_all(&sensor_ring);
    sensor_stats.streaming = 0;
    fail = 0; confirmation = 0;
    radio_mark_reinit();
    unsigned configs = rf_config_calls;
    status_emit();
    /* Standby STATUS carries PAUSED flag, 0x8000 temp, and frees on single ACK without confirmation table. */
    assert(next_exchange() == UINT16_MAX && last_type == 2);
    assert(rf_config_calls == configs + 1u);
    assert((last_tx_flags & PROTO_FLAG_PAUSED) && last_tx_temp == 0x8000u);
    assert(!sensor_ring.used && !radio_tx_busy());

    /* Without gateway ACK, standby heartbeat retries once then drops to allow deep sleep. */
    fail = 1;
    status_emit();
    assert(next_exchange() == 2 && last_type == 2 && sensor_ring.used == 1);
    assert(next_exchange() == UINT16_MAX && sensor_ring.used == 0 && !radio_tx_busy());

    /* Standby drain drops remaining queue after 1 s timeout if link is lost. */
    uint8_t data[42] = {0x11, 1}; data[18] = 1;
    packet_ring_push(&sensor_ring, data, sizeof(data));
    radio_enter_standby_drain();
    status_emit();
    now += 40000u;
    assert(next_exchange() == UINT16_MAX && sensor_ring.used == 0 && !radio_tx_busy());
    fail = 0;
}

static void motion_sleep_requires_gateway_confirmation(void)
{
    sensor_stats.motion_sleep = 1;
    radio_enter_standby_drain();
    unsigned commands = command_calls;
    uint16_t seq = (uint16_t)sensor_ring.next_seq;
    status_emit();
    assert(next_exchange() == 1 && last_type == PROTO_STATUS && radio_tx_busy());
    assert(last_tx_mode == PROTO_MODE_MOTION_SLEEP && (last_tx_flags & PROTO_FLAG_PAUSED));
    confirm_seq = seq; confirmation = 1;
    assert(next_exchange() == UINT16_MAX && last_type == PROTO_POLL && !radio_tx_busy());
    assert(command_calls == commands);
    sensor_stats.motion_sleep = 0;
    sensor_stats.streaming = 1;
    confirmation = 0;
    status_emit();
    assert(next_exchange() == 1 && last_tx_mode == PROTO_MODE_ACTIVE && !(last_tx_flags & PROTO_FLAG_PAUSED));
}

int main(void)
{
    sensor_stats.streaming = 1;
    uint8_t data[42] = {0x11, 1}; data[18] = 1;
    packet_slot_t *slot = packet_ring_push(&sensor_ring, data, sizeof(data));
    radio_init();
    radio_request_tx();
    assert(sensor_stats.ack_rssi == 0);
    /* Startup POLLs must ignore ACK commands to avoid stale pre-reboot command execution. */
    ack_cmd_node = 1;
    receive_rssi = 4;
    assert(next_exchange() == 1 && last_type == 5 && sensor_ring.used == 1);
    assert(command_calls == 0);
    assert(sensor_stats.ack_rssi == 4 && sensor_stats.tx_first_attempts == 0);
    receive_rssi = -9;
    assert(next_exchange() == 1 && last_type == 5 && slot->attempt == 0);
    assert(command_calls == 0);
    assert(sensor_stats.ack_rssi == -2 && sensor_stats.tx_first_attempts == 0);
    receive_rssi = -8;
    assert(next_exchange() == 1 && last_type == 1 && last_attempt == 1);
    assert(command_calls == 1);
    ack_cmd_node = 0;
    assert(sensor_stats.ack_rssi == -4 && sensor_stats.tx_first_attempts == 1);
    assert(sensor_stats.tx_first_ok == 1 && sensor_stats.tx_ok == 3);
    receive_rssi = -45;
    assert(!active && !(test_gpio_out & GPIO_Pin_5));
    assert(slot->state == SLOT_WAIT_CONF && sensor_ring.used == 1);
    assert(sensor_stats.sync_attempt == 1 && sensor_stats.tx_finish == 3);
    uint32_t sync = sensor_stats.sync_rtc;
    confirmation = 1; confirm_seq = 0;
    assert(next_exchange() == UINT16_MAX && last_type == 5 && sensor_ring.used == 0);
    assert(sensor_stats.sync_rtc == sync); /* POLL never creates an ambiguous sync pair. */
    assert(next_exchange() == UINT16_MAX);
    slot = packet_ring_push(&sensor_ring, data, sizeof(data)); fail = 1;
    radio_request_tx();
    assert(next_exchange() == 0 && slot->attempt == 1 && slot->state == SLOT_QUEUED);
    assert(next_exchange() == 0 && slot->attempt == 2);
    assert(next_exchange() >= 8 && slot->attempt == 3);
    assert(!active && !(test_gpio_out & GPIO_Pin_5));
    assert(sensor_stats.tx_retry == 3 && sensor_stats.tx_backoff == 1);
    assert(sensor_stats.tx_first_attempts == 2 && sensor_stats.tx_first_ok == 1);
    fail = 0; confirmation = 0;
    assert(next_exchange() == 1 && slot->state == SLOT_WAIT_CONF);
    assert(next_exchange() == 1 && slot->state == SLOT_WAIT_CONF);
    assert(next_exchange() == 1 && slot->state == SLOT_WAIT_CONF);
    assert(next_exchange() == 1 && slot->state == SLOT_QUEUED);
    confirmation = 1; confirm_seq = 1;
    assert(next_exchange() == UINT16_MAX && sensor_ring.used == 0);
    rssi_window_test();
    backoff_with_new_data_and_status();
    standby_heartbeat_and_drain_test();
    motion_sleep_requires_gateway_confirmation();
    puts("sensor radio state and backoff tests passed"); return 0;
}
