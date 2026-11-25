/*
 * USB CDC 控制协议依据 WCH CH592 EVT USB/Device/COM 示例移植。
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 */
#include "usb_control.h"
#include "gateway_app.h"
#include "proto.h"

/* 监听 RF 时也会枚举/控制传输；描述符必须放 RAM，不能用只读 Flash 表。 */
static uint8_t device_desc[] = {
    18,1,0x10,1,2,0,0,64,0x86,0x1A,0x40,0x80,2,1,1,2,3,1
};
static uint8_t config_desc[] = {
    9,2,67,0,2,1,0,0x80,50,
    9,4,0,0,1,2,2,1,0,
    5,0x24,0,0x10,1, 5,0x24,1,0,1,
    4,0x24,2,2, 5,0x24,6,0,1,
    7,5,0x82,3,8,0,16,
    9,4,1,0,2,0x0A,0,0,0,
    7,5,0x01,2,64,0,0, 7,5,0x81,2,64,0,0
};
static uint8_t lang_desc[] = {4,3,9,4};
static uint8_t vendor_desc[] = {8,3,'W',0,'C',0,'H',0};
static uint8_t product_desc[] = {
    30,3,'B',0,'M',0,'I',0,'3',0,'2',0,'3',0,' ',0,'G',0,'a',0,'t',0,'e',0,'w',0,'a',0,'y',0
};
static uint8_t serial_desc[] = {
    18,3,'B',0,'M',0,'I',0,'G',0,'W',0,
    '0' + APP_GW_ID / 100, 0, '0' + APP_GW_ID / 10 % 10, 0, '0' + APP_GW_ID % 10, 0
};
static uint8_t line_coding[7] = {0,0xC2,1,0,0,0,8};
static uint8_t response[2];
static const uint8_t *control_ptr;
static uint16_t control_remaining;
static uint8_t control_request;
static uint8_t new_address;
static uint8_t address_pending;
static uint8_t expect_line_coding;

static GW_RAM void send_next(void)
{
    uint8_t len = control_remaining > 64u ? 64u : (uint8_t)control_remaining;
    gw_copy(cdc_ep0, control_ptr, len);
    control_ptr += len;
    control_remaining = (uint16_t)(control_remaining - len);
    R8_UEP0_T_LEN = len;
    R8_UEP0_CTRL = (R8_UEP0_CTRL & ~(MASK_UEP_T_RES | MASK_UEP_R_RES)) |
                    UEP_T_RES_ACK | UEP_R_RES_ACK;
}

static GW_RAM void reply(const uint8_t *bytes, uint16_t len, uint16_t requested)
{
    control_ptr = bytes;
    control_remaining = len < requested ? len : requested;
    send_next();
}

static GW_RAM void status_in(void)
{
    control_remaining = 0;
    R8_UEP0_T_LEN = 0;
    R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_NAK | UEP_T_RES_ACK;
}

static GW_RAM void stall(void)
{
    R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;
}

static GW_RAM bool descriptor(uint16_t value, uint16_t requested)
{
    uint8_t type = (uint8_t)(value >> 8);
    uint8_t index = (uint8_t)value;
    if (type == 1u && index == 0) reply(device_desc, sizeof(device_desc), requested);
    else if (type == 2u && index == 0) reply(config_desc, sizeof(config_desc), requested);
    else if (type == 3u && index == 0) reply(lang_desc, sizeof(lang_desc), requested);
    else if (type == 3u && index == 1) reply(vendor_desc, sizeof(vendor_desc), requested);
    else if (type == 3u && index == 2) reply(product_desc, sizeof(product_desc), requested);
    else if (type == 3u && index == 3) reply(serial_desc, sizeof(serial_desc), requested);
    else return false;
    return true;
}

static GW_RAM bool standard_request(uint8_t direction, uint16_t value,
                                    uint16_t index, uint16_t requested)
{
    if (control_request == USB_GET_DESCRIPTOR && direction == 0x80u)
        return descriptor(value, requested);
    if (control_request == USB_SET_ADDRESS && direction == 0 && value <= 127u && requested == 0) {
        new_address = (uint8_t)value;
        address_pending = 1;
        status_in();
        return true;
    }
    if (control_request == USB_SET_CONFIGURATION && direction == 0 && value <= 1u && requested == 0) {
        cdc_configured = (uint8_t)value;
        cdc_dtr = 0;
        cdc_tx_busy = 0;
        ++cdc_link_generation;
        status_in();
        return true;
    }
    if (control_request == USB_GET_CONFIGURATION && direction == 0x80u) {
        response[0] = cdc_configured;
        reply(response, 1, requested);
        return true;
    }
    if (control_request == USB_GET_STATUS && (direction & 0x80u)) {
        response[0] = 0;
        response[1] = 0;
        reply(response, 2, requested);
        return true;
    }
    if (control_request == USB_GET_INTERFACE && direction == 0x81u && index <= 1u) {
        response[0] = 0;
        reply(response, 1, requested);
        return true;
    }
    if (control_request == USB_SET_INTERFACE && direction == 1u && index <= 1u && value == 0 && requested == 0) {
        status_in();
        return true;
    }
    if (control_request == USB_CLEAR_FEATURE && direction == 2u && value == 0 && requested == 0) {
        if (index == 0x81u) {
            cdc_tx_busy = 0;
            R8_UEP1_CTRL = (R8_UEP1_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_NAK;
        } else if (index == 1u) {
            cdc_out_len = cdc_out_pos = 0;
            R8_UEP1_CTRL = (R8_UEP1_CTRL & ~(RB_UEP_R_TOG | MASK_UEP_R_RES)) | UEP_R_RES_ACK;
        }
        else if (index == 0x82u) R8_UEP2_CTRL = (R8_UEP2_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_NAK;
        else return false;
        status_in();
        return true;
    }
    return false;
}

static GW_RAM bool class_request(uint8_t direction, uint16_t value,
                                 uint16_t index, uint16_t requested)
{
    if (index != 0) return false;
    if (control_request == 0x21u && direction == 0xA1u) {
        reply(line_coding, sizeof(line_coding), requested);
        return true;
    }
    if (control_request == 0x20u && direction == 0x21u && requested == 7u) {
        expect_line_coding = 1;
        R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;
        return true;
    }
    if (control_request == 0x22u && direction == 0x21u && requested == 0) {
        uint8_t dtr = (uint8_t)(value & 1u);
        if (cdc_dtr != dtr) ++cdc_link_generation;
        cdc_dtr = dtr;
        status_in();
        return true;
    }
    if (control_request == 0x23u && direction == 0x21u && requested == 0) {
        status_in();
        return true;
    }
    return false;
}

GW_RAM void cdc_control_setup(const uint8_t *request)
{
    control_remaining = 0;
    expect_line_coding = 0;
    address_pending = 0;
    /* SETUP_ACT 保证请求完整；SETUP 不更新 RX_LEN，该寄存器仍是前次 OUT 长度。 */
    uint8_t direction = request[0];
    control_request = request[1];
    uint16_t value = proto_get_u16(request + 2);
    uint16_t index = proto_get_u16(request + 4);
    uint16_t requested = proto_get_u16(request + 6);
    R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;
    bool ok = false;
    if ((direction & USB_REQ_TYP_MASK) == USB_REQ_TYP_STANDARD)
        ok = standard_request(direction, value, index, requested);
    if ((direction & USB_REQ_TYP_MASK) == USB_REQ_TYP_CLASS)
        ok = class_request(direction, value, index, requested);
    if (!ok) stall();
}

GW_RAM void cdc_control_in(void)
{
    if (address_pending) {
        R8_USB_DEV_AD = new_address;
        address_pending = 0;
    }
    if (control_remaining != 0) {
        R8_UEP0_CTRL ^= RB_UEP_T_TOG;
        send_next();
    } else {
        R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;
    }
}

GW_RAM void cdc_control_out(void)
{
    if (expect_line_coding) {
        if (R8_USB_RX_LEN != 7u) {
            stall();
            return;
        }
        gw_copy(line_coding, cdc_ep0, 7);
        expect_line_coding = 0;
        status_in();
    } else {
        R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;
    }
}

GW_RAM void cdc_control_reset(void)
{
    cdc_configured = 0;
    cdc_dtr = 0;
    cdc_tx_busy = 0;
    cdc_out_len = 0;
    cdc_out_pos = 0;
    cdc_suspended = 0;
    ++cdc_link_generation;
    control_remaining = 0;
    expect_line_coding = 0;
    address_pending = 0;
    R8_USB_DEV_AD = 0;
    R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
    R8_UEP1_CTRL = RB_UEP_AUTO_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;
    R8_UEP2_CTRL = RB_UEP_AUTO_TOG | UEP_T_RES_NAK;
}
