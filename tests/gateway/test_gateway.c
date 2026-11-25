#include "gateway_core.h"
#include "host_parser.h"
#include "framing.h"
#include "crc16.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t packet[246];
static uint8_t ack[GW_ACK_MAX];
static gw_core_t core;

static void data(uint16_t boot, uint32_t seq)
{
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x11;
    packet[1] = 1;
    packet[3] = 0xBD;
    proto_put_u16(packet + 4, boot);
    proto_put_u32(packet + 6, seq);
    packet[18] = 35;
    packet[20] = 1;
}

static void test_backpressure(void)
{
    gw_core_init(&core);
    for (uint32_t i = 0; i < GW_QUEUE_SLOTS; ++i) {
        data(1, i);
        assert(gw_receive(&core, i, -60, packet, sizeof(packet), 0));
        assert(gw_queue_used(&core) == i + 1u);
    }
    assert(!gw_can_rearm(&core));
    uint8_t old_conf[GW_ACK_MAX];
    gw_build_ack(&core, 1, old_conf);
    data(1, 100);
    assert(!gw_receive(&core, 100, -80, packet, sizeof(packet), 0));
    assert(core.queue_full == 1);
    assert(gw_build_ack(&core, 1, ack) == GW_ACK_MAX);
    assert(memcmp(old_conf, ack, sizeof(ack)) == 0);
    for (uint32_t i = 0; i < GW_QUEUE_SLOTS; ++i) {
        const gw_rx_record_t *record = gw_queue_peek(&core);
        assert(record && record->rx_us == i);
        assert(proto_get_u32(record->payload + 6) == i);
        gw_queue_pop(&core);
    }
    assert(gw_queue_peek(&core) == NULL && gw_can_rearm(&core));
    /* 单调u8生产/消费计数器跨255，槽位与长度仍正确。 */
    for (uint32_t i = 0; i < 1024; ++i) {
        data(1, i);
        assert(gw_receive(&core, i, -30, packet, sizeof(packet), 0));
        assert(gw_queue_peek(&core)->rx_us == i);
        gw_queue_pop(&core);
    }
}

static void test_confirmation_and_boot(void)
{
    gw_core_init(&core);
    for (uint32_t i = 0; i < 12; ++i) {
        data(1, i);
        assert(gw_receive(&core, i, -50, packet, sizeof(packet), 0));
        gw_queue_pop(&core);
    }
    gw_build_ack(&core, 1, ack);
    assert(ack[14] == 8);
    for (uint8_t i = 0; i < 8; ++i) assert(proto_get_u16(ack + 16u + 3u * i) == 11u - i);
    data(1, 10);
    assert(gw_receive(&core, 12, -50, packet, sizeof(packet), 0));
    gw_queue_pop(&core);
    gw_build_ack(&core, 1, ack);
    assert(ack[14] == 8 && proto_get_u16(ack + 16) == 10);
    /* 首个新boot POLL清旧表，不确认尚未分配的next_seq。 */
    packet[0] = 0x15;
    proto_put_u16(packet + 4, 2);
    assert(gw_receive(&core, 13, -50, packet, 20, 0));
    gw_queue_pop(&core);
    gw_build_ack(&core, 1, ack);
    assert(ack[14] == 0);
    data(2, 10);
    assert(gw_receive(&core, 14, -50, packet, sizeof(packet), 0));
    gw_queue_pop(&core);
    gw_build_ack(&core, 1, ack);
    assert(ack[14] == 1 && proto_get_u16(ack + 16) == 10);
}

static void test_invalid_and_commands(void)
{
    gw_core_init(&core);
    data(1, 1);
    assert(!gw_receive(&core, 0, -50, packet, sizeof(packet), 1));
    assert(!gw_receive(&core, 0, -50, packet, sizeof(packet), 2));
    assert(!gw_receive(&core, 0, -50, packet, 245, 0));
    assert(core.crc_err == 1 && core.type_err == 2 && core.conf_count == 0);
    packet[18] = 0;
    assert(!gw_receive(&core, 0, -50, packet, PROTO_DATA_HEADER, 0));
    assert(core.conf_count == 0 && gw_queue_used(&core) == 0);
    data(1, 1);
    uint8_t cmd[] = {1, 7, 2, 1, 0};
    assert(gw_queue_command(&core, cmd, sizeof(cmd)) == GW_COMMAND_OK);
    assert(gw_queue_command(&core, cmd, sizeof(cmd)) == GW_COMMAND_OK);
    gw_build_ack(&core, 1, ack);
    assert(ack[2] == 1 && ack[3] == 7 && ack[4] == 2 && ack[6] == 0);
    /* 同节点新命令覆写旧待发槽，不再返回 BUSY。 */
    cmd[1] = 8; cmd[4] = 1;
    assert(gw_queue_command(&core, cmd, sizeof(cmd)) == GW_COMMAND_OK);
    gw_build_ack(&core, 1, ack);
    assert(ack[2] == 1 && ack[3] == 8 && ack[4] == 2 && ack[6] == 1);
    packet[21] = 8;
    assert(!gw_receive(&core, 0, -50, packet, 245, 0));
    gw_build_ack(&core, 1, ack);
    assert(ack[2] == 1);
    assert(gw_receive(&core, 0, -50, packet, sizeof(packet), 0));
    gw_build_ack(&core, 1, ack);
    assert(ack[2] == 0);
    /* 节点 boot 变化时，即使通过 POLL 且旧确认项已被挤出，也必须清除旧待发命令。 */
    cmd[0] = 1; cmd[1] = 9; cmd[2] = 2; cmd[3] = 1; cmd[4] = 0;
    assert(gw_queue_command(&core, cmd, sizeof(cmd)) == GW_COMMAND_OK);
    gw_build_ack(&core, 1, ack);
    assert(ack[2] == 1 && ack[3] == 9);
    packet[0] = 0x15; packet[1] = 1; proto_put_u16(packet + 4, 2);
    assert(gw_receive(&core, 0, -50, packet, 20, 0));
    gw_queue_pop(&core);
    gw_build_ack(&core, 1, ack);
    assert(ack[2] == 0);
    /* 8 个不同节点占满后，第 9 个节点返回 BUSY。 */
    for (uint8_t n = 1; n <= GW_COMMAND_SLOTS; ++n) {
        uint8_t fill[] = {n, 1, 2, 1, 1};
        assert(gw_queue_command(&core, fill, sizeof(fill)) == GW_COMMAND_OK);
    }
    uint8_t ninth[] = {9, 1, 2, 1, 1};
    assert(gw_queue_command(&core, ninth, sizeof(ninth)) == GW_COMMAND_BUSY);
    /* 清空槽位再测其他命令校验。 */
    gw_core_init(&core);
    cmd[0] = 1; cmd[1] = 8; cmd[2] = 4; cmd[3] = 1; cmd[4] = 1;
    assert(gw_queue_command(&core, cmd, sizeof(cmd)) == GW_COMMAND_UNSUPPORTED);
    cmd[2] = 5; cmd[3] = 0;
    assert(gw_queue_command(&core, cmd, 4) == GW_COMMAND_UNSUPPORTED);
    cmd[2] = 3; cmd[3] = 1; cmd[4] = 0;
    assert(gw_queue_command(&core, cmd, 5) == GW_COMMAND_UNSUPPORTED);
    cmd[4] = 0xBF;
    assert(gw_queue_command(&core, cmd, 5) == GW_COMMAND_OK);
    cmd[0] = 0;
    assert(gw_queue_command(&core, cmd, sizeof(cmd)) == GW_COMMAND_INVALID);
}

static unsigned frame_count;
static void got_frame(void *ctx, uint8_t type, const uint8_t *payload, uint16_t len)
{
    (void)ctx;
    assert(type == HOST_TIME_REQ && len == 4 && proto_get_u32(payload) == 12345);
    ++frame_count;
}

static void got_max_frame(void *ctx, uint8_t type, const uint8_t *payload, uint16_t len)
{
    (void)ctx;
    assert(type == HOST_TIME_REQ && len == GW_HOST_RX_PAYLOAD_MAX && payload[len - 1] == 0x42);
    ++frame_count;
}

static void test_parser(void)
{
    gw_host_parser_t p = {0};
    uint8_t encoded[32], payload[4];
    proto_put_u32(payload, 12345);
    size_t n = host_frame_encode(encoded, sizeof(encoded), HOST_TIME_REQ, 1, payload, 4);
    for (size_t i = 0; i < n; ++i) gw_host_feed(&p, encoded[i], got_frame, NULL);
    assert(frame_count == 1 && p.used == 0);
    uint8_t invalid_len[] = {0xA5, 0x5A, 0x81, 0, GW_HOST_RX_PAYLOAD_MAX + 1, 0};
    for (size_t i = 0; i < sizeof(invalid_len); ++i) gw_host_feed(&p, invalid_len[i], got_frame, NULL);
    encoded[n - 1] ^= 1;
    for (size_t i = 0; i < n; ++i) gw_host_feed(&p, encoded[i], got_frame, NULL);
    encoded[n - 1] ^= 1;
    gw_host_feed(&p, 0xA5, got_frame, NULL);
    for (size_t i = 0; i < n; ++i) gw_host_feed(&p, encoded[i], got_frame, NULL);
    assert(frame_count == 2 && p.length_errors == 1 && p.crc_errors == 1);
    uint8_t max_frame[GW_HOST_RX_FRAME_MAX], max_payload[GW_HOST_RX_PAYLOAD_MAX] = {0};
    max_payload[sizeof(max_payload) - 1] = 0x42;
    n = host_frame_encode(max_frame, sizeof(max_frame), HOST_TIME_REQ, 2, max_payload, sizeof(max_payload));
    assert(n == sizeof(max_frame));
    for (size_t i = 0; i < n; ++i) gw_host_feed(&p, max_frame[i], got_max_frame, NULL);
    assert(frame_count == 3 && p.used == 0);
    uint32_t random = 1;
    for (unsigned i = 0; i < 10000; ++i) {
        random = random * 1664525u + 1013904223u;
        gw_host_feed(&p, (uint8_t)(random >> 24), got_frame, NULL);
        assert(p.used <= GW_HOST_RX_FRAME_MAX);
    }
}

static void test_time_conversion(void)
{
    uint32_t random = 1;
    for (unsigned i = 0; i < 100000; ++i) {
        random = random * 1664525u + 1013904223u;
        uint32_t high = random;
        random = random * 1664525u + 1013904223u;
        uint32_t low = random;
        uint64_t ticks = ((uint64_t)high << 32) | low;
        assert(gw_ticks_to_us(high, low) == (uint32_t)(ticks / 60u));
    }
}

int main(void)
{
    assert(crc16_ccitt((const uint8_t *)"123456789", 9) == 0x29B1);
    test_backpressure();
    test_confirmation_and_boot();
    test_invalid_and_commands();
    test_parser();
    test_time_conversion();
    puts("gateway core/parser/time tests passed");
    return 0;
}
