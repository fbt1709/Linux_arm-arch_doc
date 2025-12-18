# TSP RESUME 返回值分析

## 问题

在RESUME处理代码（722-774行）中，**没有直接修改ret0, ret1, ret2**。这些值应该由TSP在完成计算后返回。

## 返回值流程

### 1. RESUME处理流程

```c
case TSP_FID_RESUME:
    // 检查yield_smc_active_flag
    if (!get_yield_smc_active_flag(tsp_ctx->state))
        SMC_RET1(handle, SMC_UNK);  // 返回-1
    
    // 保存non-secure上下文
    cm_el1_sysregs_context_save(NON_SECURE);
    
    // 恢复secure上下文
    cm_el1_sysregs_context_restore(SECURE);
    cm_set_next_eret_context(SECURE);
    
    // 返回到TSP继续执行
    SMC_RET0(&tsp_ctx->cpu_ctx);  // 这里只是返回到TSP，不设置返回值
```

**关键点**：`SMC_RET0`只是返回到TSP，**不会设置ret0, ret1, ret2**。

### 2. TSP继续执行流程

TSP从`tsp_yield_smc_entry`继续执行：

```assembly
func tsp_yield_smc_entry
    msr daifclr, #DAIF_FIQ_BIT | DAIF_IRQ_BIT  // 使能中断
    bl  tsp_smc_handler                         // 调用处理函数
    msr daifset, #DAIF_FIQ_BIT | DAIF_IRQ_BIT  // 禁用中断
    restore_args_call_smc                       // 恢复参数并调用SMC返回
```

`tsp_smc_handler`返回`set_smc_args(func, 0, results[0], results[1], 0, 0, 0, 0)`。

### 3. TSPD处理TSP返回值

在TSPD的`tspd_smc_handler`中（652-681行）：

```c
} else {
    // 这是TSP返回的结果
    assert(handle == cm_get_context(SECURE));
    cm_el1_sysregs_context_save(SECURE);
    
    // 恢复non-secure上下文
    ns_cpu_context = cm_get_context(NON_SECURE);
    cm_el1_sysregs_context_restore(NON_SECURE);
    cm_set_next_eret_context(NON_SECURE);
    
    if (GET_SMC_TYPE(smc_fid) == SMC_TYPE_YIELD) {
        clr_yield_smc_active_flag(tsp_ctx->state);  // 清除flag
    }
    
    // 返回结果给non-secure世界
    SMC_RET3(ns_cpu_context, x1, x2, x3);  // 这里设置ret0=x1, ret1=x2, ret2=x3
}
```

**关键点**：TSPD通过`SMC_RET3`将TSP的返回值（x1, x2, x3）传递给non-secure世界。

## 问题分析

### 返回 -2, 4, 6 的含义

- **ret0 = -2**：这是`SMC_PREEMPTED`，说明TSP又返回了`TSP_PREEMPTED`而不是计算结果
- **ret1 = 4**：这是原始的`arg1`，说明TSP没有完成计算
- **ret2 = 6**：这是原始的`arg2`，说明TSP没有完成计算

## ADD计算执行位置

### 计算在 `tsp_smc_handler` 函数中执行

**文件**：`bl32/tsp/tsp_main.c`

**函数**：`tsp_smc_handler` (204-301行)

**计算流程**：

```c
smc_args_t *tsp_smc_handler(uint64_t func, uint64_t arg1, uint64_t arg2, ...)
{
    uint64_t results[2];
    
    // 1. 初始化结果（232-233行）
    results[0] = arg1;  // 初始值 = 4
    results[1] = arg2;  // 初始值 = 6
    
    // 2. 从TSPD获取magic值（239-241行）
    service_args = tsp_get_magic();  // 调用TSP_GET_ARGS SMC
    service_arg0 = (uint64_t)service_args;      // 通常是4
    service_arg1 = (uint64_t)(service_args >> 64U);  // 通常是6
    
    // 3. 执行ADD计算（253-255行）
    case TSP_ADD:
        results[0] += service_arg0;  // 4 + 4 = 8
        results[1] += service_arg1;  // 6 + 6 = 12
        break;
    
    // 4. 返回结果（297-300行）
    return set_smc_args(func, 0, results[0], results[1], 0, 0, 0, 0);
    // 返回: func, 0, 8, 12, 0, 0, 0, 0
}
```

### 关键步骤说明

1. **初始化**（232-233行）：
   - `results[0] = arg1` → 4
   - `results[1] = arg2` → 6

2. **获取magic值**（239行）：
   - 调用`tsp_get_magic()`，内部调用`TSP_GET_ARGS` SMC
   - TSPD返回之前保存的`arg1`和`arg2`（通过`get_tsp_args`）
   - 通常返回：`service_arg0 = 4, service_arg1 = 6`

3. **执行计算**（254-255行）：
   - `results[0] += service_arg0` → 4 + 4 = **8**
   - `results[1] += service_arg1` → 6 + 6 = **12**

4. **返回结果**（297-300行）：
   - 通过`set_smc_args`返回计算结果
   - TSPD通过`SMC_RET3(ns_cpu_context, x1, x2, x3)`传递给non-secure世界

### 如果返回 -2, 4, 6 说明什么？

- **ret0 = -2**：TSP返回了`TSP_PREEMPTED`，**没有执行到计算部分**
- **ret1 = 4**：原始`arg1`，说明`results[0] += service_arg0`**没有执行**
- **ret2 = 6**：原始`arg2`，说明`results[1] += service_arg1`**没有执行**

**结论**：TSP在`tsp_smc_handler`中，可能在以下位置被中断：
1. 在`tsp_get_magic()`调用时（239行）
2. 在计算执行前（253行之前）
3. 或者在计算执行后，返回前（297行之前）

最可能的情况是：**TSP在RESUME后，执行`tsp_smc_handler`时，又遇到了NS中断，导致再次被抢占，返回`TSP_PREEMPTED`**。

### 可能的原因

#### 原因1：TSP在RESUME后又被抢占

如果TSP在RESUME后执行`tsp_smc_handler`时，又遇到了NS中断，可能会：
1. 调用`tsp_common_int_handler`
2. 如果不是TSP能处理的中断（如NS SGI），会调用`tsp_handle_preemption()`
3. 返回`TSP_PREEMPTED`

#### 原因2：上下文恢复不正确

RESUME时，TSP的上下文可能没有正确恢复，导致：
1. TSP没有从被抢占的点继续执行
2. 或者执行状态不正确

#### 原因3：yield_smc_active_flag状态问题

虽然检查通过了，但可能：
1. TSP的上下文已经被破坏
2. 或者TSP认为它不应该继续执行

## 调试建议

### 1. 检查TSP是否又被抢占

在`tsp_smc_handler`中添加调试：

```c
smc_args_t *tsp_smc_handler(...)
{
    INFO("TSP: tsp_smc_handler called, func=0x%llx, arg1=%llu, arg2=%llu\n",
         func, arg1, arg2);
    
    // 检查是否有pending的中断
    unsigned int pending = plat_ic_get_pending_interrupt_id();
    if (pending != 0x3FF) {  // 0x3FF是spurious interrupt
        WARN("TSP: Pending interrupt %u detected!\n", pending);
    }
    
    // ... 原有代码 ...
}
```

### 2. 检查RESUME时的上下文

在RESUME处理中添加调试：

```c
case TSP_FID_RESUME:
    // ...
    INFO("TSP: RESUME - ELR_EL3=0x%llx, SPSR_EL3=0x%llx\n",
         read_elr_el3(SECURE), read_spsr_el3(SECURE));
    INFO("TSP: RESUME - saved_elr_el3=0x%llx, saved_spsr_el3=0x%llx\n",
         tsp_ctx->saved_elr_el3, tsp_ctx->saved_spsr_el3);
    
    // 检查是否需要恢复保存的上下文
    if (get_yield_smc_active_flag(tsp_ctx->state) && 
        tsp_ctx->saved_elr_el3 != 0) {
        INFO("TSP: Restoring saved context\n");
        // 可能需要恢复saved_elr_el3和saved_spsr_el3
    }
    // ...
```

### 3. 检查中断路由

确认NS中断在RESUME后是否仍然路由到EL3：

```c
case TSP_FID_RESUME:
    // ...
#if TSP_NS_INTR_ASYNC_PREEMPT
    // 检查中断路由状态
    INFO("TSP: RESUME - NS interrupt routing enabled\n");
    enable_intr_rm_local(INTR_TYPE_NS, SECURE);
#endif
    // ...
```

## 关键发现

**RESUME处理代码确实没有直接修改ret0, ret1, ret2**。这些值应该由：
1. TSP完成计算后返回
2. TSPD通过`SMC_RET3`传递给non-secure世界

如果返回`-2, 4, 6`，说明：
- TSP在RESUME后**没有完成计算**
- TSP可能又返回了`TSP_PREEMPTED`
- 或者TSP的上下文恢复有问题

## 下一步

1. **添加调试打印**，确认TSP在RESUME后是否被调用
2. **检查TSP的返回值**，看它返回的是什么
3. **检查中断状态**，看是否有pending的中断导致再次抢占

