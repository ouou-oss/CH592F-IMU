#ifndef GATEWAY_CORE_H
#define GATEWAY_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__riscv)
#define GW_RAM __attribute__((section(".highcode")))
#define GW_RAM_DATA __attribute__((section(".data.gateway")))
#else
#define GW_RAM
#define GW_RAM_DATA
#endif

#define GW_QUEUE_SLOTS 8u
#define GW_CONFIRM_SLOTS 8u
#define GW_COMMAND_SLOTS 8u
#define GW_RF_MAX 246u
#define GW_ACK_MAX 39u
#define GW_COMMAND_OK 0u
#define GW_COMMAND_INVALID 1u
#define GW_COMMAND_UNSUPPORTED 2u
#define GW_COMMAND_BUSY 3u

typedef struct {
    uint32_t rx_us;
    int8_t rssi;
    uint8_t len;
    uint8_t payload[GW_RF_MAX];
} gw_rx_record_t;

typedef struct {
    uint8_t node_id;
    uint16_t boot_id;
    uint16_t seq;
} gw_confirm_t;

typedef struct {
    uint8_t node_id;
    uint8_t id;
    uint8_t code;
    uint8_t len;
    uint8_t payload[8];
} gw_command_t;

typedef struct {
    gw_rx_record_t records[GW_QUEUE_SLOTS];
    volatile uint8_t head;
    volatile uint8_t tail;
    gw_confirm_t confirmations[GW_CONFIRM_SLOTS];
    uint8_t conf_count;
    gw_command_t commands[GW_COMMAND_SLOTS];
    uint8_t command_cursor;
    uint8_t boot_cursor;
    uint8_t boot_nodes[GW_CONFIRM_SLOTS];
    uint16_t boot_ids[GW_CONFIRM_SLOTS];
    volatile uint32_t rx_ok;
    volatile uint32_t crc_err;
    volatile uint32_t type_err;
    volatile uint32_t queue_full;
    uint32_t net_drop;
    uint32_t rearm_lat_max_us;
} gw_core_t;

/* producer 在 RF 回调，consumer 在主循环；tail 只在完整 USB 帧交付后推进。 */
void gw_core_init(gw_core_t *core);
GW_RAM uint8_t gw_queue_used(const gw_core_t *core);
GW_RAM bool gw_can_rearm(const gw_core_t *core);
GW_RAM bool gw_receive(gw_core_t *core, uint32_t rx_us, int8_t rssi,
                       const uint8_t *payload, uint8_t len, uint8_t rsr);
GW_RAM const gw_rx_record_t *gw_queue_peek(const gw_core_t *core);
GW_RAM void gw_queue_pop(gw_core_t *core);
/* 仅 RF 空闲、已禁 BLEL 中断时生成 ACK/写命令。 */
GW_RAM uint8_t gw_build_ack(gw_core_t *core, uint8_t gw_id, uint8_t *out);
GW_RAM uint8_t gw_queue_command(gw_core_t *core, const uint8_t *payload,
                               uint16_t len);
GW_RAM void gw_copy(uint8_t *dst, const uint8_t *src, size_t len);
GW_RAM uint32_t gw_ticks_to_us(uint32_t high, uint32_t low);

#endif
