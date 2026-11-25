/*
 * USB 寄存器初始化与控制传输依据 WCH CH592 EVT COM 示例移植。
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 */
#ifndef GATEWAY_USB_CONTROL_H
#define GATEWAY_USB_CONTROL_H

#include "CONFIG.h"
#include "gateway_core.h"

extern __attribute__((aligned(4))) uint8_t cdc_ep0[64];
extern __attribute__((aligned(4))) uint8_t cdc_ep1[128];
extern __attribute__((aligned(4))) uint8_t cdc_ep2[8];
extern volatile uint8_t cdc_configured;
extern volatile uint8_t cdc_dtr;
extern volatile uint8_t cdc_tx_busy;
extern volatile uint8_t cdc_out_len;
extern volatile uint8_t cdc_out_pos;
extern volatile uint8_t cdc_suspended;
extern volatile uint32_t cdc_link_generation;

GW_RAM void cdc_control_setup(const uint8_t *request);
GW_RAM void cdc_control_in(void);
GW_RAM void cdc_control_out(void);
GW_RAM void cdc_control_reset(void);

#endif
