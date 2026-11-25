#include "sensor.h"
#include <assert.h>
#include <stdio.h>

#define __INTERRUPT
#define GPIO_Pin_All 0xffffu
#define GPIO_Pin_8 (1u << 8)
#define GPIO_Pin_9 (1u << 9)
#define GPIO_Pin_10 (1u << 10)
#define GPIO_Pin_11 (1u << 11)
#define GPIO_Pin_12 (1u << 12)
#define GPIO_Pin_13 (1u << 13)
#define GPIO_Pin_14 (1u << 14)
#define GPIO_Pin_15 (1u << 15)
#define GPIO_A_IRQn 17
#define RB_PIN_UART1 0x20u
#define GPIOA_ModeCfg(pins, mode) ((void)(pins))
#define GPIOB_ModeCfg(pins, mode) ((void)(pins))
#define GPIOB_SetBits(pins) ((void)(pins))
#define UART1_DefInit() ((void)0)
#define SPI0_MasterDefInit() ((void)0)
#define SPI0_CLKCfg(div) ((void)(div))
#define SPI0_DataMode(mode) ((void)0)
#define GPIOA_ITModeCfg(pins, mode) (R16_PA_INT_EN |= (pins))

sensor_stats_t sensor_stats;
uint8_t sensor_task;
uint32_t test_gpio_out;
static uint16_t R16_PA_INT_EN, R16_PA_INT_IF;
#if SENSOR_PA8_LED
static uint16_t R16_PIN_ALTERNATE;
#endif
static unsigned irq_enabled;
static void PFIC_EnableIRQ(int irq) { assert(irq == GPIO_A_IRQn); irq_enabled = 1; }
static void PFIC_DisableIRQ(int irq)
{
    assert(irq == GPIO_A_IRQn && !(R16_PA_INT_EN & GPIO_Pin_4));
    irq_enabled = 0;
}
static void GPIOA_ClearITFlagBit(uint16_t pins)
{
    assert(!irq_enabled);
    R16_PA_INT_IF &= ~pins;
}
uint8_t tmos_set_event(uint8_t task, uint16_t events)
{
    assert(task == sensor_task && events == FIFO_EVT);
    return 1;
}

#include "../../firmware/sensor/app/board.c"

int main(void)
{
    board_init();
#if SENSOR_PA8_LED
    assert(R16_PIN_ALTERNATE & RB_PIN_UART1);
    assert(!!(test_gpio_out & GPIO_Pin_8) == SENSOR_LED_ACTIVE_LOW);
    board_activity_led_set(1);
    assert(!!(test_gpio_out & GPIO_Pin_8) == !SENSOR_LED_ACTIVE_LOW);
    board_activity_led_toggle();
    assert(!!(test_gpio_out & GPIO_Pin_8) == SENSOR_LED_ACTIVE_LOW);
#endif
    R16_PA_INT_EN = R16_PA_INT_IF = GPIO_Pin_5;
    board_fifo_interrupt_enable();
    assert(irq_enabled && (R16_PA_INT_EN & GPIO_Pin_4));
    R16_PA_INT_IF |= GPIO_Pin_4;
    board_fifo_interrupt_disable();
    assert(!irq_enabled && R16_PA_INT_EN == GPIO_Pin_5 && R16_PA_INT_IF == GPIO_Pin_5);
    puts("sensor PA4 interrupt disable test passed");
    return 0;
}
