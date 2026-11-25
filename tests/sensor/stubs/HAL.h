#ifndef SENSOR_TEST_HAL_H
#define SENSOR_TEST_HAL_H
#include <stdint.h>

#if HAL_SLEEP
#define DISABLE 0
#define ENABLE 1
#define Short_Delay 0
#define RTC_IRQn 28
#define RTC_MAX_COUNT 0xa8c00000u
#define SYSTEM_TIME_MICROSEN 625
#define SLEEP_RTC_MIN_TIME 33u
#define WAKE_UP_RTC_MAX_TIME 53u
#define RB_SLP_RTC_WAKE 0x08u
#define RB_SLP_GPIO_WAKE 0x10u
#define RB_WAKE_EV_MODE 0x40u
#define RB_WAKE_DLY_MOD 0x03u
#define RB_PWR_XROM 0x01u
#define RB_RTC_TRIG_FLAG 0x80u
#define RB_PWR_PLAN_EN 0x8000u
#define RB_PWR_CORE 0x04u
#define RB_PWR_DCDC_EN 0x0200u
#define RB_PWR_DCDC_PRE 0x0400u
#define RB_RAM_RET_LV 0x40u
#define RB_RTC_TRIG_CLR 0x20u
#define RB_RTC_IGNORE_B0 0x08u
#define RB_RTC_TRIG_EN 0x20u
#define RB_PWR_RAM2K 0x02u
#define RB_PWR_RAM24K 0x10u
#define RB_PWR_EXTEND 0x08u
#define RB_XT_PRE_EN 0x4000u
#define HSE_RCur_100 1u
#ifndef CLK_SOURCE_PLL_60MHz
#define CLK_SOURCE_PLL_60MHz 1
#endif
typedef struct { uint32_t SCTLR; } test_pfic_t;
extern test_pfic_t test_pfic;
#define PFIC (&test_pfic)
extern volatile uint16_t R16_PA_INT_IF;
extern volatile uint8_t R8_RTC_MODE_CTRL, R8_RTC_FLAG_CTRL;
extern volatile uint8_t R8_SLP_WAKE_CTRL;
extern volatile uint32_t RTCTigFlag, R32_RTC_TRIG;
extern volatile uint8_t R8_BAT_DET_CTRL, R8_XT32M_TUNE, R8_SLP_POWER_CTRL;
extern volatile uint16_t R16_POWER_PLAN;
void GetMACAddress(uint8_t *mac);
void PWR_PeriphWakeUpCfg(int enable, uint8_t sources, int delay);
void sys_safe_access_enable(void);
void sys_safe_access_disable(void);
void PFIC_ClearPendingIRQ(int irq);
void PFIC_EnableIRQ(int irq);
void RTC_SetTignTime(uint32_t deadline);
void SYS_DisableAllIrq(uint32_t *pirqv);
void SYS_RecoverIrq(uint32_t irq_status);
void WWDG_ResetCfg(int enable);
void WWDG_SetCounter(int count);
void HSECFG_Current(uint8_t cur);
uint32_t GetSysClock(void);
void SetSysClock(int clk);
void _SEV(void);
void _WFE(void);
void HAL_SleepInit(void);
uint32_t CH59x_LowPower(uint32_t deadline);
#endif
#endif
