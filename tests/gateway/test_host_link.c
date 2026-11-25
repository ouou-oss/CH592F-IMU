#include "gateway_app.h"
#include "usb_cdc.h"
#include "framing.h"
#include "crc16.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

gw_core_t gateway;
static uint32_t now;
static bool ready = true;
static bool busy;
static uint32_t generation = 1;
static uint8_t input[1024];
static size_t input_len, input_pos;
static uint8_t output[4096];
static size_t output_len;
static unsigned ack_refreshes;

uint32_t gateway_now_us(void) { return now; }
void gateway_radio_refresh_ack(void) { ++ack_refreshes; }
bool cdc_ready(void) { return ready; }
bool cdc_tx_idle(void) { return !busy; }
uint32_t cdc_generation(void) { return generation; }
bool cdc_read_byte(uint8_t *byte)
{
    if (input_pos == input_len) return false;
    *byte = input[input_pos++];
    return true;
}
bool cdc_write(const uint8_t *bytes, uint8_t count)
{
    assert(count <= 64);
    if (!ready || busy) return false;
    assert(output_len + count <= sizeof(output));
    memcpy(output + output_len, bytes, count);
    output_len += count;
    busy = true;
    return true;
}

static void poll(void)
{
    now += 625;
    gateway_host_poll(now);
}

static void drain(void)
{
    for (unsigned i = 0; i < 16; ++i) {
        busy = false;
        poll();
    }
}

static void request(uint8_t type, const uint8_t *payload, uint16_t len)
{
    input_pos = 0;
    input_len = host_frame_encode(input, sizeof(input), type, 1, payload, len);
    assert(input_len != 0);
}

static const uint8_t *find_frame(uint8_t type, uint16_t *len)
{
    for (size_t at = 0; at + 8u <= output_len;) {
        assert(output[at] == 0xA5 && output[at + 1] == 0x5A);
        uint16_t length = proto_get_u16(output + at + 4);
        assert(at + length + 8u <= output_len);
        assert(crc16_ccitt(output + at + 2, length + 4u) == proto_get_u16(output + at + 6u + length));
        if (output[at + 2] == type) {
            *len = length;
            return output + at + 6;
        }
        at += length + 8u;
    }
    return NULL;
}

static void test_hello_and_time(void)
{
    gateway_host_init();
    drain();
    uint16_t len;
    const uint8_t *hello = find_frame(HOST_HELLO, &len);
    assert(hello && len == 8 && hello[0] == 1 && hello[1] == 1 && hello[7] == 1);
    assert(proto_get_u32(hello + 3) == 2450000);
    output_len = 0;
    uint8_t req[4];
    proto_put_u32(req, 1234);
    request(HOST_TIME_REQ, req, sizeof(req));
    uint32_t sent = now;
    drain();
    const uint8_t *time = find_frame(HOST_TIME_RESP, &len);
    assert(time && len == 12 && proto_get_u32(time) == 1234);
    assert(proto_get_u32(time + 4) >= sent && proto_get_u32(time + 8) >= proto_get_u32(time + 4));
}

static void test_queue_commit_and_reconnect(void)
{
    output_len = 0;
    uint8_t rf[246] = {0};
    rf[0] = 0x11; rf[1] = 1; rf[3] = 0xBD; rf[18] = 35; rf[20] = 1;
    assert(gw_receive(&gateway, 123, -53, rf, sizeof(rf), 0));
    busy = false;
    poll();
    assert(busy && output_len == 64 && gw_queue_used(&gateway) == 1);
    /* CDC中断当前主机帧：未交付末块，队列不能释放。 */
    ready = false;
    busy = false;
    ++generation;
    poll();
    assert(gw_queue_used(&gateway) == 1);
    output_len = 0;
    ready = true;
    ++generation;
    drain();
    assert(gw_queue_used(&gateway) == 0);
    uint16_t len;
    assert(find_frame(HOST_HELLO, &len));
    const uint8_t *received = find_frame(HOST_RF_RX, &len);
    assert(received && len == 253 && received[0] == 1);
    assert(proto_get_u32(received + 1) == 123 && (int8_t)received[5] == -53 && received[6] == 246);
    assert(memcmp(received + 7, rf, sizeof(rf)) == 0);
}

static void test_commands(void)
{
    output_len = 0;
    request(HOST_SET_GW_CFG, NULL, 0);
    drain();
    uint16_t len;
    const uint8_t *response = find_frame(HOST_CMD_RESP, &len);
    assert(response && len == 2 && response[0] == HOST_SET_GW_CFG && response[1] == GW_COMMAND_UNSUPPORTED);
    output_len = 0;
    uint8_t reboot[] = {1, 8, PROTO_CMD_REBOOT, 0};
    request(HOST_QUEUE_DOWNLINK, reboot, sizeof(reboot));
    drain();
    response = find_frame(HOST_CMD_RESP, &len);
    assert(response && response[0] == HOST_QUEUE_DOWNLINK && response[1] == GW_COMMAND_UNSUPPORTED);
    output_len = 0;
    unsigned before_refresh = ack_refreshes;
    uint8_t command[] = {1, 9, PROTO_CMD_STREAM_CTRL, 1, 0};
    request(HOST_QUEUE_DOWNLINK, command, sizeof(command));
    drain();
    response = find_frame(HOST_CMD_RESP, &len);
    assert(response && response[0] == HOST_QUEUE_DOWNLINK && response[1] == GW_COMMAND_OK);
    assert(ack_refreshes == before_refresh + 1u);
    uint8_t ack[39];
    gw_build_ack(&gateway, 1, ack);
    assert(ack[2] == 1 && ack[3] == 9 && ack[4] == PROTO_CMD_STREAM_CTRL);
}

int main(void)
{
    gw_core_init(&gateway);
    test_hello_and_time();
    test_queue_commit_and_reconnect();
    test_commands();
    puts("gateway host-link tests passed");
    return 0;
}
