/*
 * 在EL3中检测SError路由配置 - 推荐方法
 * 
 * 在EL3异常处理函数中添加，可以直接读取SCR_EL3
 */

#include <arch_helpers.h>
#include <common/debug.h>

/* 
 * 在serror_aarch64入口处调用
 * 位置：bl31/aarch64/runtime_exceptions.S
 */
void check_serror_routing_in_el3(void)
{
    uint64_t scr_el3 = read_scr_el3();
    uint64_t hcr_el2 = read_hcr_el2();  // EL3可以读取HCR_EL2
    uint64_t esr_el3 = read_esr_el3();
    uint64_t elr_el3 = read_elr_el3();
    uint64_t spsr_el3 = read_spsr_el3();
    
    INFO("========================================\n");
    INFO("[EL3] SError异常处理入口 - 完整分析\n");
    INFO("========================================\n");
    
    /* 1. 检查SCR_EL3 */
    INFO("1. SCR_EL3配置:\n");
    INFO("   SCR_EL3 = 0x%016llx\n", scr_el3);
    INFO("   EA (bit 3) = %d\n", (scr_el3 & SCR_EA_BIT) ? 1 : 0);
    
    /* 2. 检查HCR_EL2 */
    INFO("\n2. HCR_EL2配置:\n");
    INFO("   HCR_EL2 = 0x%016llx\n", hcr_el2);
    INFO("   AMO (bit 5) = %d\n", (hcr_el2 >> 5) & 1);
    
    if ((hcr_el2 >> 5) & 1) {
        INFO("   -> HCR_EL2.AMO = 1，SError路由到EL2（最高优先级）\n");
    } else {
        INFO("   -> HCR_EL2.AMO = 0，根据SCR_EL3.EA决定路由\n");
    }
    
    /* 3. 检查SPSR_EL3 */
    INFO("\n3. SPSR_EL3（异常发生时的状态）:\n");
    INFO("   SPSR_EL3 = 0x%016llx\n", spsr_el3);
    uint64_t m = spsr_el3 & 0xF;
    INFO("   M[3:0] = 0x%llx - ", m);
    if (m == 0x9) {
        INFO("EL2h（异常在EL2处理）\n");
    } else if (m == 0x3) {
        INFO("EL1h（异常在EL1发生）\n");
    } else if (m == 0xD) {
        INFO("EL3h（异常在EL3发生）\n");
    } else {
        INFO("其他状态\n");
    }
    
    /* 4. 综合判断 */
    INFO("\n4. 路由判断:\n");
    if ((hcr_el2 >> 5) & 1) {
        INFO("   HCR_EL2.AMO = 1 → SError路由到EL2 ✓\n");
        if (m == 0x9) {
            INFO("   SPSR_EL3.M[3:0] = EL2h，与路由配置一致 ✓\n");
        }
    } else if ((scr_el3 & SCR_EA_BIT)) {
        INFO("   HCR_EL2.AMO = 0 且 SCR_EL3.EA = 1 → SError应该路由到EL3\n");
        if (m == 0x9) {
            WARN("   但SPSR_EL3.M[3:0] = EL2h，异常！\n");
            WARN("   可能EL2转发了异常到EL3\n");
        }
    }
    
    /* 5. 其他信息 */
    INFO("\n5. 其他信息:\n");
    INFO("   ESR_EL3 = 0x%016llx\n", esr_el3);
    INFO("   ELR_EL3 = 0x%016llx\n", elr_el3);
    
    INFO("========================================\n");
}

/* 
 * 在BL31退出到非安全世界前调用
 * 位置：lib/el3_runtime/aarch64/context_mgmt.c的cm_prepare_el3_exit()
 */
void check_scr_el3_before_exit_to_ns(void)
{
    uint64_t scr_el3 = read_scr_el3();
    
    INFO("========================================\n");
    INFO("[BL31退出到NS前] SCR_EL3配置:\n");
    INFO("  SCR_EL3 = 0x%016llx\n", scr_el3);
    INFO("  SCR_EL3.EA = %d\n", (scr_el3 & SCR_EA_BIT) ? 1 : 0);
    
    if (scr_el3 & SCR_EA_BIT) {
        INFO("  -> 退出后SError将路由到EL3 (FFH) ✓\n");
    } else {
        INFO("  -> 退出后SError将路由到低异常等级 (KFH) ✗\n");
        WARN("  警告: HANDLE_EA_EL3_FIRST_NS可能未启用！\n");
    }
    INFO("========================================\n");
}

