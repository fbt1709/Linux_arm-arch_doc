# Pending SGI 在中断关闭时的处理机制

## 问题

**测试用例中，已经运行到了TSPD，检测到SGI是pending状态，但是此时中断是关闭的（PSTATE.I=1），并没有enable，这个时候会怎么办呢？**

## 关键理解

**TSPD在EL3时，不会检查pending状态，也不会enable中断。** 它只是：
1. 设置中断路由（让NS中断路由到EL3）
2. 允许抢占（通过EHF框架）
3. ERET到TSP（S-EL1）

**关键点**：**TSP在S-EL1会立即enable中断**，一旦中断被enable，pending的SGI立即触发中断异常。

## 详细流程分析

### 阶段1：Non-Secure世界（中断关闭）

```c
// test_normal_int_switch.c:78
disable_irq();  // PSTATE.I = 1，中断禁用

// test_normal_int_switch.c:84
tftf_send_sgi(IRQ_NS_SGI_0, core_pos);  // SGI进入pending状态

// test_normal_int_switch.c:90
shared_data.tsp_result = tftf_smc(tsp_svc_params);  // 调用SMC
```

**状态**：
- ✅ **PSTATE.I = 1**：中断禁用
- ✅ **SGI pending**：SGI在GIC中处于pending状态
- ✅ **硬件不会触发中断异常**：因为中断被禁用

### 阶段2：进入EL3（TSPD）

```c
// services/spd/tspd/tspd_main.c:632
enable_intr_rm_local(INTR_TYPE_NS, SECURE);  // 设置路由：NS中断路由到EL3

// services/spd/tspd/tspd_main.c:645
ehf_allow_ns_preemption(TSP_PREEMPTED);  // 允许NS抢占，预设返回值

// services/spd/tspd/tspd_main.c:651
SMC_RET3(&tsp_ctx->cpu_ctx, smc_fid, x1, x2);  // ERET到TSP
```

**关键**：
- ❌ **TSPD不检查pending状态**：TSPD在EL3时，不会检查是否有pending的SGI
- ❌ **TSPD不enable中断**：TSPD在EL3时，不会enable中断
- ✅ **TSPD设置路由**：让NS中断路由到EL3（`CSS=0, TEL3=1`）
- ✅ **TSPD允许抢占**：通过EHF框架允许NS抢占
- ✅ **TSPD ERET到TSP**：将控制权交给TSP（S-EL1）

### 阶段3：TSP在S-EL1（立即enable中断）

```assembly
// bl32/tsp/aarch64/tsp_entrypoint.S:477-479
func tsp_yield_smc_entry
    msr	daifclr, #DAIF_FIQ_BIT | DAIF_IRQ_BIT  // ← 关键：立即enable中断
    bl	tsp_smc_handler
```

**关键**：
- ✅ **TSP立即enable中断**：`msr daifclr`清除DAIF位，enable中断
- ✅ **Pending SGI立即触发**：一旦中断被enable，pending的SGI立即触发中断异常
- ✅ **中断路由到EL3**：由于路由已设置（`CSS=0, TEL3=1`），中断路由到EL3

### 阶段4：中断异常触发（TSP被抢占）

```
1. TSP enable中断（daifclr）
   ↓
2. 硬件检测到pending的SGI
   ↓
3. 由于路由已设置（TEL3=1），中断路由到EL3
   ↓
4. 中断异常触发，跳转到EL3异常向量表
   ↓
5. EL3处理中断，TSP被抢占
   ↓
6. 返回到TSPD，返回TSP_PREEMPTED（已预设）
```

## 关键点总结

### 1. TSPD不检查pending状态

**原因**：
- TSPD在EL3时，不需要检查pending状态
- 它只是设置路由和允许抢占
- 实际的抢占发生在TSP enable中断时

### 2. TSPD不enable中断

**原因**：
- TSPD在EL3时，不会enable中断
- 它只是设置路由和允许抢占
- TSP负责管理自己的中断状态

### 3. TSP立即enable中断

**原因**：
- TSP的`tsp_yield_smc_entry`会立即enable中断
- 这是TSP的设计：yielding SMC应该允许中断
- 一旦中断被enable，pending的SGI立即触发

### 4. 抢占时机

**抢占发生在**：
- TSP enable中断的**瞬间**
- 硬件检测到pending的SGI
- 由于路由已设置，中断路由到EL3
- TSP被立即抢占

## 代码证据

### TSPD的注释

```c
// services/spd/tspd/tspd_main.c:616-617
/* We expect the TSP to manage the PSTATE.I and PSTATE.F
 * flags as appropriate.
 */
```

**关键**：TSPD期望TSP自己管理中断状态。

### TSP的entry point

```assembly
// bl32/tsp/aarch64/tsp_entrypoint.S:477-479
func tsp_yield_smc_entry
    msr	daifclr, #DAIF_FIQ_BIT | DAIF_IRQ_BIT  // 立即enable中断
    bl	tsp_smc_handler
```

**关键**：TSP在yielding SMC entry时，立即enable中断。

### 测试用例的注释

```c
// test_normal_int_switch.c:80-83
/*
 * Send SGI to the current CPU. It can't be handled because the
 * interrupts are disabled.
 */
```

**关键**：注释说"不能处理"，但这是指在Non-Secure世界。一旦进入TSP，TSP会enable中断，然后就能处理了。

## 时序图

```
Non-Secure (PSTATE.I=1)          EL3 (TSPD)                    S-EL1 (TSP)
───────────────────────          ──────────                    ────────────
disable_irq()
  │
  │ PSTATE.I = 1
  │
send_sgi()
  │
  │ SGI pending
  │
tftf_smc()
  │
  ├─────────────────────────────────>│
  │                                    │ enable_intr_rm_local()
  │                                    │ ehf_allow_ns_preemption()
  │                                    │
  │                                    │ ERET
  │                                    ├──────────────────────────>│
  │                                    │                            │
  │                                    │                            │ msr daifclr
  │                                    │                            │ (enable中断)
  │                                    │                            │
  │                                    │                            │ ← Pending SGI触发！
  │                                    │                            │
  │                                    │<───────────────────────────┤
  │                                    │ 中断异常（路由到EL3）
  │                                    │
  │                                    │ 处理抢占
  │                                    │
  │<───────────────────────────────────┤
  │ 返回TSP_PREEMPTED
  │
enable_irq()  // 在Non-Secure世界enable中断
```

## 为什么这样设计？

### 1. 职责分离

- **TSPD（EL3）**：负责设置路由和允许抢占
- **TSP（S-EL1）**：负责管理自己的中断状态

### 2. 灵活性

- TSP可以根据需要enable/disable中断
- TSPD不需要知道TSP的中断状态

### 3. 性能

- 不需要在EL3检查pending状态
- 抢占发生在TSP enable中断时，时机准确

## 总结

### 关键理解

1. **TSPD不检查pending状态**：它只是设置路由和允许抢占
2. **TSPD不enable中断**：它期望TSP自己管理中断状态
3. **TSP立即enable中断**：在`tsp_yield_smc_entry`中立即enable中断
4. **抢占发生在enable中断时**：一旦中断被enable，pending的SGI立即触发

### 流程总结

```
1. Non-Secure: disable_irq() + send_sgi() → SGI pending
2. Non-Secure: tftf_smc() → 进入EL3
3. EL3 (TSPD): 设置路由 + 允许抢占 + ERET到TSP
4. S-EL1 (TSP): enable中断 → Pending SGI立即触发
5. EL3: 处理中断，TSP被抢占
6. Non-Secure: 收到TSP_PREEMPTED返回值
```

### 答案

**TSPD在EL3时，不会检查pending状态，也不会enable中断。** 它只是设置路由和允许抢占，然后ERET到TSP。**TSP在S-EL1会立即enable中断**，一旦中断被enable，pending的SGI立即触发中断异常，由于路由已设置，中断路由到EL3，TSP被立即抢占。

**这就是为什么测试用例中，即使Non-Secure世界的中断是关闭的，TSP仍然会被pending的SGI抢占的原因。**

