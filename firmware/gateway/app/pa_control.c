/*
 * AT2401C 外部前端使能。网关只用 AUTO 接收 + 硬件自动 ACK，软件没有主动发射点，
 * 所以 TX_EN/RX_EN 不能由应用按包翻转，只能交给厂商 BLE_PAControlInit 在协议栈
 * 的 RF 使能处自动处理。
 */
#include "CONFIG.h"
#include "pa_control.h"

#if GW_BOARD_PA

#if (GW_PA_TX_PIN == GW_PA_RX_PIN)
#error "AT2401C needs separate TX_EN and RX_EN pins"
#endif

static blePaControlConfig_t pa_control;

void gateway_pa_init(void)
{
    /* 先拉低再配成输出：复位后到注册之间不能出现使能毛刺。 */
    GPIOA_ResetBits(GW_PA_TX_PIN | GW_PA_RX_PIN);
    GPIOA_ModeCfg(GW_PA_TX_PIN | GW_PA_RX_PIN, GPIO_ModeOut_PP_5mA);

    pa_control.txEnableGPIO = (uint32_t)&R32_PA_OUT;
    pa_control.txDisableGPIO = (uint32_t)&R32_PA_CLR;
    pa_control.tx_pin = GW_PA_TX_PIN;
    pa_control.rxEnableGPIO = (uint32_t)&R32_PA_OUT;
    pa_control.rxDisableGPIO = (uint32_t)&R32_PA_CLR;
    pa_control.rx_pin = GW_PA_RX_PIN;
    BLE_PAControlInit(&pa_control);
}

GW_RAM void gateway_pa_off(void)
{
    GPIOA_ResetBits(GW_PA_TX_PIN | GW_PA_RX_PIN);
}

#else

/* 原板无外部前端；保留同一调用路径，radio.c 不再需要条件编译。 */
void gateway_pa_init(void)
{
}

GW_RAM void gateway_pa_off(void)
{
}

#endif
