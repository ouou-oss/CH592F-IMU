#include "sensor.h"
#include "HAL.h"
#include <assert.h>
#include <stdio.h>

enum { NO_IRQ, DURING_ARM, DURING_WAIT_ENTRY, RTC_SW_AT_ARM, RTC_RAW_AT_ARM, RTC_AFTER_WDOG,
       MOTION_BEFORE_SLEEP };
test_pfic_t test_pfic;
volatile uint16_t R16_PA_INT_IF;
volatile uint8_t R8_RTC_MODE_CTRL, R8_RTC_FLAG_CTRL;
volatile uint8_t R8_SLP_WAKE_CTRL;
volatile uint32_t RTCTigFlag, R32_RTC_TRIG;
volatile uint8_t R8_BAT_DET_CTRL, R8_XT32M_TUNE, R8_SLP_POWER_CTRL;
volatile uint16_t R16_POWER_PLAN;
sensor_stats_t sensor_stats;
static uint32_t rtc, compare, arm_latency, sys_clock = 60000000u;
static unsigned irq_phase, writes, waits, slept, deep_slept, latched, rtc_irq, unlocked, irq_masked, mac_reads, mac_mismatch, wdog_disabled;
static unsigned wdog_enabled = 1, wdog_fed, radio_reinit_marks, hse_restored, clock_restored;
static uint8_t tx_busy, identify_active, rtc_settle_tick;
static uint32_t motion_pin;

uint32_t GPIOA_ReadPortPin(uint32_t pins) { return motion_pin & pins; }

uint32_t sensor_rtc(void)
{
    uint32_t value = rtc;
    if (rtc_settle_tick && --rtc_settle_tick == 0)
        rtc = (rtc + 1u) % RTC_MAX_COUNT;
    return value;
}
uint32_t sensor_elapsed(uint32_t now, uint32_t then)
{
    return (now + (uint64_t)RTC_MAX_COUNT - then) % RTC_MAX_COUNT;
}
uint8_t radio_tx_busy(void) { return tx_busy; }
void radio_mark_reinit(void) { radio_reinit_marks++; }
uint8_t command_identify_active(void) { return identify_active; }
static void trigger_rtc(void)
{
    assert(irq_masked && (test_pfic.SCTLR & 16u));
    R8_RTC_FLAG_CTRL = RB_RTC_TRIG_FLAG;
    latched = 1;
}
void SYS_DisableAllIrq(uint32_t *pirqv) { assert(!irq_masked); irq_masked = 1; *pirqv = 7; }
void SYS_RecoverIrq(uint32_t irq_status)
{
    assert(irq_masked && irq_status == 7 && wdog_enabled);
    irq_masked = 0;
    if (R8_RTC_FLAG_CTRL & RB_RTC_TRIG_FLAG) RTCTigFlag = 1;
    R8_RTC_FLAG_CTRL = 0;
}
void WWDG_ResetCfg(int enable)
{
    wdog_enabled = enable ? 1u : 0u;
    if (enable) return;
    assert(irq_masked && !RTCTigFlag && !wdog_disabled);
    wdog_disabled++;
    if (irq_phase == RTC_AFTER_WDOG) trigger_rtc();
}
void WWDG_SetCounter(int count) { assert(count == 0); wdog_fed++; }
uint32_t GetSysClock(void) { return sys_clock; }
void SetSysClock(int clk) { assert(clk == CLK_SOURCE_PLL_60MHz); sys_clock = 60000000u; clock_restored++; }
void HSECFG_Current(uint8_t cur) { assert(cur == HSE_RCur_100); hse_restored++; }
void GetMACAddress(uint8_t *mac)
{
    if (mac_reads == 0) assert(!irq_masked && !writes);
    else assert(irq_masked && !wdog_enabled);
    mac[0] = (mac_reads++ == 1 && mac_mismatch) ? 2 : 1;
}
void PWR_PeriphWakeUpCfg(int enable, uint8_t sources, int delay)
{
    assert(enable && delay == Short_Delay);
    assert(sources == RB_SLP_RTC_WAKE);
}
void sys_safe_access_enable(void) { unlocked = 1; }
void sys_safe_access_disable(void) { assert(unlocked); unlocked = 0; }
void PFIC_ClearPendingIRQ(int irq) { assert(irq == RTC_IRQn); }
void PFIC_EnableIRQ(int irq)
{
    assert(irq == RTC_IRQn && !unlocked && R8_RTC_FLAG_CTRL == RB_RTC_TRIG_CLR);
    rtc_irq = 1;
}
static void service_fifo_irq(void)
{
    sensor_stats.irq_count++;
    latched = 1;
}
void RTC_SetTignTime(uint32_t deadline)
{
    assert(rtc_irq && deadline < RTC_MAX_COUNT);
    compare = R32_RTC_TRIG = deadline;
    RTCTigFlag = 0;
    writes++;
    rtc = (rtc + arm_latency) % RTC_MAX_COUNT;
    if (irq_phase == DURING_ARM) service_fifo_irq();
    if (irq_phase == RTC_SW_AT_ARM) RTCTigFlag = 1;
    if (irq_phase == RTC_RAW_AT_ARM) trigger_rtc();
}
void _SEV(void) { latched = 1; }
void _WFE(void)
{
    ++waits;
    if (waits == 1 && irq_phase == MOTION_BEFORE_SLEEP) motion_pin = GPIO_Pin_4;
    if (waits == 2 && irq_phase == DURING_WAIT_ENTRY) service_fifo_irq();
    if (latched) {
        latched = 0;
        if (test_pfic.SCTLR & 4u) rtc_settle_tick = 1;
        return;
    }
    if (test_pfic.SCTLR & 4u) {
        assert(irq_masked && !wdog_enabled);
        if (sensor_stats.motion_sleep) {
            assert(!writes && R8_SLP_WAKE_CTRL == (RB_WAKE_EV_MODE | RB_SLP_GPIO_WAKE));
            assert(!(R8_RTC_MODE_CTRL & RB_RTC_TRIG_EN));
            assert(!(R8_SLP_POWER_CTRL & (RB_WAKE_DLY_MOD | RB_RAM_RET_LV)));
            assert((R16_POWER_PLAN & RB_PWR_XROM) && !(R16_POWER_PLAN & RB_XT_PRE_EN));
            motion_pin = GPIO_Pin_4;
        } else {
            assert(writes == 1);
            assert(R16_POWER_PLAN & RB_XT_PRE_EN);
            rtc = R32_RTC_TRIG;
        }
        if (!deep_slept) {
            uint16_t retained = RB_PWR_PLAN_EN | RB_PWR_RAM2K | RB_PWR_RAM24K | RB_PWR_EXTEND | (1u << 13);
            assert((R16_POWER_PLAN & retained) == retained);
        }
        deep_slept++;
        rtc_settle_tick = mac_mismatch && deep_slept == 1 ? 0 : 1;
        sys_clock = 6400000u;
        return;
    }
    assert(waits == 2 && writes == 1 && sensor_elapsed(compare, rtc) <= 676u);
    rtc = compare; /* Model the programmed RTC interrupt waking the core. */
    slept++;
}

static void dispatch(uint32_t initial_rtc)
{
    rtc = initial_rtc;
    rtc_settle_tick = 0;
    writes = waits = slept = deep_slept = arm_latency = latched = 0;
    mac_reads = mac_mismatch = wdog_disabled = 0;
    R8_RTC_FLAG_CTRL = 0;
    R16_PA_INT_IF = 0;
    motion_pin = 0;
    RTCTigFlag = 0;
    irq_phase = NO_IRQ;
    sensor_idle_prepare();
}

int main(void)
{
    test_pfic.SCTLR = 0x06;
    R8_RTC_MODE_CTRL = RB_RTC_IGNORE_B0;
    HAL_SleepInit();
    assert(!test_pfic.SCTLR && rtc_irq && R8_RTC_MODE_CTRL == RB_RTC_TRIG_EN);

    /* Active streaming mode uses shallow Idle only. */
    sensor_stats.streaming = 1;
    dispatch(1000);
    assert(CH59x_LowPower(1655) != 0 && writes == 1 && slept == 1 && !deep_slept && rtc == 1655);
    dispatch(1000);
    assert(CH59x_LowPower(1004) != 0 && slept == 1);
    dispatch(RTC_MAX_COUNT - 100);
    assert(CH59x_LowPower(10) != 0 && slept == 1 && rtc == 10);
    dispatch(RTC_MAX_COUNT - 100);
    assert(CH59x_LowPower(RTC_MAX_COUNT) != 0 && slept == 1 && compare == 0);

    dispatch(1000);
    assert(CH59x_LowPower(RTC_MAX_COUNT + 1u) != 0 && !writes);
    assert(CH59x_LowPower(999) != 0 && !writes);
    assert(CH59x_LowPower(1000) != 0 && !writes);
    assert(CH59x_LowPower(1003) != 0 && !writes);
    assert(CH59x_LowPower(1677) != 0 && !writes);
    dispatch(1000);
    arm_latency = 6;
    assert(CH59x_LowPower(1005) != 0 && waits == 1 && !slept);
    dispatch(1000);
    arm_latency = 2;
    assert(CH59x_LowPower(1005) != 0 && waits == 1 && !slept);

    dispatch(1000);
    service_fifo_irq(); /* TMOS might have already scanned before this ISR. */
    assert(CH59x_LowPower(1655) != 0 && !writes && !slept);
    dispatch(1000);
    R16_PA_INT_IF = GPIO_Pin_4;
    assert(CH59x_LowPower(1655) != 0 && !writes);
    dispatch(1000);
    irq_phase = DURING_ARM;
    assert(CH59x_LowPower(1655) != 0 && writes == 1 && waits == 1 && !slept);
    dispatch(1000);
    irq_phase = DURING_WAIT_ENTRY;
    assert(CH59x_LowPower(1655) != 0 && writes == 1 && waits == 2 && !slept);

    /* Standby enters deep Sleep when TX is idle and IDENTIFY is inactive. */
    sensor_stats.streaming = 0;
    tx_busy = 1;
    dispatch(1000);
    assert(CH59x_LowPower(1000 + 600) != 0 && !deep_slept && slept == 1);
    dispatch(1000);
    assert(CH59x_LowPower(66536) == 2 && !writes && !waits && wdog_enabled);
    tx_busy = 0; identify_active = 1;
    dispatch(1000);
    assert(CH59x_LowPower(1000 + 600) != 0 && !deep_slept && slept == 1);
    identify_active = 0;
    dispatch(1000);
    assert(CH59x_LowPower(1000 + 65536u) == 0 && deep_slept == 1);
    assert(compare == 1000 + 65536u - WAKE_UP_RTC_MAX_TIME);
    assert(wdog_enabled == 1 && wdog_fed == 1 && hse_restored == 1 && clock_restored == 1);
    assert(radio_reinit_marks == 1 && !(test_pfic.SCTLR & 4u));
    /* Wrapped deadline near RTC_MAX_COUNT still wakes WAKE_UP_RTC_MAX_TIME early. */
    dispatch(RTC_MAX_COUNT - 1000);
    assert(CH59x_LowPower(20) == 0 && deep_slept == 1);
    assert(compare == RTC_MAX_COUNT - WAKE_UP_RTC_MAX_TIME + 20);

    for (unsigned phase = RTC_SW_AT_ARM; phase <= RTC_RAW_AT_ARM; ++phase) {
        dispatch(1000);
        irq_phase = phase;
        assert(CH59x_LowPower(66536) == 2 && !wdog_disabled && !deep_slept && !irq_masked);
    }
    dispatch(1000);
    arm_latency = 65536;
    assert(CH59x_LowPower(66536) == 2 && !wdog_disabled && !deep_slept);
    dispatch(1000);
    irq_phase = RTC_AFTER_WDOG;
    assert(CH59x_LowPower(66536) == 0 && !deep_slept && waits == 2);
    assert(wdog_enabled && !irq_masked && RTCTigFlag);
    dispatch(1000);
    mac_mismatch = 1;
    assert(CH59x_LowPower(66536) == 0 && deep_slept == 2);
    assert(R32_RTC_TRIG == compare + 10u && wdog_enabled && !irq_masked);

    sensor_stats.motion_sleep = 1;
    R8_SLP_WAKE_CTRL = RB_SLP_RTC_WAKE;
    R8_SLP_POWER_CTRL = RB_RAM_RET_LV | 1u;
    dispatch(1000);
    assert(CH59x_LowPower(66536) == 2 && !writes && !deep_slept);
    sensor_motion_wait();
    assert(deep_slept == 1 && motion_pin && wdog_enabled && !irq_masked);
    assert(R8_SLP_WAKE_CTRL == RB_SLP_RTC_WAKE && R8_RTC_MODE_CTRL == RB_RTC_TRIG_EN);
    assert(R8_SLP_POWER_CTRL == (RB_RAM_RET_LV | 1u));
    dispatch(1000);
    irq_phase = MOTION_BEFORE_SLEEP;
    sensor_motion_wait();
    assert(motion_pin && !deep_slept && !wdog_disabled && !irq_masked);

    puts("sensor RTC/PA4 idle and standby sleep tests passed");
    return 0;
}
