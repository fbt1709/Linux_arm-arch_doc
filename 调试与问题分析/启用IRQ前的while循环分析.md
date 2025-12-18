# 为什么在enable_irq()前加入死循环会导致TSP未被调用？

## 问题现象

在`enable_irq()`之前加入`while`死循环，发现：
- `tftf_smc(tsp_svc_params)`已经被调用了
- 但是TSP都没有被调用（没有看到TSP的INFO打印）

## 可能的原因

### 原因1：TSP在ERET后立即被抢占

**执行流程**：

```
1. tftf_smc()调用SMC指令
   ↓
2. CPU切换到EL3，TSPD接收
   ↓
3. TSPD设置上下文，ERET到TSP (651行)
   ↓
4. CPU切换到S-EL1，TSP开始执行tsp_yield_smc_entry
   ↓
5. TSP使能中断 (478行)
   msr daifclr, #DAIF_FIQ_BIT | DAIF_IRQ_BIT
   ↓
6. TSP立即检测到pending的NS中断
   ↓
7. NS中断路由到EL3（如果TSP_NS_INTR_ASYNC_PREEMPT启用）
   ↓
8. EL3的tspd_ns_interrupt_handler被调用
   ↓
9. TSP被抢占，返回TSP_SMC_PREEMPTED
   ↓
10. 返回到Non-Secure世界
    ↓
11. tftf_smc()返回TSP_SMC_PREEMPTED
    ↓
12. 但是，如果Non-Secure世界的中断还是禁用的（disable_irq()）
    可能无法处理某些事情？
```

**关键点**：TSP可能在执行`tsp_smc_handler`之前就被抢占了！

### 原因2：TSP_NS_INTR_ASYNC_PREEMPT未启用

如果`TSP_NS_INTR_ASYNC_PREEMPT`未启用：
- NS中断不会路由到EL3
- TSP可能无法检测到pending的NS中断
- TSP可能正常执行，但无法被抢占

### 原因3：中断路由时机问题

**关键代码**（626-632行）：

```c
#if TSP_NS_INTR_ASYNC_PREEMPT
    /*
     * Enable the routing of NS interrupts to EL3
     * during processing of a Yielding SMC Call on
     * this core.
     */
    enable_intr_rm_local(INTR_TYPE_NS, SECURE);
#endif
```

这个代码在**TSPD切换到TSP之前**执行。如果：
- `TSP_NS_INTR_ASYNC_PREEMPT`未启用
- 或者中断路由配置有问题
- NS中断可能无法路由到EL3

### 原因4：TSP在ERET时立即检测到中断

**可能的情况**：

```
1. TSPD执行ERET到TSP (651行)
   ↓
2. CPU切换到S-EL1
   ↓
3. TSP开始执行tsp_yield_smc_entry
   ↓
4. TSP执行daifclr使能中断 (478行)
   ↓
5. 立即检测到pending的NS中断
   ↓
6. 但是，如果EL3_EXCEPTION_HANDLING启用
    需要ehf_allow_ns_preemption允许抢占
   ↓
7. 如果抢占被允许，NS中断路由到EL3
   ↓
8. TSP被抢占，返回TSP_SMC_PREEMPTED
   ↓
9. 但是，TSP可能还没有执行到tsp_smc_handler
    所以没有INFO打印
```

## 调试建议

### 1. 检查TSP是否真的被调用

在TSP的入口点添加打印：

```c
// bl32/tsp/aarch64/tsp_entrypoint.S:477
func tsp_yield_smc_entry
    // 添加打印
    INFO("TSP: tsp_yield_smc_entry called!\n");
    
    msr daifclr, #DAIF_FIQ_BIT | DAIF_IRQ_BIT
    bl  tsp_smc_handler
    // ...
```

### 2. 检查TSP是否被立即抢占

在TSPD的抢占处理中添加打印：

```c
// services/spd/tspd/tspd_main.c:224
static uint64_t tspd_ns_interrupt_handler(...)
{
    INFO("TSPD: NS interrupt handler called, TSP preempted!\n");
    // ...
}
```

### 3. 检查中断路由配置

```c
// 检查TSP_NS_INTR_ASYNC_PREEMPT是否启用
#if TSP_NS_INTR_ASYNC_PREEMPT
    INFO("TSPD: TSP_NS_INTR_ASYNC_PREEMPT enabled\n");
#else
    WARN("TSPD: TSP_NS_INTR_ASYNC_PREEMPT NOT enabled!\n");
#endif
```

### 4. 检查EL3_EXCEPTION_HANDLING配置

```c
#if EL3_EXCEPTION_HANDLING
    INFO("TSPD: EL3_EXCEPTION_HANDLING enabled\n");
    INFO("TSPD: ehf_allow_ns_preemption called\n");
#else
    WARN("TSPD: EL3_EXCEPTION_HANDLING NOT enabled!\n");
#endif
```

## 最可能的原因

### 场景：TSP被立即抢占，还没来得及执行tsp_smc_handler

```
1. TSPD ERET到TSP
   ↓
2. TSP执行tsp_yield_smc_entry
   ↓
3. TSP使能中断 (daifclr)
   ↓
4. 立即检测到pending的NS中断
   ↓
5. NS中断路由到EL3
   ↓
6. TSP被抢占（在bl tsp_smc_handler之前！）
   ↓
7. 返回到Non-Secure世界
   ↓
8. tftf_smc()返回TSP_SMC_PREEMPTED
   ↓
9. 但是TSP的tsp_smc_handler还没有执行
    所以没有INFO打印（224行）
```

**关键**：TSP可能在执行`bl tsp_smc_handler`之前就被抢占了！

## 验证方法

### 方法1：在TSP入口点添加打印

```assembly
func tsp_yield_smc_entry
    // 添加这个打印
    INFO("TSP: Entry point called!\n");
    
    msr daifclr, #DAIF_FIQ_BIT | DAIF_IRQ_BIT
    INFO("TSP: Interrupts enabled, about to call handler\n");
    bl  tsp_smc_handler
    INFO("TSP: Handler returned\n");
    // ...
```

### 方法2：检查抢占时机

在TSPD的抢占处理中添加打印：

```c
uint64_t tspd_handle_sp_preemption(void *handle)
{
    INFO("TSPD: TSP preempted! Check if TSP handler was called.\n");
    // ...
}
```

### 方法3：检查中断状态

在TSP入口点检查pending中断：

```c
// 在tsp_smc_handler开始处
smc_args_t *tsp_smc_handler(...)
{
    // 检查pending中断
    unsigned int pending = plat_ic_get_pending_interrupt_id();
    INFO("TSP: Handler called, pending interrupt: %u\n", pending);
    // ...
}
```

## 总结

**最可能的情况**：
1. TSP确实被调用了（ERET到S-EL1）
2. 但是TSP在使能中断后，**立即被pending的NS中断抢占**
3. 抢占发生在执行`bl tsp_smc_handler`之前
4. 所以没有看到TSP的INFO打印（224行）
5. TSP被抢占后，返回到Non-Secure世界
6. `tftf_smc()`返回`TSP_SMC_PREEMPTED`
7. 但是，如果Non-Secure世界还在死循环中，可能无法继续执行

**关键点**：TSP可能在执行handler之前就被抢占了，所以看不到TSP的打印！

