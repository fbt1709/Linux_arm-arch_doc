# 为什么在90-96行后加while循环会导致TSP打印看不到？

## 问题现象

在90-96行（`tftf_smc()`调用和检查）后面加`while`循环，发现：
- `tftf_smc()`已经返回了（说明TSP被抢占）
- 但是TSP里面的打印（224行）看不到

## 根本原因

**TSP在使能中断后立即被中断异常打断，还没来得及执行`bl tsp_smc_handler`！**

## 详细分析

### 执行流程

```
1. tftf_smc()调用SMC指令 (90行)
   ↓
2. CPU切换到EL3，TSPD接收
   ↓
3. TSPD切换到TSP (ERET到S-EL1)
   ↓
4. TSP执行tsp_yield_smc_entry (477行)
   ↓
5. TSP使能中断 (478行)
   msr daifclr, #DAIF_FIQ_BIT | DAIF_IRQ_BIT
   ↓
6. 立即检测到pending的NS中断！
   ↓
7. CPU触发中断异常，跳转到TSP异常向量表
   irq_sp_elx 或 fiq_sp_elx (108/112行)
   ↓
8. 异常向量表调用handle_tsp_interrupt宏 (52-68行)
   ↓
9. handle_tsp_interrupt调用tsp_common_int_handler (57行)
   ↓
10. tsp_common_int_handler检测到NS中断 (86行)
    返回TSP_PREEMPTED (95行)
    ↓
11. handle_tsp_interrupt执行smc #0返回到EL3 (64行)
    ↓
12. TSPD处理抢占，返回SMC_PREEMPTED
    ↓
13. 返回到Non-Secure世界
    ↓
14. tftf_smc()返回TSP_SMC_PREEMPTED
    ↓
15. 检查返回值 (91行)
    ↓
16. 进入while循环（如果添加了）
```

### 关键点

**TSP在使能中断后，立即被中断异常打断，没有执行到`bl tsp_smc_handler`！**

```assembly
func tsp_yield_smc_entry
    msr daifclr, #DAIF_FIQ_BIT | DAIF_IRQ_BIT  // 使能中断
    // ↑ 这里使能中断后，立即检测到pending的NS中断
    // ↓ 中断异常触发，跳转到异常向量表
    // ↓ 不会执行下面的 bl tsp_smc_handler
    bl  tsp_smc_handler  // 这行代码没有执行！
    // ...
```

### TSP的异常向量表处理

```assembly
// aarch64/tsp_exceptions.S:108-114
vector_entry irq_sp_elx
    handle_tsp_interrupt irq_sp_elx  // 调用中断处理宏
end_vector_entry irq_sp_elx

vector_entry fiq_sp_elx
    handle_tsp_interrupt fiq_sp_elx  // 调用中断处理宏
end_vector_entry fiq_sp_elx
```

```assembly
// aarch64/tsp_exceptions.S:52-68
.macro handle_tsp_interrupt label
    msr daifclr, #DAIF_ABT_BIT
    save_caller_regs_and_lr
    bl  tsp_common_int_handler  // 调用中断处理函数
    cbz x0, interrupt_exit_\label
    
    // 如果不是TSP能处理的中断，返回EL3
    smc #0  // 返回到EL3，通知TSP被抢占
interrupt_exit_\label:
    restore_caller_regs_and_lr
    exception_return
.endm
```

## 为什么while循环会影响？

### 可能的原因1：打印缓冲问题

如果while循环阻塞了某些操作，可能导致：
- 打印缓冲区没有刷新
- 或者打印输出被延迟

### 可能的原因2：时序问题

如果while循环在检查返回值之后：
- 可能影响了后续的执行流程
- 或者阻塞了某些必要的操作

### 可能的原因3：TSP确实没有被调用到handler

**最可能的情况**：
- TSP被调用了（ERET到S-EL1）
- 但是使能中断后立即被中断异常打断
- 没有执行到`bl tsp_smc_handler`
- 所以看不到TSP的INFO打印（224行）

## 验证方法

### 方法1：在TSP入口点添加打印

```assembly
func tsp_yield_smc_entry
    // 添加这个打印
    INFO("TSP: Entry point called!\n");
    
    msr daifclr, #DAIF_FIQ_BIT | DAIF_IRQ_BIT
    INFO("TSP: Interrupts enabled\n");
    
    // 检查pending中断
    // 这里可能需要用C代码检查
    
    bl  tsp_smc_handler
    INFO("TSP: Handler returned\n");
    // ...
```

### 方法2：在中断处理中添加打印

```c
// bl32/tsp/tsp_interrupt.c:69
int32_t tsp_common_int_handler(void)
{
    INFO("TSP: Interrupt handler called!\n");
    
    id = plat_ic_get_pending_interrupt_id();
    INFO("TSP: Pending interrupt: %u\n", id);
    
    if (id != TSP_IRQ_SEC_PHY_TIMER) {
        INFO("TSP: Returning TSP_PREEMPTED\n");
        return tsp_handle_preemption();
    }
    // ...
}
```

### 方法3：在TSPD的抢占处理中添加打印

```c
// services/spd/tspd/tspd_main.c:224
static uint64_t tspd_ns_interrupt_handler(...)
{
    INFO("TSPD: NS interrupt handler called!\n");
    INFO("TSPD: TSP was preempted by NS interrupt\n");
    // ...
}
```

## 总结

### 执行流程

```
tftf_smc()调用
    ↓
TSPD切换到TSP
    ↓
TSP执行tsp_yield_smc_entry
    ↓
TSP使能中断 (daifclr)
    ↓
立即检测到pending的NS中断
    ↓
中断异常触发，跳转到异常向量表
    ↓
调用tsp_common_int_handler
    ↓
返回TSP_PREEMPTED
    ↓
执行smc #0返回到EL3
    ↓
TSPD处理抢占，返回SMC_PREEMPTED
    ↓
返回到Non-Secure世界
    ↓
tftf_smc()返回
    ↓
检查返回值
    ↓
进入while循环（如果添加了）
```

### 关键发现

1. **TSP确实被调用了**（ERET到S-EL1）
2. **但是使能中断后立即被中断异常打断**
3. **没有执行到`bl tsp_smc_handler`**
4. **所以看不到TSP的INFO打印（224行）**
5. **while循环可能只是影响了打印的显示，或者阻塞了某些操作**

### 验证

如果看到"Entry point called!"但没有看到"Interrupts enabled"或"Handler called"，说明TSP在使能中断后立即被抢占。

