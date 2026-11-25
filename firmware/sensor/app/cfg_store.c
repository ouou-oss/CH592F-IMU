#include "sensor.h"
#include "crc16.h"
#include <string.h>
#include <stdlib.h>

sensor_config_t sensor_config;
static sensor_config_t pending;
static char command[48];
static uint8_t command_used;

static uint8_t erased_record(const uint8_t *record)
{
    return proto_get_u32(record) == UINT32_MAX && proto_get_u32(record + 4) == UINT32_MAX;
}

static uint32_t boot_increment(void)
{
    uint8_t page[256] __attribute__((aligned(4)));
    uint32_t highest = 0, best_page = 0x100, next_offset = 0;
    for (uint32_t address = 0x100; address <= 0x200; address += 0x100) {
        EEPROM_READ(address, page, sizeof(page));
        for (unsigned offset = 0; offset < sizeof(page); offset += 8) {
            if (proto_get_u16(page + offset) != 0xb007 ||
                crc16_ccitt(page + offset, 6) != proto_get_u16(page + offset + 6)) continue;
            uint32_t value = proto_get_u32(page + offset + 2);
            if (value >= highest) { highest = value; best_page = address; next_offset = offset + 8; }
        }
    }
    EEPROM_READ(best_page, page, sizeof(page));
    while (next_offset < sizeof(page) && !erased_record(page + next_offset))
        next_offset += 8;
    if (next_offset == sizeof(page)) {
        best_page = best_page == 0x100 ? 0x200 : 0x100; next_offset = 0;
        if (EEPROM_ERASE(best_page, 256)) return UINT32_MAX;
    }
    uint8_t record[8] __attribute__((aligned(4)));
    proto_put_u16(record, 0xb007); proto_put_u32(record + 2, highest + 1u);
    proto_put_u16(record + 6, crc16_ccitt(record, 6));
    if (EEPROM_WRITE(best_page + next_offset, record, sizeof(record))) return UINT32_MAX;
    uint8_t verify[8] __attribute__((aligned(4)));
    EEPROM_READ(best_page + next_offset, verify, sizeof(verify));
    return memcmp(record, verify, sizeof(record)) ? UINT32_MAX : highest + 1u;
}

static uint8_t valid_frequency(unsigned long khz)
{
    return khz >= 2400000u && khz <= 2483500u &&
           khz != 2402000u && khz != 2426000u && khz != 2480000u;
}

void config_load(void)
{
    uint8_t record[20] __attribute__((aligned(4)));
    sensor_config.node = 1; sensor_config.odr = 13; sensor_config.range = 3;
    sensor_config.bandwidth = 1; sensor_config.power = LL_TX_PWR_0_DBM;
    sensor_config.frequency = SENSOR_RF_FREQUENCY;
    EEPROM_READ(0, record, sizeof(record));
    if (proto_get_u32(record) == 0x31494d42 &&
        crc16_ccitt(record, 18) == proto_get_u16(record + 18) &&
        record[4] > 0 && record[4] < 255 && record[5] >= 12 && record[5] <= 14 &&
        record[6] <= 3 && record[7] <= 1 && valid_frequency(proto_get_u32(record + 10))) {
        sensor_config.node = record[4]; sensor_config.odr = record[5];
        sensor_config.range = record[6]; sensor_config.bandwidth = record[7];
        sensor_config.power = record[8]; sensor_config.frequency = proto_get_u32(record + 10);
    }
    uint32_t boot = boot_increment();
    /* Never start RF with a reused session identifier after a flash write failure. */
    if (boot == UINT32_MAX) {
#if SENSOR_DEBUG
        PRINT("BOOT DataFlash error; acquisition halted\n");
#endif
        while (1) { __nop(); }
    }
    sensor_config.boot = (uint16_t)boot;
    pending = sensor_config;
}

uint8_t config_save(void)
{
    uint8_t record[20] __attribute__((aligned(4))) = {0};
    proto_put_u32(record, 0x31494d42);
    record[4] = pending.node; record[5] = pending.odr; record[6] = pending.range;
    record[7] = pending.bandwidth; record[8] = pending.power;
    proto_put_u32(record + 10, pending.frequency);
    proto_put_u16(record + 18, crc16_ccitt(record, 18));
    if (EEPROM_ERASE(0, 256) || EEPROM_WRITE(0, record, sizeof(record))) return 0;
    uint8_t verify[20] __attribute__((aligned(4)));
    EEPROM_READ(0, verify, sizeof(verify));
    return memcmp(record, verify, sizeof(record)) == 0;
}

static void handle_command(void)
{
    if (!strncmp(command, "SET NODE ", 9)) {
        unsigned long node = strtoul(command + 9, 0, 10);
        if (node > 0 && node < 255) pending.node = (uint8_t)node;
    } else if (!strncmp(command, "SET FREQ ", 9)) {
        unsigned long frequency = strtoul(command + 9, 0, 10);
        if (valid_frequency(frequency)) pending.frequency = frequency;
    } else if (!strcmp(command, "SAVE")) {
        RF_Shut(); packer_flush();
        if (config_save()) SYS_ResetExecute();
    }
}

void config_uart_poll(void)
{
    for (unsigned n = 0; n < 48 && (UART1_GetLinSTA() & RB_LSR_DATA_RDY); ++n) {
        uint8_t byte = UART1_RecvByte();
        if (byte == '\r' || byte == '\n') {
            command[command_used] = 0;
            if (command_used) handle_command();
            command_used = 0;
        } else if (command_used < sizeof(command) - 1) command[command_used++] = (char)byte;
    }
}
