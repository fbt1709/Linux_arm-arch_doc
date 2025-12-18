/*
 * 检测SCR_EL3.EA配置的代码
 * 
 * 用于在BL31退出和TFTF执行时检查SCR_EL3.EA的值
 */

#include <arch_helpers.h>
#include <common/debug.h>

/* 在BL31退出到非安全世界前调用 */
void check_scr_el3_before_exit(void)
{
    uint64_t scr_el3 = read_scr_el3();
    
    INFO("========================================\n");
    INFO("[BL31退出前] SCR_EL3配置检查:\n");
    INFO("  SCR_EL3 = 0x%016llx\n", scr_el3);
    INFO("  SCR_EL3.EA (bit 3) = %d\n", (scr_el3 & SCR_EA_BIT) ? 1 : 0);
    
    if (scr_el3 & SCR_EA_BIT) {
        INFO("  -> SError将路由到EL3 (FFH)\n");
    } else {
        INFO("  -> SError将路由到低异常等级 (KFH)\n");
        WARN("  警告: HANDLE_EA_EL3_FIRST_NS可能未启用!\n");
    }
    INFO("========================================\n");
}

/* 
 * 注意：在非安全EL1中无法直接读取SCR_EL3（EL3特权寄存器）
 * 需要通过以下方式之一来检测：
 * 1. 通过SMC调用到EL3读取（需要实现对应的SMC服务）
 * 2. 通过观察SError的实际路由行为来判断
 * 3. 在EL3异常处理函数中添加打印
 */

/* 方法1：通过SMC调用到EL3读取SCR_EL3（需要在EL3实现对应的SMC服务） */
void check_scr_el3_via_smc(void)
{
    /* 假设有一个SMC服务可以返回SCR_EL3的值 */
    /* 需要先在EL3实现对应的SMC handler */
    /*
    smc_result_t result = tftf_smc(ARM_ARCH_SVC_UID, 
                                    ARM_ARCH_SVC_GET_SCR_EL3, 
                                    0, 0, 0, 0, 0, 0);
    uint64_t scr_el3 = result.ret0;
    
    tftf_testcase_printf("SCR_EL3 = 0x%016llx\n", scr_el3);
    tftf_testcase_printf("SCR_EL3.EA = %d\n", (scr_el3 >> 3) & 1);
    */
}

/* 方法2：通过观察SError路由行为来判断（推荐） */
void check_serror_routing_by_behavior(void)
{
    tftf_testcase_printf("========================================\n");
    tftf_testcase_printf("检测SError路由配置:\n");
    tftf_testcase_printf("  方法：通过观察SError的实际路由行为\n");
    tftf_testcase_printf("  如果SError被路由到EL3，会在EL3异常处理函数中打印\n");
    tftf_testcase_printf("  如果SError被路由到当前EL，会触发当前EL的异常处理\n");
    tftf_testcase_printf("========================================\n");
    
    /* 注入错误后，观察错误被路由到哪里 */
    /* 如果路由到EL3，会在EL3的serror_aarch64入口处打印 */
    /* 如果路由到当前EL，会在当前EL的异常处理中触发 */
}

