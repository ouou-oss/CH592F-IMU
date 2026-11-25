#ifndef TEST_USB_REGISTERS_H
#define TEST_USB_REGISTERS_H

/* 复用厂家位定义，仅把实际用到的 MMIO 寄存器替换为宿主内存。 */
#include "CH592SFR.h"
#define __INTERRUPT
#define __HIGH_CODE
#define USB_IRQn 22

typedef struct {
    uint8_t ctrl, dev_ctrl, int_flags, int_enable, int_status, mis_status;
    uint8_t device_address, rx_len, ep4_1_mode, ep2_3_mode;
    uint8_t ep0_ctrl, ep1_ctrl, ep2_ctrl, ep0_len, ep1_len;
    uint16_t ep0_dma, ep1_dma, ep2_dma, pin_analog;
} test_usb_registers_t;

extern volatile test_usb_registers_t test_usb_registers;
void test_usb_clear_interrupts(uint8_t mask);
#define CDC_CLEAR_INTERRUPTS(mask) test_usb_clear_interrupts(mask)

#undef R8_USB_CTRL
#define R8_USB_CTRL test_usb_registers.ctrl
#undef R8_UDEV_CTRL
#define R8_UDEV_CTRL test_usb_registers.dev_ctrl
#undef R8_USB_INT_FG
#define R8_USB_INT_FG test_usb_registers.int_flags
#undef R8_USB_INT_EN
#define R8_USB_INT_EN test_usb_registers.int_enable
#undef R8_USB_INT_ST
#define R8_USB_INT_ST test_usb_registers.int_status
#undef R8_USB_MIS_ST
#define R8_USB_MIS_ST test_usb_registers.mis_status
#undef R8_USB_DEV_AD
#define R8_USB_DEV_AD test_usb_registers.device_address
#undef R8_USB_RX_LEN
#define R8_USB_RX_LEN test_usb_registers.rx_len
#undef R8_UEP4_1_MOD
#define R8_UEP4_1_MOD test_usb_registers.ep4_1_mode
#undef R8_UEP2_3_MOD
#define R8_UEP2_3_MOD test_usb_registers.ep2_3_mode
#undef R8_UEP0_CTRL
#define R8_UEP0_CTRL test_usb_registers.ep0_ctrl
#undef R8_UEP1_CTRL
#define R8_UEP1_CTRL test_usb_registers.ep1_ctrl
#undef R8_UEP2_CTRL
#define R8_UEP2_CTRL test_usb_registers.ep2_ctrl
#undef R8_UEP0_T_LEN
#define R8_UEP0_T_LEN test_usb_registers.ep0_len
#undef R8_UEP1_T_LEN
#define R8_UEP1_T_LEN test_usb_registers.ep1_len
#undef R16_UEP0_DMA
#define R16_UEP0_DMA test_usb_registers.ep0_dma
#undef R16_UEP1_DMA
#define R16_UEP1_DMA test_usb_registers.ep1_dma
#undef R16_UEP2_DMA
#define R16_UEP2_DMA test_usb_registers.ep2_dma
#undef R16_PIN_ANALOG_IE
#define R16_PIN_ANALOG_IE test_usb_registers.pin_analog

#endif
