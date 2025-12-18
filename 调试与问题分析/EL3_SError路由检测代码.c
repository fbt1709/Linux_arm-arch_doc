/*
 * EL3 SError路由检测 - 简单版本
 * 
 * 在EL3异常处理函数中添加这几行代码即可
 */

#include <arch_helpers.h>
#include <common/debug.h>

/* 在serror_aarch64或handle_lower_el_async_ea()函数中添加 */
void check_serror_routing(void)
{
    uint64_t scr_el3 = read_scr_el3();
    
    INFO("[EL3] SError路由检测: SCR_EL3.EA = %d, ", (scr_el3 & SCR_EA_BIT) ? 1 : 0);
    if (scr_el3 & SCR_EA_BIT) {
        INFO("路由到EL3 (FFH)\n");
    } else {
        INFO("路由到低异常等级 (KFH)\n");
    }
    INFO("  SCR_EL3 = 0x%016llx, ESR_EL3 = 0x%016llx\n", scr_el3, read_esr_el3());
}

