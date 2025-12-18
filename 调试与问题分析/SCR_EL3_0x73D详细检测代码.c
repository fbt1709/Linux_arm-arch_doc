/*
 * SCR_EL3 = 0x73D 详细分析代码
 * 
 * 用于在注入错误前后检查SCR_EL3和相关寄存器
 */

#include <tftf_lib.h>

/* 通过SMC读取SCR_EL3（需要在EL3实现对应的SMC服务） */
uint64_t get_scr_el3_via_smc(void)
{
    /* 假设有一个SMC服务可以返回SCR_EL3的值 */
    /* 需要在EL3实现对应的SMC handler */
    smc_result_t result = tftf_smc(ARM_ARCH_SVC_UID, 
                                    ARM_ARCH_SVC_GET_SCR_EL3, 
                                    0, 0, 0, 0, 0, 0);
    return result.ret0;
}

/* 分析SCR_EL3的各个位 */
void analyze_scr_el3(uint64_t scr_el3)
{
    tftf_testcase_printf("========================================\n");
    tftf_testcase_printf("SCR_EL3 = 0x%016llx 详细分析:\n", scr_el3);
    tftf_testcase_printf("  NS (bit 0) = %d - %s\n", 
                         (scr_el3 >> 0) & 1,
                         (scr_el3 >> 0) & 1 ? "非安全" : "安全");
    tftf_testcase_printf("  IRQ (bit 1) = %d\n", (scr_el3 >> 1) & 1);
    tftf_testcase_printf("  FIQ (bit 2) = %d\n", (scr_el3 >> 2) & 1);
    tftf_testcase_printf("  EA (bit 3) = %d - %s <-- 关键位\n", 
                         (scr_el3 >> 3) & 1,
                         (scr_el3 >> 3) & 1 ? "路由到EL3" : "路由到低异常等级");
    tftf_testcase_printf("  RES1 (bit 4) = %d\n", (scr_el3 >> 4) & 1);
    tftf_testcase_printf("  RES0 (bit 5) = %d\n", (scr_el3 >> 5) & 1);
    tftf_testcase_printf("  RES0 (bit 6) = %d\n", (scr_el3 >> 6) & 1);
    tftf_testcase_printf("  SMD (bit 7) = %d\n", (scr_el3 >> 7) & 1);
    tftf_testcase_printf("  HCE (bit 8) = %d\n", (scr_el3 >> 8) & 1);
    tftf_testcase_printf("  SIF (bit 9) = %d\n", (scr_el3 >> 9) & 1);
    tftf_testcase_printf("  RW (bit 10) = %d - %s\n", 
                         (scr_el3 >> 10) & 1,
                         (scr_el3 >> 10) & 1 ? "AArch64" : "AArch32");
    tftf_testcase_printf("  ST (bit 11) = %d\n", (scr_el3 >> 11) & 1);
    tftf_testcase_printf("========================================\n");
}

/* 检查RAS相关寄存器 */
void check_ras_registers(void)
{
    uint64_t errselr, erxctlr, erxpfgctl, erxpfgcdn;
    
    /* 选择错误记录0 */
    asm volatile("msr errselr_el1, %0" : : "r" (0ULL));
    isb();
    
    /* 读取ERXCTLR_EL1 */
    asm volatile("mrs %0, erxctlr_el1" : "=r" (erxctlr));
    
    /* 读取ERXPFGCTL_EL1 */
    asm volatile("mrs %0, erxpfgctl_el1" : "=r" (erxpfgctl));
    
    /* 读取ERXPFGCDN_EL1 */
    asm volatile("mrs %0, erxpfgcdn_el1" : "=r" (erxpfgcdn));
    
    tftf_testcase_printf("========================================\n");
    tftf_testcase_printf("RAS错误记录寄存器 (记录0):\n");
    tftf_testcase_printf("  ERXCTLR_EL1 = 0x%016llx\n", erxctlr);
    tftf_testcase_printf("    ED (bit 0) = %d - 错误检测使能\n", (erxctlr >> 0) & 1);
    tftf_testcase_printf("    UE (bit 4) = %d - 不可纠正错误\n", (erxctlr >> 4) & 1);
    tftf_testcase_printf("    CE (bit 5) = %d - 可纠正错误\n", (erxctlr >> 5) & 1);
    tftf_testcase_printf("  ERXPFGCTL_EL1 = 0x%016llx\n", erxpfgctl);
    tftf_testcase_printf("    UC (bit 1) = %d - Uncontainable错误\n", (erxpfgctl >> 1) & 1);
    tftf_testcase_printf("    UEU (bit 2) = %d - Unrecoverable错误\n", (erxpfgctl >> 2) & 1);
    tftf_testcase_printf("    CDEN (bit 31) = %d - 倒计时使能\n", (erxpfgctl >> 31) & 1);
    tftf_testcase_printf("  ERXPFGCDN_EL1 = 0x%016llx - 倒计数值\n", erxpfgcdn);
    tftf_testcase_printf("========================================\n");
}

/* 改进的test_uncontainable，添加详细检测 */
test_result_t test_uncontainable_detailed(void)
{
    uint64_t scr_before, scr_after;
    
    tftf_testcase_printf("\n=== 开始Uncontainable错误测试 ===\n\n");
    
    /* 1. 注入错误前检查SCR_EL3 */
    tftf_testcase_printf("步骤1: 注入错误前检查SCR_EL3\n");
    scr_before = get_scr_el3_via_smc();
    analyze_scr_el3(scr_before);
    
    if (!((scr_before >> 3) & 1)) {
        tftf_testcase_printf("警告: SCR_EL3.EA = 0，SError不会路由到EL3！\n");
    }
    
    /* 2. 检查RAS寄存器 */
    tftf_testcase_printf("\n步骤2: 检查RAS寄存器配置\n");
    check_ras_registers();
    
    /* 3. 注入错误 */
    tftf_testcase_printf("\n步骤3: 注入Uncontainable错误\n");
    inject_uncontainable_ras_error();
    
    /* 4. 注入错误后立即检查SCR_EL3 */
    tftf_testcase_printf("\n步骤4: 注入错误后立即检查SCR_EL3\n");
    scr_after = get_scr_el3_via_smc();
    analyze_scr_el3(scr_after);
    
    /* 5. 比较前后SCR_EL3是否变化 */
    if (scr_before != scr_after) {
        tftf_testcase_printf("警告: SCR_EL3在注入错误后发生了变化！\n");
        tftf_testcase_printf("  注入前: 0x%016llx\n", scr_before);
        tftf_testcase_printf("  注入后: 0x%016llx\n", scr_after);
    }
    
    /* 6. 等待错误触发（如果还没有触发） */
    tftf_testcase_printf("\n步骤5: 等待SError触发...\n");
    tftf_testcase_printf("  如果SError路由到EL3，会在EL3异常处理函数中打印\n");
    tftf_testcase_printf("  如果SError路由到TFTF，会在当前EL处理\n");
    
    tftf_testcase_printf("\n=== 测试完成 ===\n");
    
    return TEST_RESULT_SUCCESS;
}

