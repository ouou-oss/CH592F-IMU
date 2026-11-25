/*
 * CH592 USB CDC 外设实现，依据 WCH EVT COM 与 StdPeriphDriver 示例。
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 */
#include "usb_cdc.h"
#include "usb_control.h"

#ifndef CDC_CLEAR_INTERRUPTS
#define CDC_CLEAR_INTERRUPTS(mask) (R8_USB_INT_FG = (mask))
#endif

__attribute__((aligned(4))) uint8_t cdc_ep0[64];
__attribute__((aligned(4))) uint8_t cdc_ep1[128];
__attribute__((aligned(4))) uint8_t cdc_ep2[8];
volatile uint8_t cdc_configured;
volatile uint8_t cdc_dtr;
volatile uint8_t cdc_tx_busy;
volatile uint8_t cdc_out_len;
volatile uint8_t cdc_out_pos;
volatile uint8_t cdc_suspended;
volatile uint32_t cdc_link_generation;

void cdc_init(void)
{
    R8_USB_CTRL = 0;
    R8_UEP4_1_MOD = RB_UEP1_RX_EN | RB_UEP1_TX_EN;
    R8_UEP2_3_MOD = RB_UEP2_TX_EN;
    R16_UEP0_DMA = (uint16_t)(uintptr_t)cdc_ep0;
    R16_UEP1_DMA = (uint16_t)(uintptr_t)cdc_ep1;
    R16_UEP2_DMA = (uint16_t)(uintptr_t)cdc_ep2;
    cdc_control_reset();
    R16_PIN_ANALOG_IE |= RB_PIN_USB_IE | RB_PIN_USB_DP_PU;
    R8_USB_CTRL = RB_UC_DEV_PU_EN | RB_UC_INT_BUSY | RB_UC_DMA_EN;
    R8_UDEV_CTRL = RB_UD_PD_DIS | RB_UD_PORT_EN;
    CDC_CLEAR_INTERRUPTS(0xFF);
    R8_USB_INT_EN = RB_UIE_TRANSFER | RB_UIE_BUS_RST | RB_UIE_SUSPEND;
    PFIC_EnableIRQ(USB_IRQn);
}

GW_RAM bool cdc_ready(void)
{
    return cdc_configured != 0 && cdc_dtr != 0 && cdc_suspended == 0;
}

GW_RAM bool cdc_tx_idle(void)
{
    return cdc_tx_busy == 0;
}

GW_RAM uint32_t cdc_generation(void)
{
    return cdc_link_generation;
}

GW_RAM bool cdc_write(const uint8_t *bytes, uint8_t count)
{
    if (!cdc_ready() || cdc_tx_busy || count > 64u) return false;
    PFIC_DisableIRQ(USB_IRQn);
    if (!cdc_ready() || cdc_tx_busy) {
        PFIC_EnableIRQ(USB_IRQn);
        return false;
    }
    gw_copy(cdc_ep1 + 64, bytes, count);
    cdc_tx_busy = 1;
    R8_UEP1_T_LEN = count;
    R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;
    PFIC_EnableIRQ(USB_IRQn);
    return true;
}

GW_RAM bool cdc_read_byte(uint8_t *byte)
{
    if (cdc_out_pos >= cdc_out_len) return false;
    /* CLEAR_FEATURE 和总线复位也会从 ISR 清理 OUT 游标。 */
    PFIC_DisableIRQ(USB_IRQn);
    if (cdc_out_pos >= cdc_out_len) {
        PFIC_EnableIRQ(USB_IRQn);
        return false;
    }
    *byte = cdc_ep1[cdc_out_pos++];
    if (cdc_out_pos == cdc_out_len) {
        cdc_out_pos = 0;
        cdc_out_len = 0;
        R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_ACK;
    }
    PFIC_EnableIRQ(USB_IRQn);
    return true;
}

__INTERRUPT __HIGH_CODE void USB_IRQHandler(void)
{
    uint8_t flags = R8_USB_INT_FG;
    if (flags & RB_UIF_BUS_RST) {
        cdc_control_reset();
        CDC_CLEAR_INTERRUPTS(RB_UIF_BUS_RST | RB_UIF_TRANSFER);
        return;
    }
    if (flags & RB_UIF_TRANSFER) {
        uint8_t status = R8_USB_INT_ST;
        uint8_t request[8];
        /* 旧 EP0 IN 可能重填 DMA 缓冲，先保存同时到达的新请求。 */
        if (status & RB_UIS_SETUP_ACT) gw_copy(request, cdc_ep0, sizeof(request));
        /* 手册要求先释放待处理的 IN/OUT，再单独处理 SETUP，两次 W1C。 */
        if ((status & MASK_UIS_TOKEN) != MASK_UIS_TOKEN) {
            switch (status & (MASK_UIS_TOKEN | MASK_UIS_ENDP)) {
            case UIS_TOKEN_IN:
                cdc_control_in();
                break;
            case UIS_TOKEN_OUT:
                if (status & RB_UIS_TOG_OK) cdc_control_out();
                break;
            case UIS_TOKEN_IN | 1u:
                R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
                cdc_tx_busy = 0;
                break;
            case UIS_TOKEN_OUT | 1u:
                if (status & RB_UIS_TOG_OK) {
                    cdc_out_pos = 0;
                    uint8_t length = R8_USB_RX_LEN;
                    cdc_out_len = length > 64u ? 64u : length;
                    if (cdc_out_len != 0)
                        R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_NAK;
                }
                break;
            default:
                break;
            }
            CDC_CLEAR_INTERRUPTS(RB_UIF_TRANSFER);
        }
        if (R8_USB_INT_ST & RB_UIS_SETUP_ACT) {
            cdc_control_setup((status & RB_UIS_SETUP_ACT) ? request : cdc_ep0);
            CDC_CLEAR_INTERRUPTS(RB_UIF_TRANSFER);
        }
    }
    if (flags & RB_UIF_SUSPEND) {
        cdc_suspended = (R8_USB_MIS_ST & RB_UMS_SUSPEND) != 0;
        CDC_CLEAR_INTERRUPTS(RB_UIF_SUSPEND);
    }
}
