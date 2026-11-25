#include "gateway_app.h"
#include "pa_control.h"
#include "stack_monitor.h"

static uint8_t ack_buffers[2][GW_ACK_MAX] __attribute__((aligned(4)));
static uint8_t active_ack;
static uint8_t listening;
static volatile uint8_t rearm_requested = 1;
static volatile uint8_t callback_seen;
static volatile uint32_t callback_at;
static volatile uint32_t idle_at;
static uint32_t armed_at;
static uint32_t last_retry;
static uint32_t last_calibration;

static GW_RAM void rf_callback(uint8_t state, uint8_t rsr, uint8_t *buffer)
{
    uint32_t now = gateway_now_us();
    callback_at = now;
    callback_seen = 1;
#if !GW_BOARD_PA
    /* PA 板上 PA5 是 AT2401C 的 TX_EN，这里不能再用作测点。 */
    GPIOA_SetBits(GPIO_Pin_5);
#endif
    if (state == RX_MODE_RX_DATA) {
        if (buffer != NULL) {
#if GW_BOARD_PA && GW_RSSI_OFFSET_DB
            /* PA 板的 LNA 增益已计入 rssi，按构建常量扣回，保持新旧网关可比。 */
            int8_t rssi = (int8_t)((int8_t)buffer[0] + GW_RSSI_OFFSET_DB);
            (void)gw_receive(&gateway, now, rssi, buffer + 2, buffer[1], rsr);
#else
            (void)gw_receive(&gateway, now, (int8_t)buffer[0], buffer + 2, buffer[1], rsr);
#endif
        } else ++gateway.type_err;
    }
    if (state == RX_MODE_TX_FINISH || state == RX_MODE_TX_FAIL) {
        idle_at = now;
        rearm_requested = 1;
    }
#if !GW_BOARD_PA
    GPIOA_ResetBits(GPIO_Pin_5);
#endif
}

void gateway_radio_init(void)
{
    rfConfig_t config = {0};
    config.LLEMode = LLE_MODE_AUTO | LLE_MODE_PHY_1M | LLE_MODE_EX_CHANNEL;
    config.Channel = 0;
    config.Frequency = APP_RF_FREQUENCY;
    config.accessAddress = APP_RF_ACCESS;
    config.CRCInit = APP_RF_CRC;
    config.RxMaxlen = 251;
    config.TxMaxlen = 251;
    config.rfStatusCB = rf_callback;
#if GW_BOARD_PA
    /* 厂商要求 BLE_PAControlInit 在角色初始化之后、RF 开始工作之前调用。 */
    if (RF_RoleInit() != 0) {
        while (1) { __nop(); }
    }
    gateway_pa_init();
    if (RF_Config(&config) != 0) {
        gateway_pa_off();
        while (1) { __nop(); }
    }
#else
    if (RF_RoleInit() != 0 || RF_Config(&config) != 0) {
        /* 初始化失败不进入半工作状态，由硬件看门狗复位。 */
        while (1) { __nop(); }
    }
#endif
    last_calibration = gateway_now_us();
}

static GW_RAM void rearm(uint32_t now)
{
    PFIC_DisableIRQ(BLEL_IRQn);
    RF_Shut();
    listening = 0;
    if (!gw_can_rearm(&gateway)) {
        /* 主动背压不是重挂耗时；恢复后从下一次收包结束重新计量。 */
#if GW_BOARD_PA
        gateway_pa_off();
#endif
        idle_at = 0;
        PFIC_EnableIRQ(BLEL_IRQn);
        return;
    }
    uint8_t next = active_ack ^ 1u;
    uint8_t len = gw_build_ack(&gateway, APP_GW_ID, ack_buffers[next]);
    active_ack = next;
    rearm_requested = 0;
    callback_seen = 0;
    armed_at = now;
    GPIOA_SetBits(GPIO_Pin_6);
    uint8_t result = RF_Rx(ack_buffers[next], len, 0xFF, 0xFF);
    GPIOA_ResetBits(GPIO_Pin_6);
    if (result == 0) {
        listening = 1;
        if (idle_at != 0) {
            uint32_t latency = gateway_now_us() - idle_at;
            if (latency > gateway.rearm_lat_max_us) gateway.rearm_lat_max_us = latency;
            idle_at = 0;
        }
    } else rearm_requested = 1;
    PFIC_EnableIRQ(BLEL_IRQn);
}

GW_RAM void gateway_radio_refresh_ack(void)
{
    rearm_requested = 1;
}

GW_RAM void gateway_radio_poll(uint32_t now)
{
    /* 停止射频后才运行Flash中的温漂校准；恢复后继续RAM任务泵。 */
    if ((uint32_t)(now - last_calibration) >= 120000000u) {
        RF_Shut();
#if GW_BOARD_PA
        gateway_pa_off();
#endif
        listening = 0;
        idle_at = 0;
        app_stack_measure();
        BLE_RegInit();
#if GW_BOARD_PA
        /* 寄存器重置后重新登记前端控制，避免校准后功放不再随收发翻转。 */
        gateway_pa_init();
#endif
        last_calibration = gateway_now_us();
        rearm_requested = 1;
    }
    uint32_t last_activity = callback_seen ? callback_at : armed_at;
    if (listening && (uint32_t)(now - last_activity) >= 1000000u) rearm_requested = 1;
    if (listening && !rearm_requested) return;
    if ((uint32_t)(now - last_retry) < 625u) return;
    last_retry = now;
    /* 容量不足时保持不监听，节点得不到硬件ACK，从自身RAM缓存重传。 */
    rearm(now);
}
