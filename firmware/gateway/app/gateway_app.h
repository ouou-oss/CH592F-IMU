#ifndef GATEWAY_APP_H
#define GATEWAY_APP_H

#include "CONFIG.h"
#include "gateway_core.h"
#include "host_parser.h"

#ifndef APP_GW_ID
#define APP_GW_ID 1u
#endif
#define GW_FW_MAJOR 1u
#define GW_FW_MINOR 5u

extern gw_core_t gateway;

GW_RAM uint32_t gateway_now_us(void);
GW_RAM void gateway_host_init(void);
GW_RAM void gateway_host_poll(uint32_t now);
GW_RAM void gateway_radio_poll(uint32_t now);
GW_RAM void gateway_radio_refresh_ack(void);
void gateway_radio_init(void);

#endif
