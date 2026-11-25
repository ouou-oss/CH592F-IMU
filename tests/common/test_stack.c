#include "stack_monitor.h"
#include <assert.h>
#include <stdio.h>

/* 宿主只测试扫描算法，链接符号不代表宿主进程的活动栈。 */
uint32_t _susrstack[1], _eusrstack[1];
int main(void)
{
    uint32_t area[512];
    for (size_t i = 0; i < 512; ++i) area[i] = APP_STACK_PATTERN;
    assert(app_stack_unused(area, 512) == 2048);
    area[400] = 0;
    assert(app_stack_unused(area, 512) == 1600);
    area[4] = 0;
    assert(app_stack_unused(area, 512) == APP_STACK_GUARD_BYTES);
    area[3] = 0;
    assert(app_stack_unused(area, 512) < APP_STACK_GUARD_BYTES);
    area[0] = 0;
    assert(app_stack_unused(area, 512) == 0);
    assert(app_stack_unused(area, 0) == 0);
    puts("stack watermark/guard scan tests passed");
    return 0;
}
