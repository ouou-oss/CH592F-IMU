#include "gateway_app.h"
#include "framing.h"
#include "usb_cdc.h"

typedef struct {
    uint8_t type;
    uint8_t len;
    /* 待排队控制回复最长是 TIME_RESP 的 12 B，GW_STATUS 直接编码。 */
    uint8_t payload[12];
} control_response_t;

static gw_host_parser_t parser;
static control_response_t responses[4];
static uint8_t response_head;
static uint8_t response_tail;
/* 出站最大RF_RX：246B RF + 7B封装 + 8B主机帧头尾。 */
static uint8_t frame[GW_RF_MAX + 15u];
static uint16_t frame_len;
static uint16_t frame_pos;
static uint8_t frame_from_radio;
static uint8_t sequence;
static uint8_t hello_sent;
static uint32_t link_generation;
static uint32_t last_net;
static uint32_t last_status;

GW_RAM void gateway_host_init(void)
{
    link_generation = cdc_generation();
}

static GW_RAM bool queue_response(uint8_t type, const uint8_t *payload, uint8_t len)
{
    if ((uint8_t)(response_head - response_tail) == 4u) return false;
    control_response_t *out = &responses[response_head & 3u];
    out->type = type;
    out->len = len;
    gw_copy(out->payload, payload, len);
    ++response_head;
    return true;
}

static GW_RAM uint8_t hello_payload(uint8_t *out)
{
    out[0] = APP_GW_ID;
    out[1] = GW_FW_MAJOR;
    out[2] = GW_FW_MINOR;
    proto_put_u32(out + 3, APP_RF_FREQUENCY);
    out[7] = 1;
    return 8;
}

static GW_RAM void host_command(void *context, uint8_t type,
                                const uint8_t *payload, uint16_t len)
{
    (void)context;
    uint8_t out[12];
    if (type == HOST_TIME_REQ && len == 4u) {
        gw_copy(out, payload, 4);
        proto_put_u32(out + 4, gateway_now_us());
        proto_put_u32(out + 8, 0); /* 选中待发帧时才填 t_resp。 */
        if (!queue_response(HOST_TIME_RESP, out, 12)) ++gateway.net_drop;
        return;
    }
    if (type == HOST_GET_INFO && len == 0) {
        uint8_t n = hello_payload(out);
        if (!queue_response(HOST_HELLO, out, n)) ++gateway.net_drop;
        return;
    }
    out[0] = type;
    out[1] = GW_COMMAND_UNSUPPORTED;
    if (type == HOST_QUEUE_DOWNLINK) {
        PFIC_DisableIRQ(BLEL_IRQn);
        out[1] = gw_queue_command(&gateway, payload, len);
        if (out[1] == GW_COMMAND_OK) gateway_radio_refresh_ack();
        PFIC_EnableIRQ(BLEL_IRQn);
    } else if (type == HOST_TIME_REQ || type == HOST_GET_INFO) out[1] = GW_COMMAND_INVALID;
    if (!queue_response(HOST_CMD_RESP, out, 2)) ++gateway.net_drop;
}

static GW_RAM void encode(uint8_t type, const uint8_t *payload, uint16_t len)
{
    frame_len = (uint16_t)host_frame_encode(frame, sizeof(frame), type, sequence++, payload, len);
    frame_pos = 0;
}

static GW_RAM void choose_frame(uint32_t now)
{
    uint8_t out[GW_RF_MAX + 7u];
    frame_from_radio = 0;
    if (!hello_sent) {
        encode(HOST_HELLO, out, hello_payload(out));
        hello_sent = 1;
        return;
    }
    if (response_head != response_tail) {
        control_response_t *response = &responses[response_tail & 3u];
        if (response->type == HOST_TIME_RESP) proto_put_u32(response->payload + 8, gateway_now_us());
        encode(response->type, response->payload, response->len);
        ++response_tail;
        return;
    }
    if ((uint32_t)(now - last_status) >= 1000000u) {
        proto_put_u32(out, gateway.rx_ok);
        proto_put_u32(out + 4, gateway.crc_err);
        proto_put_u32(out + 8, gateway.type_err);
        proto_put_u32(out + 12, gateway.queue_full);
        proto_put_u32(out + 16, gateway.net_drop);
        proto_put_u32(out + 20, gateway.rearm_lat_max_us);
        proto_put_u32(out + 24, 0); /* 保留 TCP 重连计数，CDC 为0。 */
        encode(HOST_GW_STATUS, out, 28);
        last_status = now;
        return;
    }
    const gw_rx_record_t *record = gw_queue_peek(&gateway);
    if (record == NULL) return;
    out[0] = APP_GW_ID;
    proto_put_u32(out + 1, record->rx_us);
    out[5] = (uint8_t)record->rssi;
    out[6] = record->len;
    gw_copy(out + 7, record->payload, record->len);
    encode(HOST_RF_RX, out, (uint16_t)(record->len + 7u));
    frame_from_radio = 1;
}

static GW_RAM void update_link(void)
{
    uint32_t generation = cdc_generation();
    if (generation == link_generation) return;
    /* USB复位/关串口时不弹出RF队列，重连后完整重发当前帧。 */
    if (frame_len != 0 && !frame_from_radio) ++gateway.net_drop;
    frame_len = 0;
    frame_pos = 0;
    response_tail = response_head;
    parser.used = 0;
    hello_sent = 0;
    link_generation = generation;
}

GW_RAM void gateway_host_poll(uint32_t now)
{
    update_link();
    uint8_t byte;
    /* OUT端点读完才ACK：控制回复队满时反压，不执行无法回复的命令。 */
    while ((uint8_t)(response_head - response_tail) < 4u && cdc_read_byte(&byte))
        gw_host_feed(&parser, byte, host_command, NULL);
    if (!cdc_ready() || !cdc_tx_idle()) return;
    if (frame_len != 0 && frame_pos == frame_len) {
        if (frame_from_radio) gw_queue_pop(&gateway);
        frame_len = 0;
    }
    if (frame_len == 0) {
        if ((uint32_t)(now - last_net) < 625u) return;
        last_net = now;
        choose_frame(now);
    }
    if (frame_len == 0) return;
    uint16_t remaining = (uint16_t)(frame_len - frame_pos);
    uint8_t chunk = remaining > 64u ? 64u : (uint8_t)remaining;
    if (cdc_write(frame + frame_pos, chunk)) frame_pos = (uint16_t)(frame_pos + chunk);
}
