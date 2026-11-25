#include "sensor.h"
#include "bmi323.h"

static void select_sensor(void) { GPIOA_ResetBits(GPIO_Pin_12); }
static void release_sensor(void)
{
    GPIOA_SetBits(GPIO_Pin_12);
    DelayUs(2);
}

uint16_t bmi_read(uint8_t address)
{
    uint8_t lo, hi;
    select_sensor();
    SPI0_MasterSendByte(address | 0x80u);
    (void)SPI0_MasterRecvByte();
    lo = SPI0_MasterRecvByte(); hi = SPI0_MasterRecvByte();
    release_sensor();
    return (uint16_t)lo | ((uint16_t)hi << 8);
}

void bmi_write(uint8_t address, uint16_t value)
{
    select_sensor();
    SPI0_MasterSendByte(address & 0x7fu);
    SPI0_MasterSendByte((uint8_t)value);
    SPI0_MasterSendByte((uint8_t)(value >> 8));
    release_sensor();
}

void bmi_fifo_read(uint8_t *data, uint16_t bytes)
{
    select_sensor();
    SPI0_MasterSendByte(0x96);
    (void)SPI0_MasterRecvByte();
    SPI0_MasterRecv(data, bytes);
    release_sensor();
}

uint8_t bmi_initialize(uint8_t odr, uint8_t range, uint8_t bandwidth)
{
    uint16_t acc = 0x7000u | ((uint16_t)bandwidth << 7) | ((uint16_t)range << 4) | odr;
    for (unsigned retry = 0; retry < 3; ++retry) {
        DelayMs(2); (void)bmi_read(0); DelayUs(250);
        bmi_write(0x7e, 0xdeaf); DelayMs(2);
        (void)bmi_read(0); DelayUs(250);
        if ((bmi_read(0) & 0xffu) != 0x43) continue;
        if (bmi_read(1) != 0) continue;
        (void)bmi_read(2);
        /* BST-BMI323-DS000-13 §§6.1: FIFO before enabling accelerometer. */
        bmi_write(0x36, 0x0301); bmi_write(0x37, 1);
        bmi_write(0x35, 140); bmi_write(0x39, 0);
        bmi_write(0x3b, 0x1000); bmi_write(0x38, 0x0005);
        bmi_write(0x20, acc); DelayMs(5);
        sensor_stats.bmi_error = bmi_read(1);
        if (bmi_read(0x20) == acc && sensor_stats.bmi_error == 0) return 1;
    }
    sensor_stats.bmi_error = 0xffff;
    return 0;
}

uint8_t bmi_suspend(void)
{
    for (unsigned retry = 0; retry < 3; ++retry) {
        DelayMs(2); (void)bmi_read(0); DelayUs(250);
        bmi_write(0x7e, 0xdeaf); DelayMs(2);
        (void)bmi_read(0); DelayUs(250);
        if ((bmi_read(0) & 0xffu) != 0x43) continue;
        (void)bmi_read(2);
        bmi_write(0x20, 0); bmi_write(0x37, 1);
        bmi_write(0x3b, 0); bmi_write(0x38, 0);
        sensor_stats.bmi_error = bmi_read(1);
        if (bmi_read(0x20) == 0 && sensor_stats.bmi_error == 0) return 1;
    }
    bmi_write(0x20, 0); bmi_write(0x37, 1);
    bmi_write(0x3b, 0); bmi_write(0x38, 0);
    sensor_stats.bmi_error = 0xffff;
    return 0;
}

#if SENSOR_MOTION_SLEEP
static uint8_t wait_register(uint8_t reg, uint16_t mask, uint16_t expected)
{
    for (unsigned i = 0; i < 20; ++i) {
        if ((bmi_read(reg) & mask) == expected) return 1;
        DelayMs(1);
    }
    return 0;
}

static uint8_t set_motion_param(uint16_t address, uint16_t value)
{
    /* BST-BMI323-DS000-13 §6.2: one complete extended-register transfer. */
    if (!wait_register(0x43, 2, 2)) return 0;
    bmi_write(0x41, address);
    bmi_write(0x42, value);
    return (bmi_read(0x43) & 1u) == 0;
}

uint8_t bmi_arm_motion(void)
{
    bmi_write(0x7e, 0xdeaf);
    DelayMs(2); (void)bmi_read(0); DelayUs(250);
    if ((bmi_read(0) & 0xffu) != 0x43) return 0;

    /* Start the feature engine before enabling either sensor (§5.8.1). */
    bmi_write(0x12, 0x012c); bmi_write(0x14, 1); bmi_write(0x40, 1);
    if (!wait_register(0x11, 0x0f, 1)) return 0;
    /* 50 Hz, low-power mode, +/-2 g, average four samples. */
    bmi_write(0x20, 0x3207);
    if (!set_motion_param(0x05, 0x1000u | 48u) || /* 48 / 512 g, compare adjacent samples */
        !set_motion_param(0x06, 8) ||  /* 8 / 512 g hysteresis */
        !set_motion_param(0x07, 2))    /* 2 / 50 s */
        return 0;
    /* Wait for valid low-power samples before enabling the wake detector. */
    DelayMs(60);
    bmi_write(0x39, 1);      /* latched, released by reading INT_STATUS_INT1 */
    bmi_write(0x3a, 0x0004); /* Any-motion -> INT1, no FIFO interrupt */
    bmi_write(0x38, 0x0005); /* INT1 push-pull, active high */
    bmi_write(0x10, 0x0038); /* Any-motion on any of X/Y/Z */
    bmi_write(0x14, 1);
    sensor_stats.bmi_error = bmi_read(1);
    return bmi_read(0x20) == 0x3207 && sensor_stats.bmi_error == 0;
}

uint8_t bmi_motion_detected(void)
{
    return (bmi_read(0x0d) & 2u) != 0;
}
#endif
