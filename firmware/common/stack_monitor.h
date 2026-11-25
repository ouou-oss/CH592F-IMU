#ifndef APP_STACK_MONITOR_H
#define APP_STACK_MONITOR_H
#include <stddef.h>
#include <stdint.h>

#define APP_STACK_PATTERN UINT32_C(0xa5a5a5a5)
#define APP_STACK_GUARD_BYTES 16u
typedef struct {
    uint16_t reserved_bytes;
    uint16_t free_min_bytes;
    uint8_t guard_fault;
} app_stack_metrics_t;

/* 调试器可直接读取；不复用已有协议字段。 */
extern volatile app_stack_metrics_t app_stack;
size_t app_stack_unused(const uint32_t *bottom, size_t words);
/* Flash 函数：只能在射频停止的窗口调用。 */
void app_stack_measure(void);
#endif
