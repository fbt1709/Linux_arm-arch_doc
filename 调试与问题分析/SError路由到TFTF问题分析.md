# SError路由到TFTF问题分析

## 问题描述

在BL31初始化退出时发现`SCR_EL3.EA = 1`，但`test_uncontainable`测试仍然将SError路由到了TFTF（非安全世界），而不是EL3。

## 根本原因分析

### 1. SCR_EL3.EA的设置时机

SCR_EL3寄存器在以下两个地方被设置：

#### 位置1：EL3初始化时（el3_common_macros.S）

```assembly
// include/arch/aarch64/el3_common_macros.S:490
mrs	x15, scr_el3
orr	x15, x15, #SCR_EA_BIT  // 无条件设置EA位
msr	scr_el3, x15
```

**说明**：这是在EL3初始化时设置的，此时EA=1。

#### 位置2：退出到非安全世界时（context_mgmt.c）

```c
// lib/el3_runtime/aarch64/context_mgmt.c:270-273
#if HANDLE_EA_EL3_FIRST_NS
	/* SCR_EL3.EA: Route External Abort and SError Interrupt to EL3. */
	scr_el3 |= SCR_EA_BIT;
#endif
```

**关键点**：只有在`HANDLE_EA_EL3_FIRST_NS=1`时，才会在非安全世界的上下文中设置SCR_EL3.EA。

### 2. 问题原因

**当`HANDLE_EA_EL3_FIRST_NS=0`或未定义时**：

1. BL31初始化时：`SCR_EL3.EA = 1`（在EL3执行时）
2. 退出到非安全世界时：`cm_setup_context()`根据配置设置SCR_EL3
   - 如果`HANDLE_EA_EL3_FIRST_NS=0`，**不会设置SCR_EL3.EA**
   - 退出到非安全世界后，`SCR_EL3.EA = 0`
3. 在非安全世界（TFTF）执行时：
   - `SCR_EL3.EA = 0` → SError路由到当前EL（非安全EL1）
   - 因此SError被路由到TFTF，而不是EL3

### 3. 配置检查

检查平台Makefile中是否定义了`HANDLE_EA_EL3_FIRST_NS`：

```makefile
# 如果未定义或=0，SError不会路由到EL3
HANDLE_EA_EL3_FIRST_NS := 0  # 或未定义

# 如果=1，SError会路由到EL3
HANDLE_EA_EL3_FIRST_NS := 1
```

## 解决方案

### 方案1：启用HANDLE_EA_EL3_FIRST_NS（推荐）

在平台Makefile中添加：

```makefile
HANDLE_EA_EL3_FIRST_NS := 1
```

这样在退出到非安全世界时，SCR_EL3.EA会被设置为1，SError会路由到EL3。

### 方案2：通过观察SError路由行为判断（推荐）

**注意**：在非安全EL1中**无法直接读取SCR_EL3**（EL3特权寄存器），需要通过以下方式：

#### 方法A：在EL3异常处理函数中添加打印

在EL3的`serror_aarch64`入口处添加检测代码：

```c
// 在bl31/aarch64/runtime_exceptions.S的serror_aarch64入口处
vector_entry serror_aarch64
    /* 添加检测代码 */
    mrs     x0, scr_el3
    tst     x0, #SCR_EA_BIT
    b.eq    serror_not_routed_to_el3
    
    /* SError路由到EL3 */
    adr     x0, serror_routed_to_el3_msg
    bl      asm_print_str
    b       serror_check_done
    
serror_not_routed_to_el3:
    /* SError未路由到EL3 */
    adr     x0, serror_not_routed_msg
    bl      asm_print_str
    
serror_check_done:
    /* 继续正常处理流程 */
    ...
```

#### 方法B：通过SMC调用到EL3读取（需要实现SMC服务）

如果需要在TFTF中读取SCR_EL3，需要：
1. 在EL3实现一个SMC服务返回SCR_EL3的值
2. 在TFTF中通过SMC调用读取

```c
// EL3中实现SMC服务
uint64_t get_scr_el3_smc_handler(...) {
    return read_scr_el3();
}

// TFTF中调用
smc_result_t result = tftf_smc(GET_SCR_EL3_SMC_ID, 0, 0, 0, 0, 0, 0, 0);
uint64_t scr_el3 = result.ret0;
```

#### 方法C：通过观察实际路由行为

最简单的方法：在EL3异常处理函数中添加打印，观察SError是否被路由到EL3。

### 方案3：在EL3中添加检测代码

在EL3的异常处理函数中检测SCR_EL3.EA，确认路由配置：

```c
// 在handle_lower_el_async_ea()或serror_aarch64入口处
void check_serror_routing(void)
{
    uint64_t scr_el3 = read_scr_el3();
    INFO("[EL3] SError路由: SCR_EL3.EA = %d\n", (scr_el3 & SCR_EA_BIT) ? 1 : 0);
}
```

## 验证方法

### 1. 检查配置

```bash
# 在编译配置中检查
grep -r "HANDLE_EA_EL3_FIRST_NS" plat/
```

### 2. 运行时检测

在以下位置添加打印：

1. **BL31退出时**：在`cm_prepare_el3_exit()`后检查SCR_EL3（可以在EL3中直接读取）
2. **EL3异常处理时**：在`serror_aarch64`入口处检查SCR_EL3（**推荐方法**）
3. **TFTF执行时**：**无法直接读取SCR_EL3**，需要通过SMC或观察行为判断

### 3. 预期行为

- **HANDLE_EA_EL3_FIRST_NS=1**：
  - BL31退出时：SCR_EL3.EA = 1
  - TFTF执行时：SCR_EL3.EA = 1
  - SError路由到EL3 ✅

- **HANDLE_EA_EL3_FIRST_NS=0**：
  - BL31退出时：SCR_EL3.EA = 1（EL3执行时）
  - TFTF执行时：SCR_EL3.EA = 0（退出到NS时被清除）
  - SError路由到TFTF ❌

## 总结

问题的根本原因是：**BL31初始化退出时SCR_EL3.EA=1，但退出到非安全世界时，如果`HANDLE_EA_EL3_FIRST_NS=0`，SCR_EL3.EA会被清除，导致SError路由到非安全世界而不是EL3**。

**解决方法**：在平台Makefile中设置`HANDLE_EA_EL3_FIRST_NS := 1`。

