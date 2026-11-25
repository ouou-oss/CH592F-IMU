#include "gateway_core.h"
#include <assert.h>
#include <stdio.h>

/* 运行真实重挂逻辑；仅替换芯片 API，不依赖射频硬件。 */
#define GATEWAY_APP_H
#define BLEL_IRQn 21
#define APP_GW_ID 1
#define APP_RF_FREQUENCY 2450000
#define APP_RF_ACCESS 0x71764129
#define APP_RF_CRC 0x123456
#define LLE_MODE_AUTO 1
#define LLE_MODE_PHY_1M 2
#define LLE_MODE_EX_CHANNEL 4
#define GPIO_Pin_5 32
#define GPIO_Pin_6 64
#define RX_MODE_RX_DATA 1
#define RX_MODE_TX_FINISH 2
#define RX_MODE_TX_FAIL 3
#if GW_BOARD_PA
/* 与 BOARD=pa 构建一致，同时验证 rssi 补偿算法。 */
#define GW_RSSI_OFFSET_DB (-13)
#endif
typedef struct {
    unsigned LLEMode, Channel, Frequency, accessAddress, CRCInit, RxMaxlen, TxMaxlen;
    void (*rfStatusCB)(uint8_t, uint8_t, uint8_t *);
} rfConfig_t;
static uint32_t now;
static uint8_t rx_error;
static unsigned measurements;
static unsigned gpio_set_bits;
#if GW_BOARD_PA
static unsigned pa_init_calls;
static unsigned pa_off_calls;
#endif
static uint8_t radio_buf[2 + 246];
gw_core_t gateway;
uint32_t gateway_now_us(void) { return now; }
static void PFIC_DisableIRQ(int irq) { (void)irq; }
static void PFIC_EnableIRQ(int irq) { (void)irq; }
static void GPIOA_SetBits(unsigned pins) { gpio_set_bits |= pins; }
static void GPIOA_ResetBits(unsigned pins) { (void)pins; }
#if GW_BOARD_PA
void gateway_pa_init(void) { ++pa_init_calls; }
void gateway_pa_off(void) { ++pa_off_calls; }
#endif
static void RF_Shut(void) {}
static uint8_t RF_RoleInit(void) { return 0; }
static uint8_t RF_Config(rfConfig_t *cfg) { (void)cfg; return 0; }
static uint8_t RF_Rx(uint8_t *p, uint8_t len, uint8_t tx, uint8_t rx)
{ (void)p; (void)len; (void)tx; (void)rx; return rx_error; }
static void BLE_RegInit(void) {}
static void __nop(void) {}
void app_stack_measure(void) { ++measurements; }
#include "../../firmware/gateway/app/radio.c"

static void make_data(void)
{
    for (unsigned i = 0; i < sizeof(radio_buf) - 2u; ++i) radio_buf[2 + i] = 0;
    radio_buf[0] = 0xD8;            /* -40 dBm */
    radio_buf[1] = 246;             /* 36 B 头 + 35 点 x 6 B */
    radio_buf[2] = 0x11;            /* version 1, DATA */
    radio_buf[3] = 1;               /* node_id */
    radio_buf[5] = 0xBD;            /* cfg 字节，格式无关 */
    radio_buf[6] = 1;               /* boot_id */
    radio_buf[2 + 18] = 35;         /* sample_count */
    radio_buf[2 + 20] = 1;          /* attempt */
}

int main(void)
{
    now = 1000; gateway_radio_init();
#if GW_BOARD_PA
    /* PA 注册在角色初始化之后完成一次。 */
    assert(pa_init_calls == 1);
#endif

    /* DATA 入队按构建常量扣掉 LNA 增益；PA 板回调不驱动 TX_EN 测点。 */
    make_data();
    rf_callback(RX_MODE_RX_DATA, 0, radio_buf);
    assert(gateway.rx_ok == 1);
#if GW_BOARD_PA
    assert(gateway.records[0].rssi == (int8_t)(-40 + GW_RSSI_OFFSET_DB));
    assert((gpio_set_bits & GPIO_Pin_5) == 0); /* PA5 已让给 TX_EN */
#else
    assert(gateway.records[0].rssi == -40);
    assert((gpio_set_bits & GPIO_Pin_5) != 0);
#endif

    now = 2000; rf_callback(RX_MODE_TX_FINISH, 0, NULL);
    gateway.head = 7;
    now = 3000; gateway_radio_poll(now);
    assert(!listening && idle_at == 0);
#if GW_BOARD_PA
    /* 主动背压期间不监听，前端必须关掉。 */
    assert(pa_off_calls == 1);
#endif
    gateway.tail = 1;
    now = 10000000; gateway_radio_poll(now);
    assert(listening && gateway.rearm_lat_max_us == 0);
    now += 1000; rf_callback(RX_MODE_TX_FINISH, 0, NULL);
    now += 625; rx_error = 1; gateway_radio_poll(now);
    now += 625; rx_error = 0; gateway_radio_poll(now);
    assert(gateway.rearm_lat_max_us == 1250); /* 真实失败重试仍计入。 */
    now += 1000; rf_callback(RX_MODE_TX_FINISH, 0, NULL);
    now = 120001000; gateway_radio_poll(now);
    assert(measurements == 1 && idle_at == 0);
    assert(gateway.rearm_lat_max_us == 1250); /* 主动校准不污染最大值。 */
#if GW_BOARD_PA
    /* 校准窗口先关前端，BLE_RegInit 后重新登记 PA 控制。 */
    assert(pa_off_calls == 2 && pa_init_calls == 2);
#endif
    now = UINT32_MAX - 100; rf_callback(RX_MODE_TX_FINISH, 0, NULL);
    last_calibration = now; last_retry = now;
    now = 1000; gateway_radio_poll(now);
    assert(gateway.rearm_lat_max_us == 1250 && listening);
    /* 下行命令刷新 ACK 应触发重挂。 */
    gateway_radio_refresh_ack();
    assert(rearm_requested == 1);
    now += 625; gateway_radio_poll(now);
    assert(rearm_requested == 0 && listening);
    puts("gateway radio backpressure/rearm tests passed");
    return 0;
}
