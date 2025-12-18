# Uncontainable错误路由机制 - 关键发现

## 问题总结

**配置**：
- HCR_EL2.AMO = 0（注入错误前确认）
- SCR_EL3.EA = 1（0x73D）
- **但SError路由到TFTF，而不是EL3**

## 最可能的原因

### ⭐⭐⭐⭐⭐ 原因1：Uncontainable错误的架构行为

**ARM架构可能规定Uncontainable错误总是路由到当前EL**：

根据ARM架构规范，**Uncontainable错误**（Uncontainable Error）的特点：
- **不可包含**：错误无法被系统包含和恢复
- **终端错误**：通常导致系统panic或重启
- **可能总是路由到当前EL**：不受SCR_EL3.EA控制

**这是ARM架构的特定行为**，因为：
- Uncontainable错误是"不可包含"的，应该在发生错误的EL处理
- 如果路由到EL3，可能无法正确识别错误发生的上下文
- 因此ARM架构可能规定Uncontainable错误总是路由到当前EL

### ⭐⭐⭐⭐ 原因2：错误注入时的执行上下文

**错误在EL1注入，可能在EL1的上下文中触发**：

```
TFTF在EL1执行
    ↓
inject_uncontainable_ras_error() 在EL1执行
    ↓
配置RAS错误注入寄存器（EL1上下文）
    ↓
错误触发（在EL1的上下文中）
    ↓
硬件检查路由配置
    ↓
即使SCR_EL3.EA=1，错误可能先被EL1处理
    ↓
EL1的异常处理函数执行（如果配置了VBAR_EL1）
```

**关键点**：
- 错误注入时的执行上下文是EL1
- 错误可能在EL1的上下文中触发
- EL1的异常向量表可能配置了SError处理

### ⭐⭐⭐ 原因3：EL1的异常向量表拦截

**即使路由配置指向EL3，EL1仍可能拦截**：

```c
// 检查EL1的异常向量表
uint64_t vbar_el1;
asm volatile("mrs %0, vbar_el1" : "=r" (vbar_el1));

// 如果VBAR_EL1设置了异常向量表
// EL1可能有SError处理程序
// EL1可能在硬件路由到EL3之前拦截异常
```

**可能机制**：
- TFTF在初始化时设置了VBAR_EL1
- EL1的异常向量表配置了SError处理
- EL1在硬件路由到EL3之前拦截异常
- 这是软件行为，不是硬件路由

## 验证方法

### 方法1：对比普通SError和Uncontainable错误

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

### 方法2：检查EL1的异常向量表

```c
void check_el1_exception_vectors(void)
{
    uint64_t vbar_el1;
    asm volatile("mrs %0, vbar_el1" : "=r" (vbar_el1));
    
    tftf_testcase_printf("VBAR_EL1 = 0x%016llx\n", vbar_el1);
    
    if (vbar_el1 != 0) {
        tftf_testcase_printf("EL1配置了异常向量表\n");
        tftf_testcase_printf("EL1可能有SError处理程序\n");
        tftf_testcase_printf("这可能解释了为什么SError路由到TFTF\n");
    }
}
```

### 方法3：检查错误注入时的执行上下文

```c
void check_injection_context(void)
{
    unsigned int el = get_current_el();
    
    tftf_testcase_printf("错误注入时的执行上下文:\n");
    tftf_testcase_printf("  CurrentEL = %d\n", el);
    
    if (el == 1) {
        tftf_testcase_printf("  错误在EL1注入\n");
        tftf_testcase_printf("  错误可能在EL1的上下文中触发\n");
        tftf_testcase_printf("  即使SCR_EL3.EA=1，错误可能先被EL1处理\n");
    }
}
```

## 解决方案

### 如果Uncontainable错误总是路由到当前EL

**这是ARM架构的预期行为**：
- Uncontainable错误应该在发生错误的EL处理
- 测试用例的预期可能需要调整
- 或者需要特殊的配置来路由到EL3

### 如果EL1拦截了异常

**解决方案**：
- 检查EL1的异常向量表配置
- 如果EL1有SError处理程序，可能需要禁用或修改
- 或者确保EL1的SError处理程序转发到EL3

## 总结

**最可能的原因**：

1. **⭐⭐⭐⭐⭐ Uncontainable错误的架构行为**
   - ARM架构可能规定Uncontainable错误总是路由到当前EL
   - 不受SCR_EL3.EA控制
   - 这是ARM架构的特定行为

2. **⭐⭐⭐⭐ 错误注入时的执行上下文**
   - 错误在EL1注入，可能在EL1的上下文中触发
   - EL1的异常向量表可能拦截异常

3. **⭐⭐⭐ EL1的异常向量表拦截**
   - TFTF可能配置了EL1的异常向量表
   - EL1在硬件路由到EL3之前拦截异常

**需要进一步调查**：
- ARM架构规范中Uncontainable错误的路由规则
- EL1的异常向量表配置（VBAR_EL1）
- 错误注入时的实际执行上下文

