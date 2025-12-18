# TSP抢占机制分析：TSP_NS_INTR_ASYNC_PREEMPT=0 的情况

## 问题

**此时SGI是pending的，那么最终还是进入了TSP，那是不是也没有抢占阿？**

## 答案

**抢占确实会发生，但是发生在TSP内部（S-EL1），而不是在EL3。**

## 关键理解

### 配置情况

- **`TSP_NS_INTR_ASYNC_PREEMPT=0`**：NS中断使用默认路由模型（`CSS=0, TEL3=0`），路由到FEL（S-EL1/TSP）
- **`EL3_EXCEPTION_HANDLING=1`**：使用EHF框架

### 路由模型

**默认路由模型**（`TSP_NS_INTR_ASYNC_PREEMPT=0`）：
- **Secure状态时**：`CSS=0, TEL3=0` → 路由到**FEL（S-EL1/TSP）**
- **Non-Secure状态时**：`CSS=1, TEL3=0` → 路由到**FEL（NS-EL1）**

## 完整流程

### 阶段1：TSPD返回到TSP

```c
// services/spd/tspd/tspd_main.c:651
SMC_RET3(&tsp_ctx->cpu_ctx, smc_fid, x1, x2);
```

**执行的操作**：
- ✅ 设置返回值到TSP上下文
- ✅ 通过`el3_exit`和`ERET`返回到TSP（S-EL1）
- ✅ 返回到`tsp_yield_smc_entry`

**状态**：
- ✅ TSP在S-EL1执行
- ✅ NS SGI处于pending状态（在GIC中）
- ✅ 中断还未enable（PSTATE.I=1）

### 阶段2：TSP Enable中断

```assembly
// bl32/tsp/aarch64/tsp_entrypoint.S:477-479
func tsp_yield_smc_entry
    msr	daifclr, #DAIF_FIQ_BIT | DAIF_IRQ_BIT  // ← 立即enable中断
    bl	tsp_smc_handler
```

**关键**：
- ✅ **TSP立即enable中断**：`msr daifclr`清除DAIF位，enable中断
- ✅ **Pending SGI立即触发**：一旦中断被enable，pending的NS SGI立即触发中断异常
- ✅ **中断路由到S-EL1**：由于路由配置（`CSS=0, TEL3=0`），中断路由到FEL（S-EL1），即TSP的异常向量表

### 阶段3：中断异常触发到TSP

```
NS SGI pending
  ↓
硬件检测到pending中断（中断已enable）
  ↓
检查中断路由配置（CSS=0, TEL3=0）
  ↓
中断路由到FEL（S-EL1/TSP）← 关键：路由到TSP，不是EL3
  ↓
中断异常触发，跳转到TSP的异常向量表
```

**关键**：
- ✅ **中断路由到TSP**：由于`TSP_NS_INTR_ASYNC_PREEMPT=0`，NS中断路由到FEL（S-EL1）
- ✅ **TSP的异常向量表被调用**：`irq_sp_elx`或`fiq_sp_elx`被触发
- ✅ **TSP被抢占**：TSP的执行被中断异常打断

### 阶段4：TSP检测并处理NS中断

```assembly
// bl32/tsp/aarch64/tsp_exceptions.S:52-68
.macro	handle_tsp_interrupt label
    msr	daifclr, #DAIF_ABT_BIT
    save_caller_regs_and_lr
    bl	tsp_common_int_handler  // ← 调用C handler
    cbz	x0, interrupt_exit_\label
    smc	#0  // ← 如果不是Secure中断，返回到EL3
interrupt_exit_\label:
    restore_caller_regs_and_lr
    exception_return
.endm
```

```c
// bl32/tsp/tsp_interrupt.c:69-96
int32_t tsp_common_int_handler(void)
{
    id = plat_ic_get_pending_interrupt_id();
    
    /* TSP can only handle the secure physical timer interrupt */
    if (id != TSP_IRQ_SEC_PHY_TIMER) {
        return tsp_handle_preemption();  // ← 返回TSP_PREEMPTED
    }
    // ... 处理Secure Timer中断 ...
    return 0;
}
```

**关键**：
- ✅ **TSP检测到NS中断**：`tsp_common_int_handler`检测到这不是Secure中断（不是Secure Timer）
- ✅ **返回TSP_PREEMPTED**：`tsp_handle_preemption()`返回`TSP_PREEMPTED`
- ✅ **执行SMC返回到EL3**：TSP执行`smc #0`，SMC ID是`TSP_PREEMPTED`

### 阶段5：TSPD处理TSP_PREEMPTED SMC

```c
// services/spd/tspd/tspd_main.c:368-372
case TSP_PREEMPTED:
    if (ns)
        SMC_RET1(handle, SMC_UNK);
    
    return tspd_handle_sp_preemption(handle);  // ← 处理抢占
```

**关键**：
- ✅ **TSPD接收TSP_PREEMPTED SMC**：TSP通过SMC返回到EL3
- ✅ **调用抢占处理函数**：`tspd_handle_sp_preemption`处理抢占

### 阶段6：TSPD返回到Non-Secure世界

```c
// services/spd/tspd/tspd_main.c:60-89
uint64_t tspd_handle_sp_preemption(void *handle)
{
    cm_el1_sysregs_context_save(SECURE);        // ← 保存TSP状态
    cm_el1_sysregs_context_restore(NON_SECURE); // ← 恢复Non-Secure上下文
    cm_set_next_eret_context(NON_SECURE);       // ← 设置返回目标
    SMC_RET1(ns_cpu_context, SMC_PREEMPTED);   // ← 返回到TFTF
}
```

**关键**：
- ✅ **保存TSP状态**：保存TSP被抢占时的状态
- ✅ **恢复Non-Secure上下文**：恢复TFTF的上下文
- ✅ **返回到TFTF**：通过`SMC_RET1`返回到Non-Secure世界，返回值是`SMC_PREEMPTED`

## 完整时序图

```
TSP (S-EL1)                    EL3 (TSPD)                    TFTF (Non-Secure)
─────────────────              ───────────                    ──────────────────
                                │
                                │ SMC_RET3(&tsp_ctx->cpu_ctx, ...)
                                │
                                │ ERET
                                ├──────────────────────────>│
                                │                            │
                                │                            │ tsp_yield_smc_entry
                                │                            │
                                │                            │ msr daifclr (enable中断)
                                │                            │
                                │                            │ ← Pending NS SGI触发！
                                │                            │
                                │                            │ 中断异常（路由到S-EL1）
                                │                            │
                                │                            │ TSP异常向量表
                                │                            │
                                │                            │ tsp_common_int_handler
                                │                            │   - 检测到NS中断
                                │                            │   - 返回TSP_PREEMPTED
                                │                            │
                                │                            │ smc #0 (TSP_PREEMPTED)
                                │                            │
                                │<───────────────────────────┤
                                │                            │
                                │ tspd_smc_handler
                                │   case TSP_PREEMPTED:
                                │     tspd_handle_sp_preemption()
                                │       - 保存TSP状态
                                │       - 恢复Non-Secure上下文
                                │       - SMC_RET1(ns_cpu_context, SMC_PREEMPTED)
                                │
                                │ ERET
                                ├────────────────────────────────────────────>│
                                │                                            │
                                │                                            │ tftf_smc() 返回
                                │                                            │ ret0 = SMC_PREEMPTED
                                │                                            │
                                │                                            │ enable_irq()
                                │                                            │
                                │                                            │ 处理SGI中断
```

## 关键理解

### 1. 抢占确实发生

**抢占发生在TSP内部（S-EL1）**：
- ✅ TSP在S-EL1执行时被中断异常打断
- ✅ 中断异常路由到TSP的异常向量表
- ✅ TSP检测到NS中断，通过SMC返回到EL3
- ✅ EL3处理抢占，返回到Non-Secure世界

### 2. 抢占时机

**抢占发生在TSP enable中断的瞬间**：
- ✅ TSP在`tsp_yield_smc_entry`中enable中断
- ✅ 一旦中断被enable，pending的NS SGI立即触发中断异常
- ✅ 中断异常路由到TSP（S-EL1），TSP被立即抢占

### 3. 与TSP_NS_INTR_ASYNC_PREEMPT=1的区别

**TSP_NS_INTR_ASYNC_PREEMPT=1**：
- NS中断路由到EL3
- 抢占发生在EL3，不进入TSP
- 直接从EL3返回到Non-Secure世界

**TSP_NS_INTR_ASYNC_PREEMPT=0**：
- NS中断路由到FEL（S-EL1/TSP）
- 抢占发生在TSP内部（S-EL1）
- TSP检测到NS中断，通过SMC返回到EL3
- EL3处理抢占，返回到Non-Secure世界

### 4. 为什么还是能抢占？

**即使进入了TSP，抢占仍然会发生**：
- ✅ TSP enable中断后，pending的NS SGI立即触发中断异常
- ✅ 中断异常路由到TSP的异常向量表
- ✅ TSP检测到这不是Secure中断，立即通过SMC返回到EL3
- ✅ EL3处理抢占，返回到Non-Secure世界

**关键**：TSP不会处理NS中断，它会立即检测并返回到EL3。

## 代码位置总结

### 1. TSP Enable中断

**位置**：`bl32/tsp/aarch64/tsp_entrypoint.S:478`
```assembly
msr	daifclr, #DAIF_FIQ_BIT | DAIF_IRQ_BIT
```

### 2. TSP异常向量表

**位置**：`bl32/tsp/aarch64/tsp_exceptions.S:108-110`
```assembly
vector_entry irq_sp_elx
    handle_tsp_interrupt irq_sp_elx
end_vector_entry irq_sp_elx
```

### 3. TSP中断处理

**位置**：`bl32/tsp/tsp_interrupt.c:69-96`
```c
int32_t tsp_common_int_handler(void)
{
    if (id != TSP_IRQ_SEC_PHY_TIMER) {
        return tsp_handle_preemption();  // 返回TSP_PREEMPTED
    }
}
```

### 4. TSP返回到EL3

**位置**：`bl32/tsp/aarch64/tsp_exceptions.S:64`
```assembly
smc	#0  // SMC ID是TSP_PREEMPTED
```

### 5. TSPD处理抢占

**位置**：`services/spd/tspd/tspd_main.c:368-372`
```c
case TSP_PREEMPTED:
    return tspd_handle_sp_preemption(handle);
```

## 总结

### 关键点

1. **抢占确实发生**：即使进入了TSP，抢占仍然会发生
2. **抢占发生在TSP内部**：中断异常路由到TSP（S-EL1），TSP被抢占
3. **TSP立即检测并返回**：TSP检测到NS中断，立即通过SMC返回到EL3
4. **EL3处理抢占**：EL3处理抢占，返回到Non-Secure世界

### 流程总结

```
1. TSPD通过SMC_RET3返回到TSP（S-EL1）
2. TSP在tsp_yield_smc_entry中enable中断
3. Pending的NS SGI立即触发中断异常
4. 由于路由配置（CSS=0, TEL3=0），中断路由到TSP（S-EL1）
5. TSP的异常向量表被调用
6. TSP的tsp_common_int_handler检测到NS中断，返回TSP_PREEMPTED
7. TSP执行smc #0返回到EL3
8. EL3（TSPD）处理TSP_PREEMPTED SMC，调用tspd_handle_sp_preemption
9. tspd_handle_sp_preemption返回到Non-Secure世界，返回值是SMC_PREEMPTED
```

### 答案

**抢占确实会发生，但是发生在TSP内部（S-EL1）。** TSP enable中断后，pending的NS SGI立即触发中断异常，路由到TSP的异常向量表。TSP检测到这不是Secure中断，立即通过SMC返回到EL3。EL3处理抢占，返回到Non-Secure世界。

**所以，即使进入了TSP，抢占仍然会发生，只是抢占发生在TSP内部，而不是在EL3。**

