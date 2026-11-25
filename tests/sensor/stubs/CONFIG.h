#ifndef SENSOR_TEST_CONFIG_H
#define SENSOR_TEST_CONFIG_H
#include <stdint.h>
#define __HIGH_CODE
#define __nop() ((void)0)
#define APP_LOG_ENABLE 0
#define APP_RF_FREQUENCY 2450000u
#define APP_RF_ACCESS 0x71764129u
#define APP_RF_CRC 0x123456u
#define GPIO_Pin_4 16u
#define GPIO_Pin_5 32u
#define GPIO_Pin_6 64u
#define LLE_MODE_AUTO 1
#define LLE_MODE_PHY_1M 0
#define LLE_MODE_EX_CHANNEL 64
#define TX_MODE_TX_FINISH 1
#define TX_MODE_TX_FAIL 17
#define TX_MODE_RX_DATA 2
#define TX_MODE_RX_TIMEOUT 18
#define LL_TX_PWR_0_DBM 0x15
#define RB_LSR_DATA_RDY 1
#define SUCCESS 0
typedef struct {
    uint8_t LLEMode, RxMaxlen, TxMaxlen;
    uint32_t Frequency, accessAddress, CRCInit;
    void (*rfStatusCB)(uint8_t, uint8_t, uint8_t *);
} rfConfig_t;
uint8_t RF_Config(rfConfig_t *config);
uint8_t RF_Tx(uint8_t *data, uint8_t length, uint8_t tx_type, uint8_t rx_type);
void RF_Shut(void);
uint8_t LL_SetTxPowerLevel(uint8_t power);
extern uint32_t test_gpio_out;
#define GPIOA_SetBits(pins) (test_gpio_out |= (pins))
#define GPIOA_ResetBits(pins) (test_gpio_out &= ~(pins))
#define GPIOA_InverseBits(pins) (test_gpio_out ^= (pins))
uint32_t EEPROM_READ(uint32_t address, void *data, uint32_t length);
uint32_t EEPROM_WRITE(uint32_t address, void *data, uint32_t length);
uint32_t EEPROM_ERASE(uint32_t address, uint32_t length);
uint8_t UART1_GetLinSTA(void);
uint8_t UART1_RecvByte(void);
void SYS_ResetExecute(void);
uint32_t GPIOA_ReadPortPin(uint32_t pins);
uint8_t tmos_set_event(uint8_t task, uint16_t events);
uint8_t tmos_start_task(uint8_t task, uint16_t events, uint32_t delay);
uint8_t tmos_stop_task(uint8_t task, uint16_t events);
#endif
