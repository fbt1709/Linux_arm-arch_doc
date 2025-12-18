/*
 * 检查HCR_EL2路由配置
 * 
 * SPSR_EL3.M[3:0] = 0x9 (EL2h) 表明SError路由到EL2
 * 需要检查HCR_EL2的配置
 */

#include <arch_helpers.h>
#include <common/debug.h>

/* 检查HCR_EL2的SError路由配置 */
void check_hcr_el2_routing(void)
{
    uint64_t hcr_el2;
    uint64_t scr_el3 = read_scr_el3();
    
    /* 读取HCR_EL2（如果系统有EL2） */
    /* 注意：在EL3中可能无法直接读取HCR_EL2，需要通过上下文 */
    
    INFO("========================================\n");
    INFO("SError路由配置检查:\n");
    INFO("  SCR_EL3 = 0x%016llx\n", scr_el3);
    INFO("  SCR_EL3.EA (bit 3) = %d\n", (scr_el3 >> 3) & 1);
    
    INFO("\n分析:\n");
    if ((scr_el3 >> 3) & 1) {
        INFO("  SCR_EL3.EA = 1，SError应该路由到EL3\n");
        WARN("  但SPSR_EL3.M[3:0] = 0x9 (EL2h)，说明路由到了EL2\n");
        WARN("  可能原因：\n");
        WARN("    1. HCR_EL2.AMO = 1（SError路由到EL2）\n");
        WARN("    2. EL2有独立的路由配置覆盖了SCR_EL3.EA\n");
        WARN("    3. 异常处理顺序：先到EL2，再到EL3\n");
    }
    
    INFO("========================================\n");
}

/* 在EL2异常处理函数中检查HCR_EL2 */
void check_hcr_el2_in_el2(void)
{
    uint64_t hcr_el2;
    
    /* 在EL2中可以读取HCR_EL2 */
    asm volatile("mrs %0, hcr_el2" : "=r" (hcr_el2));
    
    printf("HCR_EL2 = 0x%016llx\n", hcr_el2);
    printf("  AMO (bit 5) = %d - SError路由到EL2\n", (hcr_el2 >> 5) & 1);
    printf("  TGE (bit 27) = %d - EL2配置\n", (hcr_el2 >> 27) & 1);
    
    if ((hcr_el2 >> 5) & 1) {
        printf("  -> HCR_EL2.AMO=1，SError路由到EL2\n");
    }
}

