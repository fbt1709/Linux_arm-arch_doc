# Uncontainable错误路由到TFTF深度分析

## 问题描述

**配置检查**：
- HCR_EL2.AMO = 0（注入错误前检查）
- SCR_EL3.EA = 1（0x73D）
- **但SError仍然路由到TFTF处理**

## 可能的原因分析

### 原因1：Uncontainable错误的特殊路由机制

**Uncontainable错误（ERXPFGCTL_UC_BIT）可能有独立的路由配置**：

```c
// inject_uncontainable_ras_error()
mov x2, #ERXPFGCTL_UC_BIT  // Uncontainable错误
```

**可能机制**：
- Uncontainable错误可能不受SCR_EL3.EA控制
- 可能有平台特定的路由配置
- 某些平台可能对Uncontainable错误有特殊处理

### 原因2：RAS错误记录的路由配置

**ERXCTLR_EL1或ERXPFGCTL_EL1可能包含路由配置**：

```c
// 检查错误记录的配置
msr errselr_el1, #0
isb
mrs x0, erxctlr_el1
mrs x1, erxpfgctl_el1

// 检查是否有路由相关的位
// 某些平台可能在错误记录中配置路由
```

**可能位域**：
- ERXCTLR_EL1可能有路由控制位
- ERXPFGCTL_EL1可能有路由控制位
- 这些位可能覆盖SCR_EL3.EA

### 原因3：错误注入时的执行上下文

**关键问题**：错误注入时，CPU在哪个异常等级？

```c
// TFTF在EL1执行
test_uncontainable() {
    // 此时CurrentEL = EL1
    inject_uncontainable_ras_error();  // 在EL1注入错误
    // 错误在EL1的上下文中触发
}
```

**可能机制**：
- 如果错误在EL1注入，可能首先在EL1被处理
- 即使SCR_EL3.EA=1，错误可能先触发EL1的异常处理
- 然后才路由到EL3

### 原因4：EL2的异常向量表拦截

**即使HCR_EL2.AMO = 0，EL2仍可能拦截异常**：

```c
// 检查EL2的异常向量表
uint64_t vbar_el2 = read_vbar_el2();
// 如果VBAR_EL2设置了异常向量表，EL2可能有SError处理程序
// EL2可能主动拦截SError，即使AMO=0
```

**可能机制**：
- EL2的异常向量表（VBAR_EL2）配置了SError处理
- EL2在异常路由到EL3之前拦截
- 这是软件行为，不是硬件路由

### 原因5：时序问题 - 注入错误后配置被修改

**可能情况**：
- 注入错误前：HCR_EL2.AMO = 0, SCR_EL3.EA = 1
- 注入错误后：配置可能被修改
- 错误触发时：实际配置可能不同

**验证方法**：
```c
// 在注入错误前后都检查
uint64_t hcr_before = get_hcr_el2_via_smc();
uint64_t scr_before = get_scr_el3_via_smc();

inject_uncontainable_ras_error();

// 立即检查（如果可能）
uint64_t hcr_after = get_hcr_el2_via_smc();
uint64_t scr_after = get_scr_el3_via_smc();
```

### 原因6：平台特定的路由配置

**某些平台可能有平台特定的路由机制**：

```c
// 平台可能覆盖了标准路由配置
// 检查平台特定的配置寄存器
// 某些平台可能在初始化时设置了特殊路由
```

### 原因7：错误注入寄存器的路由配置

**ERXCTLR_EL1或ERXPFGCTL_EL1可能控制路由**：

```c
// 检查错误记录的完整配置
void check_ras_error_record_config(void)
{
    msr errselr_el1, #0
    isb
    
    uint64_t erxctlr, erxpfgctl, erxstatus;
    mrs erxctlr, erxctlr_el1
    mrs erxpfgctl, erxpfgctl_el1
    mrs erxstatus, erxstatus_el1
    
    // 检查是否有路由控制位
    // 某些平台可能在错误记录中配置路由目标
}
```

### 原因8：Uncontainable错误的硬件行为

**ARM架构中，Uncontainable错误可能有特殊行为**：

- Uncontainable错误可能**总是**路由到当前EL
- 不受SCR_EL3.EA控制
- 这是ARM架构的特定行为

## 验证方法

### 方法1：检查错误注入前后的配置

```c
test_result_t test_uncontainable_with_check(void)
{
    uint64_t hcr_before, scr_before;
    uint64_t hcr_after, scr_after;
    
    // 注入错误前检查
    hcr_before = get_hcr_el2_via_smc();
    scr_before = get_scr_el3_via_smc();
    
    tftf_testcase_printf("注入错误前:\n");
    tftf_testcase_printf("  HCR_EL2 = 0x%016llx, AMO = %d\n", 
                         hcr_before, (hcr_before >> 5) & 1);
    tftf_testcase_printf("  SCR_EL3 = 0x%016llx, EA = %d\n", 
                         scr_before, (scr_before >> 3) & 1);
    
    // 注入错误
    inject_uncontainable_ras_error();
    
    // 注入错误后立即检查（如果可能）
    // 注意：错误可能已经触发，需要快速检查
    
    return TEST_RESULT_SUCCESS;
}
```

### 方法2：检查RAS错误记录配置

```c
void check_ras_error_record_routing(void)
{
    msr errselr_el1, #0
    isb
    
    uint64_t erxctlr, erxpfgctl;
    mrs erxctlr, erxctlr_el1
    mrs erxpfgctl, erxpfgctl_el1
    
    tftf_testcase_printf("错误记录0配置:\n");
    tftf_testcase_printf("  ERXCTLR_EL1 = 0x%016llx\n", erxctlr);
    tftf_testcase_printf("  ERXPFGCTL_EL1 = 0x%016llx\n", erxpfgctl);
    tftf_testcase_printf("    UC (bit 1) = %d\n", (erxpfgctl >> 1) & 1);
    
    // 检查是否有路由控制位
    // 某些平台可能在错误记录中配置路由
}
```

### 方法3：在EL3检查异常发生时的配置

```c
// 在EL3的serror_aarch64入口处
void check_config_at_exception(void)
{
    uint64_t hcr_el2 = read_hcr_el2();
    uint64_t scr_el3 = read_scr_el3();
    uint64_t spsr_el3 = read_spsr_el3();
    uint64_t esr_el3 = read_esr_el3();
    
    INFO("异常发生时的配置:\n");
    INFO("  HCR_EL2 = 0x%016llx, AMO = %d\n", hcr_el2, (hcr_el2 >> 5) & 1);
    INFO("  SCR_EL3 = 0x%016llx, EA = %d\n", scr_el3, (scr_el3 >> 3) & 1);
    INFO("  SPSR_EL3.M[3:0] = 0x%llx\n", spsr_el3 & 0xF);
    INFO("  ESR_EL3 = 0x%016llx\n", esr_el3);
    
    // 检查ESR_EL3.EC，确认错误类型
    uint64_t ec = (esr_el3 >> 26) & 0x3f;
    if (ec == 0x2f) {
        INFO("  错误类型: 来自低异常等级的SError\n");
    }
}
```

### 方法4：对比普通SError和Uncontainable错误

```c
// 测试1：普通SError
test_normal_serror();  
// 检查是否路由到EL3

// 测试2：Uncontainable错误
test_uncontainable();  
// 检查是否路由到TFTF

// 如果普通SError路由到EL3，但Uncontainable路由到TFTF
// 说明Uncontainable有特殊的路由机制
```

## 最可能的原因（按可能性排序）

### 原因A：Uncontainable错误的架构行为 ⭐⭐⭐⭐⭐

**ARM架构可能规定**：
- **Uncontainable错误可能总是路由到当前EL（FEL）**
- **不受SCR_EL3.EA控制**
- 这是ARM架构的特定行为，因为Uncontainable错误是"不可包含"的

**验证方法**：
- 查看ARM架构规范（ARMv8 RAS扩展）
- 对比普通SError和Uncontainable错误的路由行为
- 如果普通SError路由到EL3，但Uncontainable路由到当前EL，说明有特殊机制

### 原因B：错误注入时的执行上下文 ⭐⭐⭐⭐

**关键问题**：错误在EL1注入，可能在EL1的上下文中触发

```
TFTF在EL1执行
    ↓
调用 inject_uncontainable_ras_error()
    ↓
配置RAS错误注入寄存器（在EL1执行）
    ↓
错误触发（在EL1的上下文中）
    ↓
即使SCR_EL3.EA=1，错误可能先被EL1处理
    ↓
EL1的异常处理函数执行（如果配置了）
```

**可能机制**：
- 错误在EL1注入，可能在EL1的上下文中触发
- EL1的异常向量表（VBAR_EL1）可能配置了SError处理
- EL1处理异常后，可能不转发到EL3

### 原因C：EL2的软件拦截 ⭐⭐⭐

**即使HCR_EL2.AMO = 0**：
- EL2的异常向量表（VBAR_EL2）可能配置了SError处理
- EL2在硬件路由到EL3之前拦截
- 这是软件行为，不是硬件路由

**验证方法**：
```c
// 检查EL2的异常向量表
uint64_t vbar_el2 = read_vbar_el2();
// 如果设置了，EL2可能有SError处理程序
```

### 原因D：RAS错误记录的路由配置 ⭐⭐

**ERXCTLR_EL1或ERXPFGCTL_EL1可能包含路由配置**：
- 某些平台可能在错误记录中配置路由目标
- 这些配置可能覆盖SCR_EL3.EA

### 原因E：时序问题 ⭐

**注入错误后配置被修改**：
- 虽然注入前检查AMO=0，但错误触发时可能被修改
- 需要检查异常发生时的实际配置

## 解决方案

### 方案1：检查ARM架构规范

查看ARM架构规范，确认Uncontainable错误的路由行为：
- 是否总是路由到当前EL？
- 是否受SCR_EL3.EA控制？
- 是否有特殊的路由规则？

### 方案2：在EL3添加详细日志

在EL3异常处理函数中添加完整的配置检查：
- 异常发生时的HCR_EL2和SCR_EL3
- ESR_EL3的错误类型
- SPSR_EL3的状态

### 方案3：检查平台特定配置

某些平台可能有平台特定的路由配置：
- 检查平台初始化代码
- 检查是否有覆盖标准路由的配置

## 总结

**可能的原因（按可能性排序）**：

1. **Uncontainable错误的架构行为**：可能总是路由到当前EL
2. **错误注入时的执行上下文**：在EL1注入，可能在EL1触发
3. **EL2的软件拦截**：EL2异常向量表拦截异常
4. **RAS错误记录的路由配置**：ERXCTLR_EL1或ERXPFGCTL_EL1控制路由
5. **时序问题**：注入错误后配置被修改
6. **平台特定配置**：平台覆盖了标准路由

**需要进一步调查**：
- ARM架构规范中Uncontainable错误的路由规则
- 错误注入时的实际执行上下文
- EL2的异常向量表配置
- RAS错误记录的完整配置

