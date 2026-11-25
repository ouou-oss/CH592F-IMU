#include "sensor.h"

/* 板型 LED 极性：1 = PA8 输出低电平点亮，0 = 输出高电平点亮。 */
void board_activity_led_set(uint8_t on)
{
#if SENSOR_PA8_LED
#if SENSOR_LED_ACTIVE_LOW
    if (on) GPIOA_ResetBits(GPIO_Pin_8);
    else GPIOA_SetBits(GPIO_Pin_8);
#else
    if (on) GPIOA_SetBits(GPIO_Pin_8);
    else GPIOA_ResetBits(GPIO_Pin_8);
#endif
#else
    (void)on;
#endif
}

void board_activity_led_toggle(void)
{
#if SENSOR_PA8_LED
    GPIOA_InverseBits(GPIO_Pin_8);
#endif
}

/* PA5/PA6 remain logic-analyzer probes. PA8 drives the board status LED. */
void board_init(void)
{
    /* Pull unused pins to prevent floating-input leakage during Sleep, keeping 32K/SWD untouched. */
    GPIOA_ModeCfg(GPIO_Pin_All & ~(GPIO_Pin_10 | GPIO_Pin_11), GPIO_ModeIN_PU);
    GPIOB_ModeCfg(GPIO_Pin_All & ~(GPIO_Pin_14 | GPIO_Pin_15), GPIO_ModeIN_PU);
    GPIOA_SetBits(GPIO_Pin_12);
    GPIOA_ModeCfg(GPIO_Pin_12 | GPIO_Pin_13 | GPIO_Pin_14 |
                  GPIO_Pin_5 | GPIO_Pin_6, GPIO_ModeOut_PP_5mA);
    GPIOA_ModeCfg(GPIO_Pin_15, GPIO_ModeIN_PU);
#if SENSOR_PA8_LED
    R16_PIN_ALTERNATE |= RB_PIN_UART1;
    GPIOB_SetBits(GPIO_Pin_13);
    GPIOB_ModeCfg(GPIO_Pin_13, GPIO_ModeOut_PP_5mA);
    board_activity_led_set(0);
    GPIOA_ModeCfg(GPIO_Pin_8, GPIO_ModeOut_PP_5mA);
#else
    GPIOA_SetBits(GPIO_Pin_9);
    GPIOA_ModeCfg(GPIO_Pin_9, GPIO_ModeOut_PP_5mA);
#endif
    GPIOA_ModeCfg(GPIO_Pin_4, GPIO_ModeIN_PD);
    GPIOA_ResetBits(GPIO_Pin_5 | GPIO_Pin_6);
    UART1_DefInit();
    SPI0_MasterDefInit();
    SPI0_CLKCfg(8);
    SPI0_DataMode(Mode0_HighBitINFront);
}

void board_fifo_interrupt_enable(void)
{
    GPIOA_ClearITFlagBit(GPIO_Pin_4);
    GPIOA_ITModeCfg(GPIO_Pin_4, GPIO_ITMode_RiseEdge);
    PFIC_EnableIRQ(GPIO_A_IRQn);
}

void board_fifo_interrupt_disable(void)
{
    R16_PA_INT_EN &= ~GPIO_Pin_4;
    PFIC_DisableIRQ(GPIO_A_IRQn);
    GPIOA_ClearITFlagBit(GPIO_Pin_4);
}

#if SENSOR_MOTION_SLEEP
void board_motion_interrupt_enable(void)
{
    GPIOA_ClearITFlagBit(GPIO_Pin_4);
    GPIOA_ITModeCfg(GPIO_Pin_4, GPIO_ITMode_HighLevel);
    PFIC_EnableIRQ(GPIO_A_IRQn);
}
#endif

__HIGH_CODE __INTERRUPT void GPIOA_IRQHandler(void)
{
    if (R16_PA_INT_IF & GPIO_Pin_4) {
#if SENSOR_MOTION_SLEEP
        if (sensor_stats.motion_sleep) {
            /* Latched BMI323 INT1 stays high until the main loop reads it. */
            R16_PA_INT_EN &= ~GPIO_Pin_4;
            R16_PA_INT_IF = GPIO_Pin_4;
            return;
        }
#endif
        R16_PA_INT_IF = GPIO_Pin_4;
        sensor_stats.irq_count++;
        tmos_set_event(sensor_task, FIFO_EVT);
    }
}
