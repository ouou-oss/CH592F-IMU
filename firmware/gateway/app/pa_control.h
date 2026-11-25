#ifndef GATEWAY_PA_CONTROL_H
#define GATEWAY_PA_CONTROL_H

#include "gateway_core.h"

/* 板级开关：BOARD=pa 时由构建参数置 1。原板没有外部前端，保持空实现。 */
#ifndef GW_BOARD_PA
#define GW_BOARD_PA 0
#endif
/* AT2401C 的 LNA 增益会抬高上报 rssi，PA 构建按实测值扣回。 */
#ifndef GW_RSSI_OFFSET_DB
#define GW_RSSI_OFFSET_DB 0
#endif
/* 引脚宏只在 pa_control.c 展开，那里已先包含 CONFIG.h。 */
#ifndef GW_PA_TX_PIN
#define GW_PA_TX_PIN GPIO_Pin_5
#endif
#ifndef GW_PA_RX_PIN
#define GW_PA_RX_PIN GPIO_Pin_4
#endif

/* 必须在 RF_RoleInit() 之后、RF 开始工作之前调用；原板为空实现。 */
void gateway_pa_init(void);
/* 真正离开接收时调用，避免 LNA 常开；重挂路径不调用，交给协议栈翻转。 */
GW_RAM void gateway_pa_off(void);

#endif
