# Uncontainable错误路由到EL2分析

## 问题

在`test_uncontainable()`中触发SError时，SPSR_EL3显示EL2h（M[3:0] = 0x9），说明SError被路由到了EL2，而不是EL3。

## 执行流程分析

### 1. 测试执行环境

```
TFTF运行在: NS-EL1（非安全EL1）
    ↓
调用 inject_uncontainable_ras_error()
    ↓
配置RAS错误注入寄存器
    ↓
SError触发
    ↓
路由决策：
  - HCR_EL2.AMO = 1? → 路由到EL2
  - HCR_EL2.AMO = 0 且 SCR_EL3.EA = 1? → 路由到EL3
  - 否则 → 路由到当前EL（EL1）
    ↓
实际路由到: EL2（根据SPSR_EL3.M[3:0] = 0x9判断）
```

### 2. SPSR_EL3 = 0x80000249 的含义

**M[3:0] = 0x9 = EL2h** 表示：
- 异常发生时的状态是EL2
- 使用SP_EL2作为栈指针
- **SError被路由到了EL2处理**

## 可能的原因

### 原因1：HCR_EL2.AMO = 1（最可能）

**HCR_EL2.AMO（bit 5）控制SError路由**：
- **AMO = 1** → SError路由到EL2（最高优先级）
- **AMO = 0** → 根据SCR_EL3.EA决定路由

**如果HCR_EL2.AMO = 1**：
- 即使SCR_EL3.EA = 1，SError也会先路由到EL2
- EL2可以处理或转发异常到EL3

### 原因2：系统有EL2且配置了路由

如果系统启用了虚拟化（EL2）：
- EL2可能配置了SError路由
- HCR_EL2.AMO可能被设置为1

### 原因3：Uncontainable错误的特殊行为

Uncontainable错误（ERXPFGCTL_UC_BIT）可能有特殊的路由机制：
- 某些平台可能对Uncontainable错误有特殊处理
- 可能不受SCR_EL3.EA控制

## 验证方法

### 方法1：在EL3异常处理函数中检查

在EL3的`serror_aarch64`入口处添加：

```c
void check_serror_routing_detailed(void)
{
    uint64_t scr_el3 = read_scr_el3();
    uint64_t hcr_el2 = read_hcr_el2();  // EL3可以读取HCR_EL2
    uint64_t spsr_el3 = read_spsr_el3();
    
    INFO("========================================\n");
    INFO("SError路由分析:\n");
    INFO("  SCR_EL3 = 0x%016llx\n", scr_el3);
    INFO("    EA (bit 3) = %d\n", (scr_el3 >> 3) & 1);
    INFO("  HCR_EL2 = 0x%016llx\n", hcr_el2);
    INFO("    AMO (bit 5) = %d\n", (hcr_el2 >> 5) & 1);
    INFO("  SPSR_EL3 = 0x%016llx\n", spsr_el3);
    INFO("    M[3:0] = 0x%llx\n", spsr_el3 & 0xF);
    
    if ((hcr_el2 >> 5) & 1) {
        INFO("  -> HCR_EL2.AMO = 1，SError路由到EL2 ✓\n");
        INFO("     这解释了为什么SPSR_EL3.M[3:0] = EL2h\n");
    } else if ((scr_el3 >> 3) & 1) {
        INFO("  -> HCR_EL2.AMO = 0 且 SCR_EL3.EA = 1\n");
        INFO("     但SPSR_EL3显示EL2h，异常！\n");
    }
    INFO("========================================\n");
}
```

### 方法2：检查EL2是否存在

```c
// 在TFTF中检查EL2是否启用
unsigned int get_current_el(void)
{
    unsigned int el;
    asm volatile("mrs %0, CurrentEL" : "=r" (el));
    return (el >> 2) & 0x3;
}

// 检查是否可以访问EL2寄存器
// 在EL1中无法直接读取HCR_EL2，需要通过SMC
```

### 方法3：对比普通SError和Uncontainable错误

```c
// 测试1：普通SError
test_normal_serror();  // 检查SPSR_EL3.M[3:0]

// 测试2：Uncontainable错误
test_uncontainable();  // 检查SPSR_EL3.M[3:0]

// 如果两者都显示EL2h，说明是HCR_EL2.AMO=1
// 如果只有Uncontainable显示EL2h，说明是Uncontainable的特殊行为
```

## 解决方案

### 如果HCR_EL2.AMO = 1导致路由到EL2

**选项1：修改HCR_EL2.AMO = 0**
- 在EL2初始化时设置HCR_EL2.AMO = 0
- 这样SError会根据SCR_EL3.EA路由

**选项2：在EL2处理SError**
- 如果系统需要EL2处理SError，这是正常行为
- 可以在EL2的异常处理函数中处理

**选项3：EL2转发到EL3**
- EL2可以处理SError后转发到EL3
- 需要检查EL2的异常处理逻辑

## 总结

**SPSR_EL3.M[3:0] = 0x9 (EL2h) 表明**：
1. ✅ **SError被路由到了EL2**
2. ❓ **可能原因：HCR_EL2.AMO = 1**
3. ❓ **或者：Uncontainable错误的特殊路由机制**

**需要检查**：
- HCR_EL2.AMO的值
- EL2是否存在且启用
- 普通SError是否也路由到EL2（对比测试）

