# SCR_EL3 = 0x73D 分析

## SCR_EL3寄存器值解析

**SCR_EL3 = 0x73D = 0b011100111101**

### 位域分析

| 位 | 名称 | 值 | 含义 |
|---|------|-----|------|
| 0 | NS | 1 | 非安全状态 |
| 1 | IRQ | 0 | IRQ路由配置 |
| 2 | FIQ | 1 | FIQ路由配置 |
| **3** | **EA** | **1** | **SError路由到EL3 ✓** |
| 4 | RES1 | 1 | 保留位（必须为1） |
| 5 | RES0 | 1 | 保留位（通常为0，但这里为1） |
| 6 | RES0 | 1 | 保留位（通常为0，但这里为1） |
| 7 | SMD | 0 | SMC指令禁用 |
| 8 | HCE | 0 | HVC指令使能 |
| 9 | SIF | 1 | 安全指令获取 |
| 10 | RW | 1 | 执行状态（1=AArch64） |
| 11 | ST | 1 | 安全定时器 |
| 12 | TWI | 0 | 陷阱WFE指令 |
| 13 | TWE | 0 | 陷阱WFI指令 |
| ... | ... | ... | ... |

### 关键发现

**SCR_EL3.EA (bit 3) = 1** ✓

这意味着：
- **SError应该路由到EL3**（FFH模式）
- 配置是正确的

## 问题分析

如果SCR_EL3.EA=1，但SError仍然路由到TFTF，可能的原因：

### 1. 时序问题

**可能情况**：
- BL31退出时：SCR_EL3.EA = 1
- 但在TFTF执行`inject_uncontainable_ras_error()`时，SCR_EL3可能被修改了

**检查方法**：
- 在注入错误**之前**立即读取SCR_EL3
- 在注入错误**之后**立即读取SCR_EL3
- 在SError触发时读取SCR_EL3

### 2. Uncontainable错误的特殊行为

**Uncontainable错误**（ERXPFGCTL_UC_BIT）可能有特殊的路由行为：

```assembly
// inject_uncontainable_ras_error()
mov x2, #ERXPFGCTL_UC_BIT  // Uncontainable错误
```

**可能原因**：
- Uncontainable错误可能不受SCR_EL3.EA控制
- 或者有其他的路由机制覆盖了SCR_EL3.EA

### 3. 错误注入时的执行上下文

**关键问题**：错误注入时，CPU在哪个异常等级？

- 如果错误注入时在**非安全EL1**（TFTF）：
  - 即使SCR_EL3.EA=1，错误可能首先在EL1被处理
  - 然后才路由到EL3

### 4. RAS错误的路由机制

RAS错误可能有独立的路由机制，不完全依赖SCR_EL3.EA：

- **ERXCTLR_EL1**寄存器可能控制错误路由
- **ERXPFGCTL_EL1**的错误类型可能影响路由

## 调试建议

### 1. 在注入错误前后检查SCR_EL3

```c
// 在test_uncontainable()中
test_result_t test_uncontainable(void)
{
    // 注入错误前检查
    uint64_t scr_before = get_scr_el3_via_smc();
    tftf_testcase_printf("注入错误前: SCR_EL3 = 0x%016llx, EA = %d\n", 
                         scr_before, (scr_before >> 3) & 1);
    
    // 注入错误
    inject_uncontainable_ras_error();
    
    // 注入错误后立即检查
    uint64_t scr_after = get_scr_el3_via_smc();
    tftf_testcase_printf("注入错误后: SCR_EL3 = 0x%016llx, EA = %d\n", 
                         scr_after, (scr_after >> 3) & 1);
    
    return TEST_RESULT_SUCCESS;
}
```

### 2. 在EL3异常处理函数中添加详细打印

```c
// 在serror_aarch64入口处
void check_serror_routing_detailed(void)
{
    uint64_t scr_el3 = read_scr_el3();
    uint64_t esr_el3 = read_esr_el3();
    uint64_t elr_el3 = read_elr_el3();
    uint64_t far_el3 = read_far_el3();
    
    INFO("========================================\n");
    INFO("[EL3] SError异常处理 - 详细分析\n");
    INFO("  SCR_EL3 = 0x%016llx\n", scr_el3);
    INFO("    EA (bit 3) = %d\n", (scr_el3 >> 3) & 1);
    INFO("    NS (bit 0) = %d\n", (scr_el3 >> 0) & 1);
    INFO("  ESR_EL3 = 0x%016llx\n", esr_el3);
    INFO("    EC = 0x%02llx\n", (esr_el3 >> 26) & 0x3f);
    INFO("  ELR_EL3 = 0x%016llx\n", elr_el3);
    INFO("  FAR_EL3 = 0x%016llx\n", far_el3);
    
    // 检查ESR_EL3的EC字段，判断错误类型
    uint64_t ec = (esr_el3 >> 26) & 0x3f;
    if (ec == 0x2f) {  // SError from lower EL
        INFO("  错误类型: 来自低异常等级的SError\n");
    }
    
    INFO("========================================\n");
}
```

### 3. 检查ERXCTLR_EL1和ERXPFGCTL_EL1

Uncontainable错误的路由可能受这些寄存器控制：

```c
// 在注入错误后检查RAS寄存器
void check_ras_registers(void)
{
    uint64_t errselr, erxctlr, erxpfgctl;
    
    // 选择错误记录0
    asm volatile("msr errselr_el1, %0" : : "r" (0ULL));
    isb();
    
    // 读取ERXCTLR_EL1
    asm volatile("mrs %0, erxctlr_el1" : "=r" (erxctlr));
    
    // 读取ERXPFGCTL_EL1
    asm volatile("mrs %0, erxpfgctl_el1" : "=r" (erxpfgctl));
    
    tftf_testcase_printf("ERXCTLR_EL1 = 0x%016llx\n", erxctlr);
    tftf_testcase_printf("ERXPFGCTL_EL1 = 0x%016llx\n", erxpfgctl);
    tftf_testcase_printf("  UC bit = %d\n", (erxpfgctl >> 1) & 1);
}
```

## 可能的原因总结

1. **SCR_EL3.EA=1是正确的**，但SError在到达EL3之前被TFTF处理了
2. **Uncontainable错误可能有特殊的路由行为**
3. **时序问题**：错误注入时SCR_EL3可能被临时修改
4. **RAS寄存器配置**：ERXCTLR_EL1或ERXPFGCTL_EL1可能影响路由

## 下一步调试

1. 在注入错误**前后**都检查SCR_EL3
2. 在EL3的`serror_aarch64`入口处添加详细打印
3. 检查RAS相关寄存器（ERXCTLR_EL1, ERXPFGCTL_EL1）
4. 确认错误注入时的执行上下文（EL等级）

