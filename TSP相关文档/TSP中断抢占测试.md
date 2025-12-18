# TSP中断抢占测试说明

## `preempt_tsp_via_SGI` 函数作用

这个函数用于**测试非安全中断（NS SGI）抢占安全世界SMC调用**的功能。

## 测试目的

验证当TSP（Trusted Secure Payload）正在执行一个Standard SMC时，非安全世界发送的SGI中断能够正确抢占该SMC，并让TSP返回`TSP_SMC_PREEMPTED`状态。

## 测试流程详解

### 1. 初始化阶段（66-75行）

```c
/* 注册SGI 0的中断处理器 */
rc = tftf_irq_register_handler_sgi(IRQ_NS_SGI_0, sgi_handler);

/* 使能SGI 0，设置最高非安全优先级 */
tftf_irq_enable_sgi(IRQ_NS_SGI_0, GIC_HIGHEST_NS_PRIORITY);
```

- 注册SGI 0的中断处理器（`sgi_handler`）
- 使能SGI 0，设置为最高非安全优先级

### 2. 禁用中断（77-78行）

```c
/* 设置 PSTATE.I = 0（禁用IRQ） */
disable_irq();
```

**关键点**：禁用中断后，后续发送的SGI不会立即被处理，而是进入pending状态。

### 3. 发送SGI（80-84行）

```c
/*
 * 发送SGI到当前CPU。由于中断被禁用，
 * SGI无法立即处理，会保持pending状态。
 */
tftf_send_sgi(IRQ_NS_SGI_0, core_pos);
```

**状态**：SGI已发送但pending，等待中断使能后处理。

### 4. 调用STD SMC（86-96行）

```c
/*
 * 调用STD SMC。由于有pending的SGI，
 * 这个SMC应该被抢占。
 */
shared_data.tsp_result = tftf_smc(tsp_svc_params);

/* 验证SMC返回TSP_SMC_PREEMPTED */
if (shared_data.tsp_result.ret0 != TSP_SMC_PREEMPTED) {
    // 测试失败
}
```

**预期行为**：
- TSP开始执行SMC
- 检测到pending的NS中断
- TSP保存上下文并返回`TSP_SMC_PREEMPTED`
- 控制权返回到非安全世界

### 5. 使能中断（98-99行）

```c
/* 设置 PSTATE.I = 1（使能IRQ） */
enable_irq();
```

**结果**：pending的SGI现在可以被处理，`sgi_handler`会被调用。

### 6. 清理阶段（101-110行）

```c
/* 禁用SGI 0 */
tftf_irq_disable_sgi(IRQ_NS_SGI_0);

/* 注销中断处理器 */
rc = tftf_irq_unregister_handler_sgi(IRQ_NS_SGI_0);
```

## 时序图

```
时间轴：
│
├─ [1] 注册SGI处理器，使能SGI 0
│
├─ [2] disable_irq()  ← PSTATE.I = 0
│
├─ [3] 发送SGI 0      ← SGI pending（无法处理）
│
├─ [4] 调用STD SMC    ← TSP开始执行
│      │
│      ├─ TSP检测到pending NS中断
│      ├─ TSP保存上下文
│      └─ 返回 TSP_SMC_PREEMPTED
│
├─ [5] enable_irq()   ← PSTATE.I = 1
│
└─ [6] SGI被处理      ← sgi_handler() 被调用
```

## 测试场景

这个函数被多个测试用例调用：

### 1. `tsp_int_and_resume`（122行）
- 测试各种STD SMC（ADD, SUB, MUL, DIV）被抢占
- 然后通过RESUME SMC恢复执行
- 验证结果正确性

### 2. `test_fast_smc_when_tsp_preempted`（234行）
- 测试TSP被抢占后，发送Fast SMC应该返回错误
- 验证TSP的状态管理

### 3. 其他测试用例
- 测试不同的中断场景和边界条件

## 关键机制

### 1. 中断抢占机制

```
非安全世界 → 发送NS SGI → 安全世界（TSP）被抢占
```

### 2. TSP的抢占处理

当TSP执行STD SMC时：
1. 检测到pending的NS中断
2. 保存当前执行上下文
3. 返回`TSP_SMC_PREEMPTED`给调用者
4. 等待后续的RESUME SMC来恢复执行

### 3. 中断禁用的重要性

```c
disable_irq();        // 确保SGI不会立即处理
tftf_send_sgi(...);   // SGI进入pending状态
tftf_smc(...);        // SMC被pending的SGI抢占
enable_irq();         // 现在SGI可以被处理
```

如果不禁用中断，SGI会立即被处理，无法测试抢占机制。

## 验证点

1. ✅ **抢占成功**：SMC返回`TSP_SMC_PREEMPTED`
2. ✅ **状态保存**：TSP正确保存了执行上下文
3. ✅ **恢复执行**：通过RESUME SMC可以恢复并完成原始操作
4. ✅ **结果正确**：恢复后的计算结果正确

## 总结

这个测试函数验证了：
- **非安全中断可以抢占安全世界的SMC执行**
- **TSP正确处理抢占并保存上下文**
- **抢占后可以通过RESUME恢复执行**

这是TrustZone架构中**安全世界和非安全世界之间中断交互**的重要测试。

