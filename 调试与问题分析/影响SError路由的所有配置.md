# 影响SError路由的所有配置

## 主要路由配置

### 1. SCR_EL3.EA (bit 3) - 主要配置

**功能**：控制SError和External Abort的路由

- **EA = 1**：SError路由到EL3（FFH模式）
- **EA = 0**：SError路由到低异常等级（KFH模式）

### 2. HCR_EL2.AMO (bit 5) - 最高优先级

**功能**：控制SError路由到EL2

- **AMO = 1**：SError路由到EL2（最高优先级，覆盖SCR_EL3.EA）
- **AMO = 0**：根据SCR_EL3.EA决定路由

## 其他可能影响的配置

### 3. SCR_EL3.HCE (bit 8) - EL2启用

**功能**：启用EL2（Hypervisor Call Enable）

- **HCE = 1**：EL2被启用，系统可能运行在EL2
- **HCE = 0**：EL2未启用

**影响**：
- 如果EL2被启用，即使HCR_EL2.AMO=0，EL2仍可能拦截异常
- EL2的异常向量表可能配置了SError处理

### 4. SCR_EL3.NS (bit 0) - 安全状态

**功能**：当前安全状态

- **NS = 1**：非安全状态
- **NS = 0**：安全状态

**影响**：
- 可能影响异常路由的目标安全状态

### 5. HCR_EL2.TGE (bit 27) - EL2配置

**功能**：Trap General Exceptions

- **TGE = 1**：EL2配置为捕获通用异常
- **TGE = 0**：正常配置

**你的HCR_EL2 = 0x88000000，TGE = 1**

**影响**：
- TGE=1可能影响异常处理行为
- 可能影响SError的路由或处理

### 6. VBAR_EL1 - EL1异常向量表

**功能**：EL1的异常向量表基地址

**影响**：
- 如果VBAR_EL1设置了异常向量表，EL1可能有SError处理程序
- EL1可能在硬件路由到EL3之前拦截异常
- **这是软件行为，不是硬件路由配置**

### 7. VBAR_EL2 - EL2异常向量表

**功能**：EL2的异常向量表基地址

**影响**：
- 如果VBAR_EL2设置了异常向量表，EL2可能有SError处理程序
- EL2可能在硬件路由到EL3之前拦截异常
- **这是软件行为，不是硬件路由配置**

### 8. SCTLR_EL1/EL2 - 系统控制寄存器

**功能**：系统控制配置

**可能影响的位**：
- **IESB (bit 13)**：Implicit Error Synchronization Barrier
  - IESB=1：隐式错误同步屏障，可能影响SError处理
- **其他位**：通常不影响路由，但可能影响处理行为

### 9. ERXCTLR_EL1 - RAS错误记录控制

**功能**：控制错误记录的行为

**可能影响的位**：
- **ED (bit 0)**：Error Detection Enable
- **UE (bit 4)**：Uncorrected Error
- **其他位**：可能包含路由相关的配置（平台特定）

### 10. ERXPFGCTL_EL1 - RAS错误注入控制

**功能**：控制错误注入的行为

**关键位**：
- **UC (bit 1)**：Uncontainable错误
- **UEU (bit 2)**：Unrecoverable错误
- **CDEN (bit 31)**：倒计时使能

**可能影响**：
- 某些平台可能在错误注入控制中配置路由
- Uncontainable错误可能有特殊的路由机制

### 11. MDCR_EL3 - Monitor Debug Configuration

**功能**：EL3调试配置

**可能影响的位**：
- **某些位**：可能影响异常处理行为

### 12. 平台特定配置

**某些平台可能有平台特定的路由配置**：
- 平台初始化代码可能设置特殊路由
- 平台特定的寄存器可能控制路由

## 路由优先级总结

### 硬件路由优先级（从高到低）

```
1. HCR_EL2.AMO = 1 → 路由到EL2（最高优先级）
   ↓
2. HCR_EL2.AMO = 0 且 SCR_EL3.EA = 1 → 路由到EL3
   ↓
3. 否则 → 路由到当前EL（FEL）
```

### 软件拦截（可能覆盖硬件路由）

```
硬件路由到EL3
    ↓
EL2的异常向量表（VBAR_EL2）拦截 → EL2处理
    ↓
EL1的异常向量表（VBAR_EL1）拦截 → EL1处理
```

## 完整检查清单

### 硬件路由配置

- [ ] **HCR_EL2.AMO** (bit 5) - 最高优先级
- [ ] **SCR_EL3.EA** (bit 3) - 主要配置
- [ ] **SCR_EL3.HCE** (bit 8) - EL2是否启用
- [ ] **SCR_EL3.NS** (bit 0) - 安全状态

### EL2相关配置

- [ ] **HCR_EL2.TGE** (bit 27) - EL2配置（你的值是1）
- [ ] **VBAR_EL2** - EL2异常向量表
- [ ] **SCTLR_EL2** - EL2系统控制

### EL1相关配置

- [ ] **VBAR_EL1** - EL1异常向量表（**关键！**）
- [ ] **SCTLR_EL1** - EL1系统控制
- [ ] **SCTLR_EL1.IESB** (bit 13) - 错误同步屏障

### RAS相关配置

- [ ] **ERXCTLR_EL1** - 错误记录控制
- [ ] **ERXPFGCTL_EL1** - 错误注入控制
- [ ] **ERXPFGCTL_EL1.UC** (bit 1) - Uncontainable错误

### 平台特定配置

- [ ] 平台初始化代码中的路由配置
- [ ] 平台特定的寄存器

## 最需要检查的配置

### 1. VBAR_EL1（最可能的原因）⭐⭐⭐⭐⭐

```c
// 检查EL1的异常向量表
uint64_t vbar_el1;
asm volatile("mrs %0, vbar_el1" : "=r" (vbar_el1));

if (vbar_el1 != 0) {
    // EL1配置了异常向量表
    // EL1可能有SError处理程序
    // 这可能是路由到TFTF的原因
}
```

### 2. HCR_EL2.TGE = 1（你的配置）⭐⭐⭐⭐

**你的HCR_EL2 = 0x88000000，TGE = 1**

**TGE = 1可能的影响**：
- EL2配置为捕获通用异常
- 可能影响SError的处理行为
- 需要查看ARM架构规范中TGE对SError的影响

### 3. ERXPFGCTL_EL1.UC（Uncontainable错误）⭐⭐⭐⭐⭐

**Uncontainable错误可能有特殊的路由机制**：
- ARM架构可能规定Uncontainable错误总是路由到当前EL
- 不受SCR_EL3.EA控制
- 这是ARM架构的特定行为

### 4. 错误注入时的执行上下文⭐⭐⭐⭐

**错误在EL1注入**：
- 错误可能在EL1的上下文中触发
- 即使SCR_EL3.EA=1，错误可能先被EL1处理

## 验证代码

### 完整配置检查

```c
void check_all_serror_routing_configs(void)
{
    uint64_t scr_el3, hcr_el2, vbar_el1, vbar_el2;
    uint64_t sctlr_el1, sctlr_el2;
    uint64_t erxctlr, erxpfgctl;
    
    // 1. 主要路由配置
    scr_el3 = get_scr_el3_via_smc();
    hcr_el2 = get_hcr_el2_via_smc();
    
    // 2. 异常向量表
    asm volatile("mrs %0, vbar_el1" : "=r" (vbar_el1));
    asm volatile("mrs %0, vbar_el2" : "=r" (vbar_el2));
    
    // 3. 系统控制寄存器
    asm volatile("mrs %0, sctlr_el1" : "=r" (sctlr_el1));
    asm volatile("mrs %0, sctlr_el2" : "=r" (sctlr_el2));
    
    // 4. RAS错误记录
    asm volatile("msr errselr_el1, %0" : : "r" (0ULL));
    asm volatile("isb");
    asm volatile("mrs %0, erxctlr_el1" : "=r" (erxctlr));
    asm volatile("mrs %0, erxpfgctl_el1" : "=r" (erxpfgctl));
    
    tftf_testcase_printf("========================================\n");
    tftf_testcase_printf("SError路由配置 - 完整检查\n");
    tftf_testcase_printf("========================================\n");
    
    // 主要路由配置
    tftf_testcase_printf("\n1. 主要路由配置:\n");
    tftf_testcase_printf("   SCR_EL3 = 0x%016llx\n", scr_el3);
    tftf_testcase_printf("     EA (bit 3) = %d\n", (scr_el3 >> 3) & 1);
    tftf_testcase_printf("     HCE (bit 8) = %d\n", (scr_el3 >> 8) & 1);
    tftf_testcase_printf("     NS (bit 0) = %d\n", (scr_el3 >> 0) & 1);
    tftf_testcase_printf("   HCR_EL2 = 0x%016llx\n", hcr_el2);
    tftf_testcase_printf("     AMO (bit 5) = %d\n", (hcr_el2 >> 5) & 1);
    tftf_testcase_printf("     TGE (bit 27) = %d\n", (hcr_el2 >> 27) & 1);
    
    // 异常向量表
    tftf_testcase_printf("\n2. 异常向量表:\n");
    tftf_testcase_printf("   VBAR_EL1 = 0x%016llx\n", vbar_el1);
    if (vbar_el1 != 0) {
        tftf_testcase_printf("     -> EL1配置了异常向量表！\n");
        tftf_testcase_printf("     -> EL1可能有SError处理程序\n");
    }
    tftf_testcase_printf("   VBAR_EL2 = 0x%016llx\n", vbar_el2);
    if (vbar_el2 != 0) {
        tftf_testcase_printf("     -> EL2配置了异常向量表\n");
    }
    
    // 系统控制寄存器
    tftf_testcase_printf("\n3. 系统控制寄存器:\n");
    tftf_testcase_printf("   SCTLR_EL1 = 0x%016llx\n", sctlr_el1);
    tftf_testcase_printf("     IESB (bit 13) = %d\n", (sctlr_el1 >> 13) & 1);
    tftf_testcase_printf("   SCTLR_EL2 = 0x%016llx\n", sctlr_el2);
    
    // RAS错误记录
    tftf_testcase_printf("\n4. RAS错误记录配置:\n");
    tftf_testcase_printf("   ERXCTLR_EL1 = 0x%016llx\n", erxctlr);
    tftf_testcase_printf("   ERXPFGCTL_EL1 = 0x%016llx\n", erxpfgctl);
    tftf_testcase_printf("     UC (bit 1) = %d - Uncontainable\n", (erxpfgctl >> 1) & 1);
    
    tftf_testcase_printf("\n========================================\n");
}
```

## 总结

**影响SError路由的配置（按重要性排序）**：

1. **⭐⭐⭐⭐⭐ HCR_EL2.AMO** - 最高优先级硬件路由
2. **⭐⭐⭐⭐⭐ SCR_EL3.EA** - 主要硬件路由配置
3. **⭐⭐⭐⭐⭐ VBAR_EL1** - EL1异常向量表（软件拦截）
4. **⭐⭐⭐⭐ ERXPFGCTL_EL1.UC** - Uncontainable错误的特殊机制
5. **⭐⭐⭐⭐ SCR_EL3.HCE** - EL2是否启用
6. **⭐⭐⭐ HCR_EL2.TGE** - EL2配置（你的值是1）
7. **⭐⭐⭐ VBAR_EL2** - EL2异常向量表
8. **⭐⭐ SCTLR_EL1.IESB** - 错误同步屏障
9. **⭐ 其他配置** - 平台特定或次要影响

**最需要检查的**：
- **VBAR_EL1**（最可能的原因）
- **ERXPFGCTL_EL1.UC**（Uncontainable错误的特殊机制）
- **HCR_EL2.TGE = 1**（你的配置，需要确认影响）

