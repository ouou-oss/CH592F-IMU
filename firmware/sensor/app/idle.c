#include "sensor.h"
#include "HAL.h"

#if HAL_SLEEP
enum {
    IDLE_RTC_MIN = 4, /* Leave time for the 32 kHz compare to take effect. */
    IDLE_RTC_MAX = ((SENSOR_POLL_TICKS + 1u) * SYSTEM_TIME_MICROSEN * 32768u) / 1000000u + 1u,
    STANDBY_RTC_MAX = ((uint64_t)(SENSOR_STANDBY_STATUS_TICKS + 4u) * SYSTEM_TIME_MICROSEN * 32768u) / 1000000u + 1u,
    PFIC_SLEEPONEXIT = 1u << 1,
    PFIC_SLEEPDEEP = 1u << 2,
    PFIC_SEVONPEND = 1u << 4,
    POWER_PLAN_RESERVED_ONE = 1u << 13, /* CH592DS1 V1.9, R16_POWER_PLAN. */
};

static uint32_t fifo_irq_at_dispatch;

__HIGH_CODE void sensor_idle_prepare(void)
{
    fifo_irq_at_dispatch = sensor_stats.irq_count;
}

static uint8_t idle_can_wait(uint32_t deadline)
{
    uint32_t remaining = sensor_elapsed(deadline, sensor_rtc());
    if (remaining < IDLE_RTC_MIN || remaining > IDLE_RTC_MAX) return 0;
    if (!sensor_stats.streaming) return 1;
    return !(R16_PA_INT_IF & GPIO_Pin_4) && sensor_stats.irq_count == fifo_irq_at_dispatch;
}

#if SENSOR_STANDBY_SLEEP
static uint8_t standby_can_sleep(uint32_t deadline)
{
    if (sensor_stats.streaming || sensor_stats.motion_sleep || radio_tx_busy() ||
        command_identify_active()) return 0;
    uint32_t remaining = sensor_elapsed(deadline, sensor_rtc());
    return remaining >= (uint32_t)(SLEEP_RTC_MIN_TIME + WAKE_UP_RTC_MAX_TIME) &&
           remaining <= STANDBY_RTC_MAX;
}

static inline __attribute__((always_inline)) uint32_t sensor_read_mac_word(void)
{
    __attribute__((aligned(4))) uint8_t mac[6] = {0};
    GetMACAddress(mac);
    return (uint32_t)mac[0] | (uint32_t)mac[1] << 8 |
           (uint32_t)mac[2] << 16 | (uint32_t)mac[3] << 24;
}

/* CH59x_pwr.c 的节点睡眠子集。SDK 在唤醒后读 MAC 检查 Flash 是否就绪。 */
#if defined(__riscv)
__attribute__((section(".highcode.sensor_sleep")))
#endif
__attribute__((noinline))
static void sensor_sleep(uint32_t mac_before, uint8_t motion_only)
{
    sys_safe_access_enable();
    R8_BAT_DET_CTRL = 0;
    sys_safe_access_disable();
    sys_safe_access_enable();
    R8_XT32M_TUNE |= 3u;
    sys_safe_access_disable();
    sys_safe_access_enable();
    R16_POWER_PLAN &= ~RB_XT_PRE_EN;
    sys_safe_access_disable();
    PFIC->SCTLR |= PFIC_SLEEPDEEP;
    uint16_t power = R16_POWER_PLAN & (RB_PWR_DCDC_EN | RB_PWR_DCDC_PRE);
    power |= POWER_PLAN_RESERVED_ONE | RB_PWR_PLAN_EN | RB_PWR_CORE |
             RB_PWR_RAM2K | RB_PWR_RAM24K | RB_PWR_EXTEND | (2u << 11);
    sys_safe_access_enable();
    if (motion_only) {
        /* GPIO wakes asynchronously. Keep Flash powered and use the long wake delay. */
        R8_SLP_POWER_CTRL &= ~(RB_RAM_RET_LV | RB_WAKE_DLY_MOD);
        R16_POWER_PLAN = power | RB_PWR_XROM;
    } else {
        R8_SLP_POWER_CTRL |= RB_RAM_RET_LV;
        R16_POWER_PLAN = power | RB_XT_PRE_EN;
    }
    sys_safe_access_disable();
    _WFE();
    __nop();
    __nop();

    uint8_t flash_pending = sensor_read_mac_word() != mac_before;
    if (flash_pending && motion_only) {
        /* INT1 is latched. Keep the core awake while Flash comes up. */
        uint32_t started = sensor_rtc();
        while (sensor_elapsed(sensor_rtc(), started) < 10u) { __nop(); }
    } else if (flash_pending) {
        /* 清掉上一轮 RTC 事件，再给 Flash 一个短暂的恢复窗口。 */
        R8_RTC_FLAG_CTRL = RB_RTC_TRIG_CLR;
        PFIC_ClearPendingIRQ(RTC_IRQn);
        _SEV();
        _WFE();
        uint32_t retry = sensor_rtc() + 10u;
        if (retry >= RTC_MAX_COUNT) retry -= RTC_MAX_COUNT;
        sys_safe_access_enable();
        R32_RTC_TRIG = retry;
        sys_safe_access_disable();
        _WFE();
    }
    sys_safe_access_enable();
    R16_POWER_PLAN &= ~(RB_PWR_PLAN_EN | RB_XT_PRE_EN);
    sys_safe_access_disable();
    PFIC->SCTLR &= ~PFIC_SLEEPDEEP;
}

static void sleep_and_restore(uint32_t mac, uint8_t motion_only)
{
    WWDG_ResetCfg(DISABLE);
    sensor_sleep(mac, motion_only);
    HSECFG_Current(HSE_RCur_100);
    if (GetSysClock() != 60000000u) SetSysClock(CLK_SOURCE_PLL_60MHz);
    WWDG_SetCounter(0);
    WWDG_ResetCfg(ENABLE);
}

static void recover_after_sleep(void)
{
    uint32_t t = sensor_rtc();
    while (sensor_rtc() == t) { __nop(); }
    radio_mark_reinit();
}

void sensor_motion_wait(void)
{
#if SENSOR_DEBUG
    while ((R8_UART1_LSR & RB_LSR_TX_ALL_EMP) == 0) { __nop(); }
#endif
    uint32_t mac = sensor_read_mac_word();
    uint32_t irq_status;
    SYS_DisableAllIrq(&irq_status);
    uint8_t wake_sources = R8_SLP_WAKE_CTRL;
    uint8_t sleep_power = R8_SLP_POWER_CTRL;
    uint8_t rtc_mode = R8_RTC_MODE_CTRL;
    uint32_t sev_on_pend = PFIC->SCTLR & PFIC_SEVONPEND;
    PFIC->SCTLR |= PFIC_SEVONPEND;
    sys_safe_access_enable();
    R8_RTC_MODE_CTRL = rtc_mode & ~RB_RTC_TRIG_EN;
    R8_SLP_WAKE_CTRL = RB_WAKE_EV_MODE | RB_SLP_GPIO_WAKE; /* Latched INT1, high-level GPIO wake. */
    sys_safe_access_disable();
    R8_RTC_FLAG_CTRL = RB_RTC_TRIG_CLR;
    PFIC_ClearPendingIRQ(RTC_IRQn);
    _SEV();
    _WFE();
    uint8_t slept = !(GPIOA_ReadPortPin(GPIO_Pin_4) || (R16_PA_INT_IF & GPIO_Pin_4));
    if (slept) sleep_and_restore(mac, 1);
    sys_safe_access_enable();
    R8_SLP_WAKE_CTRL = wake_sources;
    R8_SLP_POWER_CTRL = sleep_power;
    R8_RTC_MODE_CTRL = rtc_mode;
    sys_safe_access_disable();
    PFIC->SCTLR = (PFIC->SCTLR & ~PFIC_SEVONPEND) | sev_on_pend;
    SYS_RecoverIrq(irq_status);
    if (slept) recover_after_sleep();
}

__attribute__((noinline)) static uint32_t standby_sleep(uint32_t deadline)
{
    uint32_t wake = deadline >= WAKE_UP_RTC_MAX_TIME
                        ? deadline - WAKE_UP_RTC_MAX_TIME
                        : RTC_MAX_COUNT - WAKE_UP_RTC_MAX_TIME + deadline;
#if SENSOR_DEBUG
    while ((R8_UART1_LSR & RB_LSR_TX_ALL_EMP) == 0) { __nop(); }
#endif
    uint32_t mac = sensor_read_mac_word();
    uint32_t irq_status;
    SYS_DisableAllIrq(&irq_status);
    if (!standby_can_sleep(deadline)) {
        SYS_RecoverIrq(irq_status);
        return 2;
    }
    /* RTC 挂起可唤醒 WFE，ISR 保持屏蔽，直到时钟和看门狗恢复。 */
    uint32_t sev_on_pend = PFIC->SCTLR & PFIC_SEVONPEND;
    PFIC->SCTLR |= PFIC_SEVONPEND;
    _SEV();
    _WFE();
    RTC_SetTignTime(wake);
    uint32_t remaining = sensor_elapsed(wake, sensor_rtc());
    uint32_t result = 2;
    if (!RTCTigFlag && !(R8_RTC_FLAG_CTRL & RB_RTC_TRIG_FLAG) &&
        remaining >= IDLE_RTC_MIN && remaining <= STANDBY_RTC_MAX) {
        sleep_and_restore(mac, 0);
        result = 0;
    }
    PFIC->SCTLR = (PFIC->SCTLR & ~PFIC_SEVONPEND) | sev_on_pend;
    SYS_RecoverIrq(irq_status);
    if (result == 0) recover_after_sleep();
    return result;
}
#endif

/* SDK hooks used by HAL_Init/CH59x_BLEInit. Keep the clocks and RF configuration. */
void HAL_SleepInit(void)
{
    PFIC->SCTLR &= ~(PFIC_SLEEPDEEP | PFIC_SLEEPONEXIT);
    PWR_PeriphWakeUpCfg(ENABLE, RB_SLP_RTC_WAKE, Short_Delay);
    sys_safe_access_enable();
    R8_RTC_MODE_CTRL = (R8_RTC_MODE_CTRL & ~RB_RTC_IGNORE_B0) | RB_RTC_TRIG_EN;
    sys_safe_access_disable();
    R8_RTC_FLAG_CTRL = RB_RTC_TRIG_CLR;
    PFIC_ClearPendingIRQ(RTC_IRQn);
    PFIC_EnableIRQ(RTC_IRQn);
}

__HIGH_CODE uint32_t CH59x_LowPower(uint32_t deadline)
{
    /* TMOS encodes the wrapped deadline 0 as ClockMaxCount. */
    if (deadline == RTC_MAX_COUNT) deadline = 0;
    if (deadline >= RTC_MAX_COUNT) return 2;
#if SENSOR_STANDBY_SLEEP
    if (standby_can_sleep(deadline)) return standby_sleep(deadline);
#endif
    if (!idle_can_wait(deadline)) return 2;

    /* Consume the old event before rechecking. An IRQ that runs after the
     * recheck leaves an event latched for the last WFE (CH592DS1 PFIC_SCTLR). */
    PFIC->SCTLR &= ~(PFIC_SLEEPDEEP | PFIC_SLEEPONEXIT);
    _SEV();
    _WFE();
    RTC_SetTignTime(deadline);
    if (idle_can_wait(deadline)) _WFE();
    return 2; /* Nonzero: TMOS must not reinitialize BB/LLE after Idle. */
}
#endif
