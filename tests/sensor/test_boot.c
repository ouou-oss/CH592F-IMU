#include "sensor.h"
#include "crc16.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Keep the production counter private; test its power-fail paths in this TU. */
#include "../../firmware/sensor/app/cfg_store.c"

static uint8_t flash[0x300];
static unsigned erase_calls;
static int partial_write = -1, partial_erase;

uint32_t EEPROM_READ(uint32_t address, void *data, uint32_t length)
{
    assert(address + length <= sizeof(flash)); memcpy(data, flash + address, length); return 0;
}
uint32_t EEPROM_WRITE(uint32_t address, void *data, uint32_t length)
{
    assert(address + length <= sizeof(flash));
    unsigned bytes = partial_write < 0 ? length : (unsigned)partial_write;
    assert(bytes <= length);
    for (unsigned i = 0; i < bytes; ++i) flash[address+i] &= ((uint8_t *)data)[i];
    return partial_write < 0 ? 0 : 1;
}
uint32_t EEPROM_ERASE(uint32_t address, uint32_t length)
{
    assert(address >= 0x100 && address % 256 == 0 && length == 256);
    assert(address + length <= sizeof(flash)); erase_calls++;
    memset(flash+address, 255, partial_erase ? length/2 : length);
    return partial_erase ? 1 : 0;
}
uint8_t UART1_GetLinSTA(void) { return 0; }
uint8_t UART1_RecvByte(void) { return 0; }
void SYS_ResetExecute(void) { assert(0); }
void RF_Shut(void) {}
void packer_flush(void) {}

static void reset_flash(void)
{
    memset(flash, 255, sizeof(flash)); erase_calls = 0;
    partial_write = -1; partial_erase = 0;
}

static void fresh_and_rollover(void)
{
    reset_flash();
    for (unsigned expected = 1; expected <= 96; ++expected)
        assert(boot_increment() == expected);
    assert(erase_calls == 2);
}

static void corrupt_pages(void)
{
    reset_flash(); memset(flash+0x100, 0, 512);
    assert(boot_increment() == 1);
    assert(erase_calls == 1); /* A page switch must erase its target only once. */
    assert(boot_increment() == 2);
    reset_flash();
    assert(boot_increment() == 1);
    memset(flash+0x108, 0, 8); /* Torn next record must not overwrite the valid one. */
    assert(boot_increment() == 2 && erase_calls == 0);
    assert(proto_get_u32(flash+0x102) == 1 && proto_get_u32(flash+0x112) == 2);
}

static void power_loss(void)
{
    uint8_t snapshot[sizeof(flash)];
    reset_flash(); assert(boot_increment() == 1); memcpy(snapshot, flash, sizeof(flash));
    for (int bytes = 0; bytes < 8; ++bytes) {
        memcpy(flash, snapshot, sizeof(flash)); partial_write = bytes;
        assert(boot_increment() == UINT32_MAX);
        partial_write = -1;
        assert(boot_increment() == 2);
    }
    reset_flash();
    for (unsigned expected=1; expected<=32; ++expected) assert(boot_increment() == expected);
    partial_erase = 1; assert(boot_increment() == UINT32_MAX);
    partial_erase = 0; assert(boot_increment() == 33);
    /* Losing power after a complete record but before use may skip, never reuse. */
    assert(boot_increment() == 34);
}

static void test_rf_frequency_policy(void)
{
    const uint32_t rejected[] = {2402000, 2426000, 2480000};
    for (unsigned i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        reset_flash();
        proto_put_u32(flash, 0x31494d42);
        flash[4] = 2; flash[5] = 13; flash[6] = 3; flash[7] = 1;
        proto_put_u32(flash + 10, rejected[i]);
        proto_put_u16(flash + 18, crc16_ccitt(flash, 18));
        config_load();
        assert(sensor_config.frequency == 2450000 && sensor_config.node == 1);
        pending.frequency = 2440000;
        snprintf(command, sizeof(command), "SET FREQ %lu", (unsigned long)rejected[i]);
        handle_command();
        assert(pending.frequency == 2440000);
        strcpy(command, "SET FREQ 2450000");
        handle_command();
        assert(pending.frequency == 2450000);
    }
}

int main(void)
{
    fresh_and_rollover(); corrupt_pages(); power_loss(); test_rf_frequency_policy();
    puts("sensor boot power-fail tests passed"); return 0;
}
