/*
 * 通过SPSR_EL3和SCTLR分析SError路由
 */

#include <arch_helpers.h>
#include <common/debug.h>

/* 分析SPSR_EL3判断SError路由 */
void analyze_serror_routing_from_spsr(void)
{
    uint64_t spsr_el3 = read_spsr_el3();
    uint64_t esr_el3 = read_esr_el3();
    
    INFO("========================================\n");
    INFO("通过SPSR_EL3判断SError路由:\n");
    INFO("  SPSR_EL3 = 0x%016llx\n", spsr_el3);
    INFO("  ESR_EL3 = 0x%016llx\n", esr_el3);
    
    /* 提取M[3:0] - 异常发生时的异常等级 */
    uint64_t m = spsr_el3 & 0xF;
    INFO("  M[3:0] = 0x%llx - ", m);
    
    if (m == 0x3) {
        INFO("EL3 ✓ (SError路由到EL3)\n");
    } else if (m == 0x1) {
        INFO("EL1 ✗ (SError路由到EL1)\n");
    } else if (m == 0x2) {
        INFO("EL2 ✗ (SError路由到EL2)\n");
    } else {
        INFO("未知异常等级\n");
    }
    
    /* 检查A位（SError掩码） */
    if (spsr_el3 & (1ULL << 27)) {
        WARN("  警告: SError被掩码（A=1），可能不会触发\n");
    } else {
        INFO("  SError未掩码（A=0）\n");
    }
    
    /* 检查ESR_EL3.EC字段 */
    uint64_t ec = (esr_el3 >> 26) & 0x3f;
    INFO("  ESR_EL3.EC = 0x%02llx - ", ec);
    if (ec == 0x2f) {
        INFO("来自低异常等级的SError\n");
    } else {
        INFO("其他异常类型\n");
    }
    
    INFO("========================================\n");
}

/* 详细分析SPSR_EL3的所有位 */
void analyze_spsr_el3_detailed(void)
{
    uint64_t spsr_el3 = read_spsr_el3();
    
    INFO("========================================\n");
    INFO("SPSR_EL3 = 0x%016llx 详细分析:\n", spsr_el3);
    
    /* M[3:0] - 异常等级 */
    uint64_t m = spsr_el3 & 0xF;
    INFO("  M[3:0] = 0x%llx - ", m);
    if (m == 0x3) {
        INFO("EL3 ✓\n");
    } else if (m == 0x1) {
        INFO("EL1 ✗\n");
    } else if (m == 0x2) {
        INFO("EL2 ✗\n");
    } else {
        INFO("其他\n");
    }
    
    /* M[4] - 执行状态 */
    INFO("  M[4] = %d - %s\n", 
         (spsr_el3 >> 4) & 1,
         (spsr_el3 >> 4) & 1 ? "AArch64" : "AArch32");
    
    /* A位 - SError掩码 */
    INFO("  A (bit 27) = %d - SError%s\n",
         (spsr_el3 >> 27) & 1,
         (spsr_el3 >> 27) & 1 ? "被掩码" : "未掩码");
    
    /* I, F位 - 中断掩码 */
    INFO("  I (bit 26) = %d - IRQ%s\n",
         (spsr_el3 >> 26) & 1,
         (spsr_el3 >> 26) & 1 ? "被掩码" : "未掩码");
    INFO("  F (bit 25) = %d - FIQ%s\n",
         (spsr_el3 >> 25) & 1,
         (spsr_el3 >> 25) & 1 ? "被掩码" : "未掩码");
    
    /* D位 - 调试异常掩码 */
    INFO("  D (bit 28) = %d - 调试异常%s\n",
         (spsr_el3 >> 28) & 1,
         (spsr_el3 >> 28) & 1 ? "被掩码" : "未掩码");
    
    INFO("========================================\n");
}

/* 在EL3异常处理函数中调用 */
void check_serror_routing_complete(void)
{
    uint64_t spsr_el3 = read_spsr_el3();
    uint64_t esr_el3 = read_esr_el3();
    uint64_t scr_el3 = read_scr_el3();
    uint64_t elr_el3 = read_elr_el3();
    
    INFO("========================================\n");
    INFO("[EL3] SError异常处理 - 完整分析\n");
    INFO("========================================\n");
    
    /* 1. 检查SCR_EL3.EA */
    INFO("1. SCR_EL3配置:\n");
    INFO("   SCR_EL3 = 0x%016llx\n", scr_el3);
    INFO("   EA (bit 3) = %d - %s\n",
         (scr_el3 >> 3) & 1,
         (scr_el3 >> 3) & 1 ? "路由到EL3" : "路由到低异常等级");
    
    /* 2. 检查SPSR_EL3.M[3:0] - 异常发生时的异常等级 */
    INFO("\n2. SPSR_EL3分析（异常发生时的状态）:\n");
    uint64_t m = spsr_el3 & 0xF;
    INFO("   SPSR_EL3 = 0x%016llx\n", spsr_el3);
    INFO("   M[3:0] = 0x%llx - ", m);
    if (m == 0x3) {
        INFO("EL3 ✓ (SError确实路由到EL3)\n");
    } else if (m == 0x1) {
        INFO("EL1 ✗ (SError路由到EL1，但异常却到达了EL3？异常！)\n");
    } else {
        INFO("其他异常等级\n");
    }
    
    /* 3. 检查ESR_EL3 */
    INFO("\n3. ESR_EL3分析:\n");
    INFO("   ESR_EL3 = 0x%016llx\n", esr_el3);
    uint64_t ec = (esr_el3 >> 26) & 0x3f;
    INFO("   EC = 0x%02llx - ", ec);
    if (ec == 0x2f) {
        INFO("来自低异常等级的SError\n");
    } else {
        INFO("其他异常类型\n");
    }
    
    /* 4. 检查ELR_EL3 */
    INFO("\n4. ELR_EL3 (异常返回地址):\n");
    INFO("   ELR_EL3 = 0x%016llx\n", elr_el3);
    
    /* 5. 综合判断 */
    INFO("\n5. 综合判断:\n");
    if ((scr_el3 >> 3) & 1) {
        if (m == 0x3) {
            INFO("   ✓ SCR_EL3.EA=1 且 SPSR_EL3.M[3:0]=EL3\n");
            INFO("   ✓ SError正确路由到EL3\n");
        } else {
            WARN("   ✗ SCR_EL3.EA=1 但 SPSR_EL3.M[3:0]!=EL3\n");
            WARN("   ✗ 路由配置异常！\n");
        }
    } else {
        WARN("   ✗ SCR_EL3.EA=0，SError应该路由到低异常等级\n");
        if (m == 0x3) {
            WARN("   ✗ 但异常却到达了EL3，异常！\n");
        }
    }
    
    INFO("========================================\n");
}

