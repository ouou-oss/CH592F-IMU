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
typedef struct {
    unsigned LLEMode, Channel, Frequency, accessAddress, CRCInit, RxMaxlen, TxMaxlen;
    void (*rfStatusCB)(uint8_t, uint8_t, uint8_t *);
} rfConfig_t;
static uint32_t now;
static uint8_t rx_error;
static unsigned measurements;
gw_core_t gateway;
uint32_t gateway_now_us(void) { return now; }
static void PFIC_DisableIRQ(int irq) { (void)irq; }
static void PFIC_EnableIRQ(int irq) { (void)irq; }
static void GPIOA_SetBits(unsigned pins) { (void)pins; }
static void GPIOA_ResetBits(unsigned pins) { (void)pins; }
static void RF_Shut(void) {}
static uint8_t RF_RoleInit(void) { return 0; }
static uint8_t RF_Config(rfConfig_t *cfg) { (void)cfg; return 0; }
static uint8_t RF_Rx(uint8_t *p, uint8_t len, uint8_t tx, uint8_t rx)
{ (void)p; (void)len; (void)tx; (void)rx; return rx_error; }
static void BLE_RegInit(void) {}
static void __nop(void) {}
void app_stack_measure(void) { ++measurements; }
#include "../../firmware/gateway/app/radio.c"

int main(void)
{
    now = 1000; gateway_radio_init();
    now = 2000; rf_callback(RX_MODE_TX_FINISH, 0, NULL);
    gateway.head = 7;
    now = 3000; gateway_radio_poll(now);
    assert(!listening && idle_at == 0);
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
