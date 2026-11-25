#include "gateway_core.h"
#include "proto.h"

/* SDK真实档位；表在RAM，错误功率码不能堵住节点的待执行命令槽。 */
static GW_RAM_DATA uint8_t power_codes[] = {0x01,0x03,0x05,0x07,0x0B,0x0F,0x13,0x15,0x1B,0x23,0x2B,0x3B,0xBF};

GW_RAM uint32_t gw_ticks_to_us(uint32_t high, uint32_t low)
{
    return high * UINT32_C(71582788) + (high / 15u) * 4u + low / 60u +
           ((high % 15u) * 16u + low % 60u) / 60u;
}

GW_RAM void gw_copy(uint8_t *dst, const uint8_t *src, size_t len)
{
    for (size_t i = 0; i < len; ++i) dst[i] = src[i];
}

void gw_core_init(gw_core_t *core)
{
    uint8_t *bytes = (uint8_t *)core;
    for (size_t i = 0; i < sizeof(*core); ++i) bytes[i] = 0;
}

GW_RAM uint8_t gw_queue_used(const gw_core_t *core)
{
    return (uint8_t)(core->head - core->tail);
}

GW_RAM bool gw_can_rearm(const gw_core_t *core)
{
    return gw_queue_used(core) <= GW_QUEUE_SLOTS - 2u;
}

static GW_RAM void copy_confirm(gw_confirm_t *dst, const gw_confirm_t *src)
{
    dst->node_id = src->node_id;
    dst->boot_id = src->boot_id;
    dst->seq = src->seq;
}

static GW_RAM void clear_node_commands(gw_core_t *core, uint8_t node)
{
    for (uint8_t i = 0; i < GW_COMMAND_SLOTS; ++i)
        if (core->commands[i].node_id == node) core->commands[i].node_id = 0;
}

static GW_RAM void update_confirmations(gw_core_t *core, const uint8_t *packet, bool add)
{
    uint8_t node = packet[1];
    uint16_t boot = proto_get_u16(packet + 4);
    uint16_t seq = (uint16_t)proto_get_u32(packet + 6);
    uint8_t slot = GW_CONFIRM_SLOTS;
    for (uint8_t i = 0; i < GW_CONFIRM_SLOTS; ++i) {
        if (core->boot_nodes[i] == node) {
            if (core->boot_ids[i] != boot) {
                core->boot_ids[i] = boot;
                clear_node_commands(core, node);
            }
            slot = i;
            break;
        }
        if (core->boot_nodes[i] == 0) slot = i;
    }
    if (slot == GW_CONFIRM_SLOTS) {
        slot = core->boot_cursor;
        core->boot_cursor = (uint8_t)((slot + 1u) & (GW_CONFIRM_SLOTS - 1u));
    }
    core->boot_nodes[slot] = node;
    core->boot_ids[slot] = boot;

    uint8_t keep = 0;
    for (uint8_t i = 0; i < core->conf_count; ++i) {
        gw_confirm_t *entry = &core->confirmations[i];
        if (entry->node_id == node && (entry->boot_id != boot || (add && entry->seq == seq))) {
            if (entry->boot_id != boot) clear_node_commands(core, node);
            continue;
        }
        if (keep != i) copy_confirm(&core->confirmations[keep], entry);
        ++keep;
    }
    if (!add) {
        core->conf_count = keep;
        return;
    }
    if (keep == GW_CONFIRM_SLOTS) --keep;
    core->conf_count = (uint8_t)(keep + 1u);
    while (keep != 0) {
        copy_confirm(&core->confirmations[keep], &core->confirmations[keep - 1u]);
        --keep;
    }
    core->confirmations[0].node_id = node;
    core->confirmations[0].boot_id = boot;
    core->confirmations[0].seq = seq;
}

static GW_RAM void acknowledge_command(gw_core_t *core, const uint8_t *packet)
{
    uint8_t ack = (packet[0] & 15u) == 1u ? packet[21] : packet[49];
    for (uint8_t i = 0; i < GW_COMMAND_SLOTS; ++i) {
        gw_command_t *cmd = &core->commands[i];
        if (cmd->node_id == packet[1] && cmd->id == ack) cmd->node_id = 0;
    }
}

GW_RAM bool gw_receive(gw_core_t *core, uint32_t rx_us, int8_t rssi,
                       const uint8_t *payload, uint8_t len, uint8_t rsr)
{
    if (rsr != 0) {
        if (rsr & 1u) ++core->crc_err;
        if (rsr & 2u) ++core->type_err;
        return false;
    }
    if (len > GW_RF_MAX || !proto_rf_valid(payload, len) ||
        ((payload[0] & 15u) != 1u && (payload[0] & 15u) != 2u && (payload[0] & 15u) != 5u)) {
        ++core->type_err;
        return false;
    }
    if (gw_queue_used(core) >= GW_QUEUE_SLOTS) {
        ++core->queue_full;
        return false;
    }
    gw_rx_record_t *record = &core->records[core->head & (GW_QUEUE_SLOTS - 1u)];
    record->rx_us = rx_us;
    record->rssi = rssi;
    record->len = len;
    gw_copy(record->payload, payload, len);
    /* POLL 的 seq 是尚未分配的 next_seq，绝不可记入确认表。 */
    bool is_poll = (payload[0] & 15u) == 5u;
    update_confirmations(core, payload, !is_poll);
    if (!is_poll) acknowledge_command(core, payload);
    __asm__ volatile ("" ::: "memory");
    ++core->head;
    ++core->rx_ok;
    return true;
}

GW_RAM const gw_rx_record_t *gw_queue_peek(const gw_core_t *core)
{
    if (core->head == core->tail) return NULL;
    __asm__ volatile ("" ::: "memory");
    return &core->records[core->tail & (GW_QUEUE_SLOTS - 1u)];
}

GW_RAM void gw_queue_pop(gw_core_t *core)
{
    if (core->head != core->tail) {
        __asm__ volatile ("" ::: "memory");
        ++core->tail;
    }
}

GW_RAM uint8_t gw_build_ack(gw_core_t *core, uint8_t gw_id, uint8_t *out)
{
    for (uint8_t i = 0; i < GW_ACK_MAX; ++i) out[i] = 0;
    out[0] = 0x18;
    out[1] = gw_id;
    for (uint8_t n = 0; n < GW_COMMAND_SLOTS; ++n) {
        uint8_t i = (uint8_t)((core->command_cursor + n) % GW_COMMAND_SLOTS);
        gw_command_t *cmd = &core->commands[i];
        if (cmd->node_id == 0) continue;
        out[2] = cmd->node_id;
        out[3] = cmd->id;
        out[4] = cmd->code;
        out[5] = cmd->len;
        gw_copy(out + 6, cmd->payload, cmd->len);
        core->command_cursor = (uint8_t)((i + 1u) % GW_COMMAND_SLOTS);
        break;
    }
    out[14] = core->conf_count;
    for (uint8_t i = 0; i < core->conf_count; ++i) {
        out[15u + 3u * i] = core->confirmations[i].node_id;
        proto_put_u16(out + 16u + 3u * i, core->confirmations[i].seq);
    }
    return (uint8_t)(15u + 3u * core->conf_count);
}

static GW_RAM bool command_supported(const uint8_t *p)
{
    if (p[2] == 1u) return p[3] == 3u && p[4] >= 12u && p[4] <= 14u && p[5] <= 3u && p[6] <= 1u;
    if (p[2] == 2u) return p[3] == 1u && p[4] <= 1u;
    if (p[2] == 3u) {
        if (p[3] != 1u) return false;
        for (size_t i = 0; i < sizeof(power_codes); ++i) if (p[4] == power_codes[i]) return true;
        return false;
    }
    if (p[2] == 6u) return p[3] == 1u;
    return false;
}

GW_RAM uint8_t gw_queue_command(gw_core_t *core, const uint8_t *p, uint16_t len)
{
    if (len < 4u || p[0] == 0u || p[0] == 255u || p[1] == 0u || p[3] > 8u || len != 4u + p[3])
        return GW_COMMAND_INVALID;
    if (!command_supported(p)) return GW_COMMAND_UNSUPPORTED;
    gw_command_t *target = NULL;
    for (uint8_t i = 0; i < GW_COMMAND_SLOTS; ++i) {
        gw_command_t *cmd = &core->commands[i];
        if (cmd->node_id == p[0]) { target = cmd; break; }
        if (cmd->node_id == 0 && target == NULL) target = cmd;
    }
    if (target == NULL) return GW_COMMAND_BUSY;
    target->node_id = p[0];
    target->id = p[1];
    target->code = p[2];
    target->len = p[3];
    gw_copy(target->payload, p + 4, p[3]);
    return GW_COMMAND_OK;
}
