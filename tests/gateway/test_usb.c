#include "usb_cdc.h"
#include "usb_control.h"
#include "proto.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

volatile test_usb_registers_t test_usb_registers;
void USB_IRQHandler(void);
static unsigned transfer_clears;

void test_usb_clear_interrupts(uint8_t mask)
{
    if (mask & RB_UIF_BUS_RST) {
        R8_USB_INT_ST = MASK_UIS_TOKEN;
        R8_USB_INT_FG &= (uint8_t)~RB_UIF_BUS_RST;
    }
    if ((mask & RB_UIF_TRANSFER) && (R8_USB_INT_FG & RB_UIF_TRANSFER)) {
        ++transfer_clears;
        if ((R8_USB_INT_ST & MASK_UIS_TOKEN) != MASK_UIS_TOKEN) {
            /* 第一次 W1C 只释放旧 IN/OUT，SETUP 仍待处理。 */
            R8_USB_INT_ST = (R8_USB_INT_ST & RB_UIS_SETUP_ACT) | MASK_UIS_TOKEN;
        } else {
            R8_USB_INT_ST &= (uint8_t)~RB_UIS_SETUP_ACT;
        }
        if (!(R8_USB_INT_ST & RB_UIS_SETUP_ACT))
            R8_USB_INT_FG &= (uint8_t)~RB_UIF_TRANSFER;
    }
    R8_USB_INT_FG &= (uint8_t)~(mask & (uint8_t)~RB_UIF_TRANSFER);
}

static void transfer(uint8_t status, uint8_t length)
{
    R8_USB_INT_FG = RB_UIF_TRANSFER;
    R8_USB_INT_ST = status;
    R8_USB_RX_LEN = length;
    USB_IRQHandler();
}

static void setup(uint8_t prior_token, uint8_t direction, uint8_t request,
                  uint16_t value, uint16_t index, uint16_t length)
{
    cdc_ep0[0] = direction;
    cdc_ep0[1] = request;
    proto_put_u16(cdc_ep0 + 2, value);
    proto_put_u16(cdc_ep0 + 4, index);
    proto_put_u16(cdc_ep0 + 6, length);
    R8_USB_INT_FG = RB_UIF_TRANSFER;
    R8_USB_INT_ST = RB_UIS_SETUP_ACT | prior_token;
    /* SETUP_ACT 已保证收到完整 8B，硬件保留前次 RX_LEN。 */
    USB_IRQHandler();
}

static void reset(void)
{
    memset((void *)&test_usb_registers, 0, sizeof(test_usb_registers));
    transfer_clears = 0;
    R8_USB_INT_FG = RB_UIF_BUS_RST;
    USB_IRQHandler();
}

static void test_setup_with_stale_lengths(void)
{
    const uint8_t lengths[] = {0, 7, 64};
    for (size_t i = 0; i < sizeof(lengths); ++i) {
        reset();
        R8_USB_RX_LEN = lengths[i];
        setup(MASK_UIS_TOKEN, 0x80, USB_GET_DESCRIPTOR, 0x0100, 0, 18);
        assert(R8_UEP0_T_LEN == 18);
        assert(cdc_ep0[0] == 18 && cdc_ep0[1] == 1 && cdc_ep0[7] == 64);
        assert(proto_get_u16(cdc_ep0 + 8) == 0x1A86);
        assert(proto_get_u16(cdc_ep0 + 10) == 0x8040);
        assert((R8_UEP0_CTRL & MASK_UEP_T_RES) == UEP_T_RES_ACK);
        assert(R8_UEP0_CTRL & RB_UEP_T_TOG);
        assert(!(R8_USB_INT_FG & RB_UIF_TRANSFER));
        assert(transfer_clears == 1);
    }
}

static void test_pending_transfer_and_setup(void)
{
    const uint8_t pending[] = {UIS_TOKEN_IN | 1u, UIS_TOKEN_OUT | 1u};
    for (size_t i = 0; i < sizeof(pending); ++i) {
        reset();
        cdc_tx_busy = 1;
        cdc_ep1[0] = 0xA5;
        R8_USB_RX_LEN = 1;
        setup(pending[i] | RB_UIS_TOG_OK, 0x80, USB_GET_DESCRIPTOR, 0x0100, 0, 18);
        assert(R8_UEP0_T_LEN == 18);
        if (i == 0) assert(cdc_tx_busy == 0);
        else assert(cdc_out_len == 1 && cdc_ep1[0] == 0xA5);
        assert(transfer_clears == 2);
        assert(!(R8_USB_INT_FG & RB_UIF_TRANSFER));
    }

    /* 旧 EP0 IN 仍会填下一数据块，不能覆盖同时到达的新 SETUP。 */
    reset();
    setup(MASK_UIS_TOKEN, 0x80, USB_GET_DESCRIPTOR, 0x0200, 0, 67);
    assert(R8_UEP0_T_LEN == 64);
    transfer_clears = 0;
    setup(UIS_TOKEN_IN, 0x80, USB_GET_DESCRIPTOR, 0x0100, 0, 18);
    assert(R8_UEP0_T_LEN == 18 && cdc_ep0[0] == 18 && cdc_ep0[1] == 1);
    assert(transfer_clears == 2);
    assert(!(R8_USB_INT_FG & RB_UIF_TRANSFER));
}

static void test_enumeration(void)
{
    reset();
    setup(UIS_TOKEN_OUT, 0x80, USB_GET_DESCRIPTOR, 0x0100, 0, 8);
    assert(R8_UEP0_T_LEN == 8 && cdc_ep0[7] == 64);
    transfer(UIS_TOKEN_IN, 0);
    transfer(UIS_TOKEN_OUT | RB_UIS_TOG_OK, 0);

    setup(UIS_TOKEN_OUT, 0, USB_SET_ADDRESS, 7, 0, 0);
    assert(R8_USB_DEV_AD == 0 && R8_UEP0_T_LEN == 0);
    transfer(UIS_TOKEN_IN, 0);
    assert(R8_USB_DEV_AD == 7);

    uint8_t config[67];
    setup(UIS_TOKEN_IN, 0x80, USB_GET_DESCRIPTOR, 0x0200, 0, 255);
    assert(R8_UEP0_T_LEN == 64 && proto_get_u16(cdc_ep0 + 2) == sizeof(config));
    memcpy(config, cdc_ep0, 64);
    transfer(UIS_TOKEN_IN, 0);
    assert(R8_UEP0_T_LEN == 3 && !(R8_UEP0_CTRL & RB_UEP_T_TOG));
    memcpy(config + 64, cdc_ep0, 3);
    unsigned interfaces = 0, endpoints = 0;
    for (size_t at = 0; at < sizeof(config); at += config[at]) {
        assert(config[at] >= 2 && at + config[at] <= sizeof(config));
        interfaces += config[at + 1] == 4;
        endpoints += config[at + 1] == 5;
    }
    assert(interfaces == 2 && endpoints == 3);
    transfer(UIS_TOKEN_IN, 0);
    transfer(UIS_TOKEN_OUT | RB_UIS_TOG_OK, 0);

    setup(UIS_TOKEN_OUT, 0x80, USB_GET_DESCRIPTOR, 0x0309, 0x0409, 255);
    assert((R8_UEP0_CTRL & MASK_UEP_T_RES) == UEP_T_RES_STALL);
    setup(UIS_TOKEN_IN, 0x80, USB_GET_DESCRIPTOR, 0x0302, 0x0409, 255);
    assert(R8_UEP0_T_LEN == 30 && cdc_ep0[0] == 30 && cdc_ep0[1] == 3);
    assert((R8_UEP0_CTRL & MASK_UEP_T_RES) == UEP_T_RES_ACK);
    transfer(UIS_TOKEN_IN, 0);
    transfer(UIS_TOKEN_OUT | RB_UIS_TOG_OK, 0);

    setup(UIS_TOKEN_OUT, 0, USB_SET_CONFIGURATION, 1, 0, 0);
    assert(cdc_configured == 1 && !cdc_ready());
    transfer(UIS_TOKEN_IN, 0);
}

static void test_cdc_requests_and_bulk(void)
{
    const uint8_t coding[] = {0, 0xC2, 1, 0, 0, 0, 8};
    setup(UIS_TOKEN_IN, 0x21, 0x20, 0, 0, sizeof(coding));
    memcpy(cdc_ep0, coding, sizeof(coding));
    transfer(UIS_TOKEN_OUT | RB_UIS_TOG_OK, sizeof(coding));
    assert(R8_UEP0_T_LEN == 0);
    transfer(UIS_TOKEN_IN, 0);
    setup(UIS_TOKEN_IN, 0xA1, 0x21, 0, 0, sizeof(coding));
    assert(R8_UEP0_T_LEN == sizeof(coding));
    assert(memcmp(cdc_ep0, coding, sizeof(coding)) == 0);
    transfer(UIS_TOKEN_IN, 0);
    transfer(UIS_TOKEN_OUT | RB_UIS_TOG_OK, 0);

    setup(UIS_TOKEN_OUT, 0x21, 0x22, 1, 0, 0);
    transfer(UIS_TOKEN_IN, 0);
    assert(cdc_ready());
    const uint8_t bytes[] = {0xA5, 0x5A, 0x81, 0};
    assert(cdc_write(bytes, sizeof(bytes)));
    assert(!cdc_tx_idle() && memcmp(cdc_ep1 + 64, bytes, sizeof(bytes)) == 0);
    transfer(UIS_TOKEN_IN | 1u, 0);
    assert(cdc_tx_idle());
    memcpy(cdc_ep1, bytes, sizeof(bytes));
    transfer(UIS_TOKEN_OUT | 1u, sizeof(bytes));
    assert(cdc_out_len == 0); /* toggle 不正确时不消费 OUT。 */
    transfer(UIS_TOKEN_OUT | 1u | RB_UIS_TOG_OK, sizeof(bytes));
    assert((R8_UEP1_CTRL & MASK_UEP_R_RES) == UEP_R_RES_NAK);
    uint8_t byte;
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        assert(cdc_read_byte(&byte) && byte == bytes[i]);
    }
    assert(!cdc_read_byte(&byte));
    assert((R8_UEP1_CTRL & MASK_UEP_R_RES) == UEP_R_RES_ACK);

    uint32_t generation = cdc_generation();
    reset();
    assert(!cdc_ready() && cdc_generation() == generation + 1);
}

static void test_endpoint_recovery_and_serial(void)
{
    reset();
    setup(MASK_UIS_TOKEN, 0x80, USB_GET_DESCRIPTOR, 0x0303, 0x0409, 64);
    const uint8_t serial[] = {18, 3, 'B', 0, 'M', 0, 'I', 0, 'G', 0, 'W', 0, '2', 0, '5', 0, '4', 0};
    assert(R8_UEP0_T_LEN == sizeof(serial) && memcmp(cdc_ep0, serial, sizeof(serial)) == 0);

    cdc_configured = cdc_dtr = 1;
    const uint8_t tx[] = {0xA5, 0x5A};
    assert(cdc_write(tx, sizeof(tx)) && !cdc_tx_idle());
    setup(MASK_UIS_TOKEN, 2, USB_CLEAR_FEATURE, 0, 0x81, 0);
    assert(cdc_tx_idle() && (R8_UEP1_CTRL & (MASK_UEP_T_RES | RB_UEP_T_TOG)) == UEP_T_RES_NAK);
    assert(cdc_write(tx, sizeof(tx)));

    memset(cdc_ep1, 0x42, 64);
    transfer(UIS_TOKEN_OUT | 1u | RB_UIS_TOG_OK, 65);
    assert(cdc_out_len == 64);
    uint8_t byte;
    for (unsigned i = 0; i < 64; ++i) assert(cdc_read_byte(&byte) && byte == 0x42);
    assert(!cdc_read_byte(&byte) && memcmp(cdc_ep1 + 64, tx, sizeof(tx)) == 0);
    transfer(UIS_TOKEN_OUT | 1u | RB_UIS_TOG_OK, 64);
    assert(cdc_read_byte(&byte) && cdc_out_pos == 1);
    setup(MASK_UIS_TOKEN, 2, USB_CLEAR_FEATURE, 0, 0x01, 0);
    assert(cdc_out_len == 0 && cdc_out_pos == 0 && !cdc_read_byte(&byte));
    assert((R8_UEP1_CTRL & (MASK_UEP_R_RES | RB_UEP_R_TOG)) == UEP_R_RES_ACK);
}

int main(void)
{
    test_setup_with_stale_lengths();
    test_pending_transfer_and_setup();
    test_enumeration();
    test_cdc_requests_and_bulk();
    test_endpoint_recovery_and_serial();
    puts("gateway USB ISR/enumeration/CDC tests passed");
    return 0;
}
