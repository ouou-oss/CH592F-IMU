#ifndef TEST_GATEWAY_CONFIG_H
#define TEST_GATEWAY_CONFIG_H
#include <stdint.h>
#ifdef TEST_USB_CDC
#include "usb_registers.h"
#endif
#define APP_RF_FREQUENCY 2450000UL
#define BLEL_IRQn 21
static inline void PFIC_DisableIRQ(int irq) { (void)irq; }
static inline void PFIC_EnableIRQ(int irq) { (void)irq; }
#endif
