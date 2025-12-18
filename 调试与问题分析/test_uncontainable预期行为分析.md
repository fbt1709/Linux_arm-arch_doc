# test_uncontainable预期行为分析

## 关键代码注释

在`inject_ras_error.S`第63行的注释：

```c
/*
 * Inject Uncontainable error through fault record 0. This function doesn't wait
 * as the handling is terminal in EL3.
 */
func inject_uncontainable_ras_error
```

**关键信息**：`the handling is terminal in EL3`

## 预期行为

### 注释说明的预期行为

**"terminal in EL3"** 表示：
- **Uncontainable错误应该在EL3中终止处理**
- **不应该返回到TFTF**
- **EL3应该处理这个错误并终止执行**

### 为什么是"terminal"？

**Uncontainable错误的特点**：
1. **不可包含**：错误无法被系统恢复
2. **终端错误**：通常会导致系统panic或重启
3. **必须在EL3处理**：因为这是系统级错误，需要最高特权级处理

### 预期执行流程

```
TFTF在EL1/EL2执行
    ↓
调用 inject_uncontainable_ras_error()
    ↓
注入Uncontainable错误
    ↓
SError触发
    ↓
应该路由到EL3（如果SCR_EL3.EA=1）
    ↓
EL3处理错误（terminal handling）
    ↓
系统panic或重启（不返回到TFTF）
```

## 实际行为 vs 预期行为

### 预期行为

- ✅ **SError路由到EL3**
- ✅ **EL3处理错误**
- ✅ **不返回到TFTF**（terminal）

### 实际行为（根据你的观察）

- ❌ **SError路由到EL2**（SPSR_EL3.M[3:0] = 0x9 = EL2h）
- ❌ **或者路由到TFTF**（之前提到的问题）
- ❓ **可能没有在EL3终止处理**

## 问题分析

### 问题1：SError路由到EL2而不是EL3

**可能原因**：
1. **HCR_EL2.AMO = 1**：SError被路由到EL2（最高优先级）
2. **EL2转发到EL3**：但SPSR_EL3显示的是EL2的状态

### 问题2：如果路由到EL2，是否符合预期？

**分析**：
- 注释说"terminal in EL3"
- 如果路由到EL2，EL2应该转发到EL3
- 最终应该在EL3终止处理

### 问题3：如果路由到TFTF，是否符合预期？

**分析**：
- ❌ **不符合预期**
- TFTF无法处理Uncontainable错误
- 应该在EL3处理

## 验证方法

### 1. 检查EL3是否真的处理了错误

在EL3的异常处理函数中添加打印：

```c
void check_uncontainable_handling(void)
{
    uint64_t esr_el3 = read_esr_el3();
    uint64_t ec = (esr_el3 >> 26) & 0x3f;
    
    INFO("ESR_EL3.EC = 0x%02llx\n", ec);
    
    // 检查是否是Uncontainable错误
    // 如果EC=0x2F（来自低异常等级的SError），说明路由正确
    
    // 检查错误处理是否终止
    // 如果函数返回，说明没有终止
    // 如果系统panic，说明正确终止
}
```

### 2. 检查错误是否在EL3终止

```c
// 在EL3的plat_ea_handler中
int plat_ea_handler(unsigned int ea_reason, uint64_t syndrome, void *cookie,
                    void *handle, uint64_t flags)
{
    // 检查是否是Uncontainable错误
    if (is_uncontainable_error(syndrome)) {
        ERROR("Uncontainable error detected - system should panic\n");
        // 应该调用panic或重启
        panic();  // 或 platform_panic()
        // 不应该返回
    }
}
```

### 3. 检查路由配置

```c
// 在EL3异常处理函数中
void check_routing_for_uncontainable(void)
{
    uint64_t scr_el3 = read_scr_el3();
    uint64_t hcr_el2 = read_hcr_el2();
    uint64_t spsr_el3 = read_spsr_el3();
    
    INFO("路由配置检查:\n");
    INFO("  SCR_EL3.EA = %d\n", (scr_el3 >> 3) & 1);
    INFO("  HCR_EL2.AMO = %d\n", (hcr_el2 >> 5) & 1);
    INFO("  SPSR_EL3.M[3:0] = 0x%llx\n", spsr_el3 & 0xF);
    
    if ((spsr_el3 & 0xF) == 0x9) {
        INFO("  异常经过EL2，最终应该到达EL3\n");
        INFO("  如果EL3正确处理，应该panic或重启\n");
    }
}
```

## 总结

### 预期行为

**test_uncontainable的预期行为**：
1. ✅ **SError应该路由到EL3**（terminal in EL3）
2. ✅ **EL3处理Uncontainable错误**
3. ✅ **系统应该panic或重启**（不返回到TFTF）

### 实际观察

**你的观察**：
1. ❓ **SPSR_EL3显示EL2h**：说明异常经过EL2
2. ❓ **可能路由到TFTF**：不符合预期

### 可能的情况

**情况1：路由正确，但显示EL2h**
- SError路由到EL2
- EL2转发到EL3
- EL3处理并终止
- **这是正常的**，只要最终在EL3终止

**情况2：路由错误**
- SError路由到TFTF（EL1/EL2）
- TFTF无法处理
- **不符合预期**

### 需要确认

1. **EL3是否真的处理了错误？**
   - 检查EL3异常处理函数的日志
   - 检查系统是否panic或重启

2. **如果路由到EL2，EL2是否转发到EL3？**
   - 检查EL2的异常处理逻辑
   - 确认最终是否到达EL3

3. **错误是否在EL3终止？**
   - 检查plat_ea_handler是否被调用
   - 检查系统是否panic

