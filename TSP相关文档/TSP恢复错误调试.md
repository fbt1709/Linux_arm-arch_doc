# TSP RESUME 返回 -2 错误分析

## 问题描述

测试 `tsp_int_and_resume` 中，RESUME SMC 返回：
- **实际返回**：`ret0 = -2, ret1 = 4, ret2 = 6`
- **期望返回**：`ret0 = 0, ret1 = 8, ret2 = 12`

## 错误码定义

```c
// include/lib/smccc.h
#define SMC_UNK         -1
#define SMC_PREEMPTED   -2  // 这是用户看到的返回值！
```

## 问题分析

### 1. 返回值 -2 的含义

`-2` 是 `SMC_PREEMPTED`，表示TSP仍然处于被抢占状态，而不是成功恢复执行。

### 2. 可能的原因

#### 原因1：`yield_smc_active_flag` 未正确设置

在 `tspd_handle_sp_preemption` 函数中（60-89行）：

```c
uint64_t tspd_handle_sp_preemption(void *handle)
{
    // ...
    // 保存secure上下文
    cm_el1_sysregs_context_save(SECURE);
    
    // 恢复non-secure上下文
    cm_el1_sysregs_context_restore(NON_SECURE);
    cm_set_next_eret_context(NON_SECURE);
    
    // 返回SMC_PREEMPTED
    SMC_RET1(ns_cpu_context, SMC_PREEMPTED);
}
```

**问题**：这个函数**没有检查或设置 `yield_smc_active_flag`**！

#### 原因2：RESUME 检查失败

在 RESUME 处理中（736行）：

```c
case TSP_FID_RESUME:
    // ...
    /* Check if we are already preempted before resume */
    if (!get_yield_smc_active_flag(tsp_ctx->state))
        SMC_RET1(handle, SMC_UNK);  // 应该返回-1，但用户看到的是-2
```

如果 `yield_smc_active_flag` 未设置，应该返回 `SMC_UNK (-1)`，但用户看到的是 `-2`。

#### 原因3：TSP_NS_INTR_ASYNC_PREEMPT 未启用

如果 `TSP_NS_INTR_ASYNC_PREEMPT` 未启用，NS中断处理流程可能不同。

## 调试步骤

### 1. 检查 TSP_NS_INTR_ASYNC_PREEMPT 配置

```bash
# 检查编译配置
grep -r "TSP_NS_INTR_ASYNC_PREEMPT" arm-trusted-firmware/
```

### 2. 检查 yield_smc_active_flag 状态

在以下位置添加调试打印：

**位置1：抢占处理时**
```c
// services/spd/tspd/tspd_main.c:60
uint64_t tspd_handle_sp_preemption(void *handle)
{
    tsp_context_t *tsp_ctx = ...;
    
    // 添加调试
    INFO("TSP: Preemption - yield_smc_active_flag = %d\n", 
         get_yield_smc_active_flag(tsp_ctx->state));
    
    // 如果flag未设置，可能需要设置它
    if (!get_yield_smc_active_flag(tsp_ctx->state)) {
        WARN("TSP: yield_smc_active_flag not set during preemption!\n");
        // 可能需要设置flag？
        // set_yield_smc_active_flag(tsp_ctx->state);
    }
    
    // ...
}
```

**位置2：RESUME 处理时**
```c
// services/spd/tspd/tspd_main.c:736
case TSP_FID_RESUME:
    // 添加调试
    INFO("TSP: RESUME - yield_smc_active_flag = %d\n",
         get_yield_smc_active_flag(tsp_ctx->state));
    
    if (!get_yield_smc_active_flag(tsp_ctx->state)) {
        ERROR("TSP: RESUME failed - yield_smc_active_flag not set!\n");
        SMC_RET1(handle, SMC_UNK);
    }
    // ...
```

### 3. 检查抢占流程

查看 NS 中断处理流程：

```c
// services/spd/tspd/tspd_main.c:224
static uint64_t tspd_ns_interrupt_handler(...)
{
    // 这个函数调用 tspd_handle_sp_preemption
    return tspd_handle_sp_preemption(handle);
}
```

**关键问题**：`tspd_handle_sp_preemption` 应该检查 `yield_smc_active_flag`，如果设置了，应该保持它；如果未设置，可能需要设置它（如果TSP正在执行yielding SMC）。

## 可能的修复方案

### 方案1：在抢占时设置 flag

```c
uint64_t tspd_handle_sp_preemption(void *handle)
{
    tsp_context_t *tsp_ctx = &tspd_sp_context[plat_my_core_pos()];
    
    // 检查是否正在执行yielding SMC
    if (!get_yield_smc_active_flag(tsp_ctx->state)) {
        // 如果TSP正在执行yielding SMC，应该设置flag
        // 但这需要知道TSP是否在执行yielding SMC
        // 可能需要从上下文判断
    }
    
    // 保存上下文
    cm_el1_sysregs_context_save(SECURE);
    
    // 恢复non-secure上下文
    // ...
    
    SMC_RET1(ns_cpu_context, SMC_PREEMPTED);
}
```

### 方案2：检查 TSP_NS_INTR_ASYNC_PREEMPT 配置

确保 `TSP_NS_INTR_ASYNC_PREEMPT` 已启用：

```makefile
# 在平台配置中
TSP_NS_INTR_ASYNC_PREEMPT := 1
```

### 方案3：检查抢占时的上下文保存

确保抢占时正确保存了TSP的上下文，以便RESUME时能够恢复。

## 验证步骤

1. **添加调试打印**，确认：
   - `yield_smc_active_flag` 在抢占时的状态
   - `yield_smc_active_flag` 在RESUME时的状态
   - TSP是否正在执行yielding SMC

2. **检查配置**：
   - `TSP_NS_INTR_ASYNC_PREEMPT` 是否启用
   - 中断路由配置是否正确

3. **检查时序**：
   - 抢占发生时，TSP是否正在执行yielding SMC
   - RESUME调用时，上下文是否完整

## 总结

返回 `-2 (SMC_PREEMPTED)` 而不是期望的结果，说明：
1. RESUME 可能没有正确恢复TSP的执行
2. 或者 `yield_smc_active_flag` 状态不正确
3. 或者 TSP 的上下文保存/恢复有问题

需要进一步调试来确定具体原因。

