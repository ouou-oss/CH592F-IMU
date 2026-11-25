#include "stack_monitor.h"

extern uint32_t _susrstack[], _eusrstack[];
volatile app_stack_metrics_t app_stack;

size_t app_stack_unused(const uint32_t *bottom, size_t words)
{
    size_t unused = 0;
    while (unused < words && bottom[unused] == APP_STACK_PATTERN) ++unused;
    return unused * sizeof(*bottom);
}

void app_stack_measure(void)
{
    size_t words = ((uintptr_t)_eusrstack - (uintptr_t)_susrstack) / sizeof(uint32_t);
    uint16_t unused = (uint16_t)app_stack_unused(_susrstack, words);
    if (!app_stack.reserved_bytes || unused < app_stack.free_min_bytes)
        app_stack.free_min_bytes = unused;
    app_stack.reserved_bytes = (uint16_t)(words * sizeof(uint32_t));
    if (unused < APP_STACK_GUARD_BYTES) app_stack.guard_fault = 1;
}
