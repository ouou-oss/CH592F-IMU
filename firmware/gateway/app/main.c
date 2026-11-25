#include "gateway_app.h"
#include "usb_cdc.h"
#include "stack_monitor.h"

__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];
gw_core_t gateway;

#if APP_LOG_ENABLE
#define LOG_STAGE(text) UART1_SendString((uint8_t *)(text "\r\n"), sizeof(text "\r\n") - 1u)
static void log_startup(void)
{
    static const uint8_t prefix[] =
#if GW_BOARD_PA
        "CH592F BMI323 gateway-pa v1.5 gw=";
#else
        "CH592F BMI323 gateway v1.5 gw=";
#endif
    uint8_t id[] = {'0' + APP_GW_ID / 100,
                    '0' + APP_GW_ID / 10 % 10,
                    '0' + APP_GW_ID % 10, '\r', '\n'};
    /* 在RF启动前直接发UART，避免为启动日志引入stdio的RAM状态。 */
    UART1_SendString((uint8_t *)prefix, sizeof(prefix) - 1);
    UART1_SendString(id, sizeof(id));
}
#else
#define LOG_STAGE(text) ((void)0)
#endif

GW_RAM uint32_t gateway_now_us(void)
{
    /* 稳定读取64位SysTick，分解 /60 避免调用Flash中的64位除法辅助函数。
     * 2^32 = 60 * 71582788 + 16；返回值自然按u32微秒回绕。 */
    volatile uint32_t *counter = (volatile uint32_t *)&SysTick->CNT;
    uint32_t high, low;
    do {
        high = counter[1];
        low = counter[0];
    } while (high != counter[1]);
    return gw_ticks_to_us(high, low);
}

static GW_RAM void run(void)
{
    for (;;) {
        /* 库的AUTO处理器由TMOS驱动，主循环及射频闭包必须全部在RAM。 */
        TMOS_SystemProcess();
        uint32_t now = gateway_now_us();
        gateway_radio_poll(now);
        gateway_host_poll(now);
        WWDG_SetCounter(0);
    }
}

int main(void)
{
#if DCDC_ENABLE
    PWR_DCDCCfg(ENABLE);
#endif
    SetSysClock(CLK_SOURCE_PLL_60MHz);
#if GW_BOARD_PA
    /* 先拉低再配输出：PA5/PA4 是 AT2401C 的 TX_EN/RX_EN，浮空电平可能误开前端。
     * PA6 保留为 RF_Rx 测点，PA5 不再用作 RF 回调测点。 */
    GPIOA_ResetBits(GPIO_Pin_5 | GPIO_Pin_4 | GPIO_Pin_6);
    GPIOA_ModeCfg(GPIO_Pin_5 | GPIO_Pin_4 | GPIO_Pin_6, GPIO_ModeOut_PP_5mA);
#else
    GPIOA_ResetBits(GPIO_Pin_5 | GPIO_Pin_6);
    GPIOA_ModeCfg(GPIO_Pin_5 | GPIO_Pin_6, GPIO_ModeOut_PP_5mA);
#endif
    GPIOA_SetBits(GPIO_Pin_9);
    GPIOA_ModeCfg(GPIO_Pin_9, GPIO_ModeOut_PP_5mA);
    UART1_DefInit();
#if APP_LOG_ENABLE
    log_startup();
#endif
    gw_core_init(&gateway);
    LOG_STAGE("BLE init");
    CH59x_BLEInit();
    LOG_STAGE("HAL init (32K)");
    HAL_Init();
    WWDG_SetCounter(0);
    WWDG_ResetCfg(ENABLE);
    LOG_STAGE("CDC init");
    cdc_init();
    LOG_STAGE("CDC ready");
    gateway_host_init();
    app_stack_measure();
#if GW_BOARD_PA
    LOG_STAGE("RF init (PA)");
#else
    LOG_STAGE("RF init");
#endif
    gateway_radio_init();
    run();
}
