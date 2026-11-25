#ifndef APP_PROTO_H
#define APP_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__riscv)
#define PROTO_RAM_CODE __attribute__((section(".highcode")))
#else
#define PROTO_RAM_CODE
#endif
#if defined(__GNUC__)
#define PROTO_INLINE static inline __attribute__((always_inline))
#else
#define PROTO_INLINE static inline
#endif

enum {
    PROTO_VERSION = 1,
    PROTO_DATA = 1, PROTO_STATUS = 2, PROTO_SUMMARY = 3,
    PROTO_EVENT = 4, PROTO_POLL = 5, PROTO_ACK = 8,
    PROTO_RF_MAX = 246, PROTO_DATA_HEADER = 36,
    PROTO_SAMPLE_SIZE = 6, PROTO_SAMPLES_MAX = 35,
    PROTO_STATUS_SIZE = 63, PROTO_POLL_SIZE = 20,
    PROTO_ACK_HEADER = 15, PROTO_CONF_MAX = 8, PROTO_ACK_MAX = 39,
    PROTO_CMD_MAX = 8,
    PROTO_FLAG_GAP = 1, PROTO_FLAG_FIFO_FULL = 2,
    PROTO_FLAG_CLIP = 4, PROTO_FLAG_NEW_SEGMENT = 8,
    PROTO_FLAG_RING_DROP = 16, PROTO_FLAG_LOW_BATT = 32,
    PROTO_FLAG_CFG_CHANGED = 64, PROTO_FLAG_PAUSED = 128,
    PROTO_MODE_ACTIVE = 1, PROTO_MODE_MOTION_SLEEP = 2,
    PROTO_CFG_RTC = 128,
    PROTO_CMD_SET_ACC_CFG = 1, PROTO_CMD_STREAM_CTRL = 2,
    PROTO_CMD_SET_TX_POWER = 3, PROTO_CMD_SET_MODE = 4,
    PROTO_CMD_REBOOT = 5, PROTO_CMD_IDENTIFY = 6,
    PROTO_CMD_SET_DETECT = 7,
    HOST_RF_RX = 1, HOST_TIME_RESP = 2, HOST_GW_STATUS = 3,
    HOST_CMD_RESP = 4, HOST_HELLO = 5,
    HOST_TIME_REQ = 0x81, HOST_QUEUE_DOWNLINK = 0x82,
    HOST_GET_INFO = 0x83, HOST_SET_GW_CFG = 0x84,
    HOST_PAYLOAD_MAX = 512, HOST_FRAME_MAX = 520
};

/* DATA 偏移与 docs/02 保持一致，不把 C 结构体当成线格式。 */
enum {
    PROTO_OFF_VERSION = 0, PROTO_OFF_NODE = 1,
    PROTO_OFF_FLAGS = 2, PROTO_OFF_CFG = 3,
    PROTO_OFF_BOOT = 4, PROTO_OFF_SEQ = 6,
    PROTO_OFF_OLDEST = 10, PROTO_OFF_FIRST_SAMPLE = 14,
    PROTO_OFF_SAMPLE_COUNT = 18, PROTO_OFF_SEGMENT = 19,
    PROTO_OFF_ATTEMPT = 20, PROTO_OFF_CMD_ACK = 21,
    PROTO_OFF_ANCHOR_IDX = 22, PROTO_OFF_ANCHOR_TIME = 26,
    PROTO_OFF_SYNC_TIME = 30, PROTO_OFF_SYNC_SEQ = 34,
    PROTO_OFF_SYNC_ATTEMPT = 35, PROTO_OFF_SAMPLES = 36
};

#define PROTO_RTC_MODULUS UINT32_C(0xA8C00000)

PROTO_INLINE uint16_t proto_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

PROTO_INLINE uint32_t proto_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

PROTO_INLINE void proto_put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

PROTO_INLINE void proto_put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

PROTO_RAM_CODE bool proto_rf_valid(const uint8_t *packet, size_t length);

#endif
