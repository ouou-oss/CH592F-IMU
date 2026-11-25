#ifndef GATEWAY_USB_CDC_H
#define GATEWAY_USB_CDC_H

#include "gateway_core.h"

void cdc_init(void);
GW_RAM bool cdc_ready(void);
GW_RAM bool cdc_tx_idle(void);
GW_RAM bool cdc_write(const uint8_t *bytes, uint8_t count);
GW_RAM bool cdc_read_byte(uint8_t *byte);
GW_RAM uint32_t cdc_generation(void);

#endif
