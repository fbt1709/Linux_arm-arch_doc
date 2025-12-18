# TSP中断处理后的Pending状态分析

## 问题

**既然TSP enable了中断，并且进入了中断处理函数（tsp_common_int_handler），那么当返回到TFTF后，还能进入TFTF的中断处理函数吗？因为pending状态可能已经被清除了。**

## 答案

**Pending状态还在，TFTF仍然可以进入中断处理函数。**

**原因**：
- ✅ **TSP没有acknowledge中断**：`tsp_common_int_handler`只是读取pending中断ID，没有调用`plat_ic_acknowledge_interrupt()`
- ✅ **Pending状态保留在GIC中**：只有当明确acknowledge中断时，pending状态才会被清除
- ✅ **TFTF enable中断后，pending的SGI会再次触发**：因为pending状态还在，所以会再次触发中断异常

## 关键代码分析

### TSP的中断处理函数

```c
// bl32/tsp/tsp_interrupt.c:69-96
int32_t tsp_common_int_handler(void)
{
    uint32_t linear_id = plat_my_core_pos(), id;

    /*
     * Get the highest priority pending interrupt id and see if it is the
     * secure physical generic timer interrupt in which case, handle it.
     * Otherwise throw this interrupt at the EL3 firmware.
     */
    id = plat_ic_get_pending_interrupt_id();  // ← 只是读取pending中断ID

    /* TSP can only handle the secure physical timer interrupt */
    if (id != TSP_IRQ_SEC_PHY_TIMER) {
        return tsp_handle_preemption();  // ← 返回TSP_PREEMPTED，没有acknowledge中断
    }

    /*
     * Acknowledge and handle the secure timer interrupt.
     */
    id = plat_ic_acknowledge_interrupt();  // ← 只有处理Secure Timer时才acknowledge
    assert(id == TSP_IRQ_SEC_PHY_TIMER);
    tsp_generic_timer_handler();
    plat_ic_end_of_interrupt(id);
    return 0;
}
```

**关键**：
- ✅ **只是读取pending中断ID**：`plat_ic_get_pending_interrupt_id()`只是读取，不会清除pending状态
- ✅ **没有acknowledge NS中断**：如果`id != TSP_IRQ_SEC_PHY_TIMER`，直接返回`TSP_PREEMPTED`，**没有调用`plat_ic_acknowledge_interrupt()`**
- ✅ **只有处理Secure Timer时才acknowledge**：只有处理Secure Timer中断时，才会调用`plat_ic_acknowledge_interrupt()`和`plat_ic_end_of_interrupt()`

### TSP返回到EL3

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

**关键**：
- ✅ **TSP执行`smc #0`返回到EL3**：如果`tsp_common_int_handler`返回非0（`TSP_PREEMPTED`），执行`smc #0`
- ✅ **没有acknowledge中断**：TSP只是检测并返回，没有acknowledge中断
- ✅ **Pending状态保留**：因为中断没有被acknowledge，pending状态保留在GIC中

## GIC中断状态机制

### 中断状态转换

```
1. 中断触发
   ↓
2. GIC将中断标记为Pending
   ↓
3. 如果中断被enable，中断异常触发
   ↓
4. 读取pending中断ID（plat_ic_get_pending_interrupt_id）
   - Pending状态仍然保留
   ↓
5. Acknowledge中断（plat_ic_acknowledge_interrupt）
   - Pending状态清除
   - Active状态设置
   ↓
6. 处理中断
   ↓
7. End of Interrupt（plat_ic_end_of_interrupt）
   - Active状态清除
```

### TSP的处理流程

```
1. NS SGI触发中断异常
   ↓
2. TSP的异常向量表被调用
   ↓
3. tsp_common_int_handler被调用
   ↓
4. plat_ic_get_pending_interrupt_id()读取pending中断ID
   - Pending状态仍然保留 ← 关键
   ↓
5. 检测到NS中断（id != TSP_IRQ_SEC_PHY_TIMER）
   ↓
6. 返回TSP_PREEMPTED
   - 没有acknowledge中断 ← 关键
   ↓
7. 执行smc #0返回到EL3
   - Pending状态仍然保留 ← 关键
```

## 完整流程分析

### 阶段1：TSP检测NS中断

```
NS SGI pending（在GIC中）
  ↓
TSP enable中断
  ↓
中断异常触发，路由到TSP
  ↓
tsp_common_int_handler被调用
  ↓
plat_ic_get_pending_interrupt_id()读取pending中断ID
  ↓
检测到NS中断（id != TSP_IRQ_SEC_PHY_TIMER）
  ↓
返回TSP_PREEMPTED
  - 没有acknowledge中断 ← 关键
  ↓
执行smc #0返回到EL3
```

**状态**：
- ✅ **Pending状态保留**：因为中断没有被acknowledge，pending状态保留在GIC中

### 阶段2：EL3返回到TFTF

```
EL3（TSPD）处理TSP_PREEMPTED SMC
  ↓
tspd_handle_sp_preemption被调用
  ↓
保存TSP状态，恢复Non-Secure上下文
  ↓
SMC_RET1返回到Non-Secure世界
  ↓
TFTF接收SMC_PREEMPTED返回值
```

**状态**：
- ✅ **Pending状态仍然保留**：中断没有被acknowledge，pending状态仍然在GIC中

### 阶段3：TFTF Enable中断并处理

```c
// test_normal_int_switch.c:99
enable_irq();  // ← 在Non-Secure世界enable中断
```

**关键**：
- ✅ **Pending状态还在**：因为TSP没有acknowledge中断，pending状态还在GIC中
- ✅ **中断再次触发**：当TFTF enable中断后，pending的NS SGI会再次触发中断异常
- ✅ **TFTF的中断处理函数被调用**：TFTF的`sgi_handler`会被调用

## 时序图

```
TSP (S-EL1)                    EL3 (TSPD)                    TFTF (Non-Secure)
─────────────────              ───────────                    ──────────────────
                                │
                                │ SMC_RET3返回到TSP
                                ├──────────────────────────>│
                                │                            │
                                │                            │ tsp_yield_smc_entry
                                │                            │ msr daifclr (enable中断)
                                │                            │
                                │                            │ ← NS SGI触发中断异常
                                │                            │
                                │                            │ TSP异常向量表
                                │                            │
                                │                            │ tsp_common_int_handler
                                │                            │   - plat_ic_get_pending_interrupt_id()
                                │                            │   - 检测到NS中断
                                │                            │   - 返回TSP_PREEMPTED
                                │                            │   - 没有acknowledge中断 ← 关键
                                │                            │
                                │                            │ smc #0 (TSP_PREEMPTED)
                                │                            │
                                │<───────────────────────────┤
                                │                            │
                                │ tspd_handle_sp_preemption()
                                │   - 保存TSP状态
                                │   - 恢复Non-Secure上下文
                                │   - SMC_RET1(ns_cpu_context, SMC_PREEMPTED)
                                │
                                │ ERET
                                ├────────────────────────────────────────────>│
                                │                                            │
                                │                                            │ tftf_smc() 返回
                                │                                            │ ret0 = SMC_PREEMPTED
                                │                                            │
                                │                                            │ enable_irq()
                                │                                            │
                                │                                            │ ← NS SGI再次触发！
                                │                                            │ （因为pending状态还在）
                                │                                            │
                                │                                            │ TFTF中断处理函数
                                │                                            │ （sgi_handler）
                                │                                            │
                                │                                            │ 处理NS SGI中断
                                │                                            │ ← 最终在这里处理
```

## 关键理解

### 1. TSP没有acknowledge中断

**关键代码**：
```c
// bl32/tsp/tsp_interrupt.c:83-96
id = plat_ic_get_pending_interrupt_id();  // 只是读取，不清除pending

if (id != TSP_IRQ_SEC_PHY_TIMER) {
    return tsp_handle_preemption();  // 返回，没有acknowledge
}
```

**关键**：
- ✅ **只是读取pending中断ID**：`plat_ic_get_pending_interrupt_id()`只是读取，不会清除pending状态
- ✅ **没有acknowledge NS中断**：如果检测到NS中断，直接返回，**没有调用`plat_ic_acknowledge_interrupt()`**
- ✅ **Pending状态保留**：因为中断没有被acknowledge，pending状态保留在GIC中

### 2. Pending状态保留在GIC中

**GIC中断状态**：
- **Pending状态**：中断在GIC中处于pending状态
- **Active状态**：中断被acknowledge后，进入active状态
- **只有acknowledge才会清除pending**：只有调用`plat_ic_acknowledge_interrupt()`才会清除pending状态

**TSP的处理**：
- ✅ **没有acknowledge NS中断**：TSP只是检测并返回，没有acknowledge中断
- ✅ **Pending状态保留**：因为中断没有被acknowledge，pending状态保留在GIC中

### 3. TFTF仍然可以处理中断

**原因**：
- ✅ **Pending状态还在**：因为TSP没有acknowledge中断，pending状态还在GIC中
- ✅ **中断再次触发**：当TFTF enable中断后，pending的NS SGI会再次触发中断异常
- ✅ **TFTF的中断处理函数被调用**：TFTF的`sgi_handler`会被调用，处理NS SGI中断

## 代码位置总结

### 1. TSP读取pending中断ID

**位置**：`bl32/tsp/tsp_interrupt.c:83`
```c
id = plat_ic_get_pending_interrupt_id();  // 只是读取，不清除pending
```

### 2. TSP返回TSP_PREEMPTED

**位置**：`bl32/tsp/tsp_interrupt.c:95`
```c
return tsp_handle_preemption();  // 返回，没有acknowledge中断
```

### 3. TSP返回到EL3

**位置**：`bl32/tsp/aarch64/tsp_exceptions.S:64`
```assembly
smc	#0  // 返回到EL3，pending状态保留
```

### 4. TFTF Enable中断

**位置**：`test_normal_int_switch.c:99`
```c
enable_irq();  // 在Non-Secure世界enable中断，pending的SGI会再次触发
```

## 总结

### 关键点

1. **TSP没有acknowledge中断**：`tsp_common_int_handler`只是读取pending中断ID，没有调用`plat_ic_acknowledge_interrupt()`
2. **Pending状态保留**：因为中断没有被acknowledge，pending状态保留在GIC中
3. **TFTF仍然可以处理中断**：当TFTF enable中断后，pending的NS SGI会再次触发中断异常，TFTF的中断处理函数会被调用

### 流程总结

```
1. TSP enable中断，pending的NS SGI触发中断异常
2. TSP的tsp_common_int_handler检测到NS中断
3. TSP返回TSP_PREEMPTED，没有acknowledge中断 ← 关键
4. TSP执行smc #0返回到EL3
5. EL3返回到TFTF，pending状态仍然保留 ← 关键
6. TFTF enable中断，pending的NS SGI再次触发中断异常 ← 关键
7. TFTF的中断处理函数（sgi_handler）处理NS SGI中断
```

### 答案

**Pending状态还在，TFTF仍然可以进入中断处理函数。**

**原因**：
- ✅ **TSP没有acknowledge中断**：`tsp_common_int_handler`只是读取pending中断ID，没有调用`plat_ic_acknowledge_interrupt()`
- ✅ **Pending状态保留在GIC中**：因为中断没有被acknowledge，pending状态保留在GIC中
- ✅ **TFTF enable中断后，pending的SGI会再次触发**：因为pending状态还在，所以会再次触发中断异常，TFTF的中断处理函数会被调用

