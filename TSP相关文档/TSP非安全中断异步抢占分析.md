# TSP_NS_INTR_ASYNC_PREEMPT 配置分析

## 问题

**`TSP_NS_INTR_ASYNC_PREEMPT` 可以不需要吗？**

## 答案

**技术上可以不需要，但不推荐。** 原因如下：

## 两种路由模型对比

### 1. 默认路由模型（`TSP_NS_INTR_ASYNC_PREEMPT=0`）

**NS中断路由**：
- **Secure状态时**：`CSS=0, TEL3=0` → 路由到**FEL（S-EL1/TSP）**
- **Non-Secure状态时**：`CSS=1, TEL3=0` → 路由到**FEL（NS-EL1）**

**问题**：
- NS中断会路由到TSP（S-EL1）
- TSP需要处理这个NS中断
- TSP检测到不是Secure中断，返回`TSP_PREEMPTED`
- 然后执行`smc #0`返回到EL3

**流程**：
```
1. NS中断触发
   ↓
2. 路由到S-EL1（TSP）← 默认路由模型
   ↓
3. TSP异常向量表接收中断
   ↓
4. 调用tsp_common_int_handler
   ↓
5. 检测到不是Secure中断（如Secure Timer）
   ↓
6. 返回TSP_PREEMPTED
   ↓
7. 执行smc #0返回到EL3
   ↓
8. EL3处理NS中断
```

**缺点**：
- ❌ **额外的上下文切换**：NS中断先到S-EL1，再返回到EL3
- ❌ **性能开销**：TSP需要保存/恢复上下文
- ❌ **复杂性**：TSP需要处理NS中断，即使它不应该处理

### 2. 抢占路由模型（`TSP_NS_INTR_ASYNC_PREEMPT=1`）

**NS中断路由**：
- **Secure状态时**：`CSS=0, TEL3=1` → 路由到**EL3**
- **Non-Secure状态时**：`CSS=1, TEL3=0` → 路由到**FEL（NS-EL1）**

**优势**：
- NS中断直接路由到EL3
- EL3可以直接处理NS中断
- 不需要经过TSP

**流程**：
```
1. NS中断触发
   ↓
2. 路由到EL3 ← 抢占路由模型
   ↓
3. EL3直接处理NS中断
   ↓
4. 返回到Non-Secure世界
```

**优点**：
- ✅ **直接处理**：NS中断直接到EL3，不需要经过TSP
- ✅ **性能更好**：减少上下文切换开销
- ✅ **逻辑清晰**：TSP只处理Secure中断

## 代码分析

### TSP的异常处理

```c
// bl32/tsp/aarch64/tsp_exceptions.S:52-68
.macro	handle_tsp_interrupt label
    msr	daifclr, #DAIF_ABT_BIT
    save_caller_regs_and_lr
    bl	tsp_common_int_handler
    cbz	x0, interrupt_exit_\label
    smc	#0  // 如果不是Secure中断，返回到EL3
interrupt_exit_\label:
    restore_caller_regs_and_lr
    exception_return
.endm
```

```c
// bl32/tsp/tsp_interrupt.c:69-115
int32_t tsp_common_int_handler(void)
{
    id = plat_ic_get_pending_interrupt_id();
    if (id != TSP_IRQ_SEC_PHY_TIMER) {
        return tsp_handle_preemption();  // 返回TSP_PREEMPTED
    }
    // ... 处理Secure Timer中断 ...
    return 0;
}
```

**关键**：如果NS中断路由到TSP，TSP会检测到它不是Secure中断，然后返回到EL3。

### TSPD的抢占处理

```c
// services/spd/tspd/tspd_main.c:626-633
#if TSP_NS_INTR_ASYNC_PREEMPT
    /*
     * Enable the routing of NS interrupts to EL3
     * during processing of a Yielding SMC Call on
     * this core.
     */
    enable_intr_rm_local(INTR_TYPE_NS, SECURE);
#endif
```

**关键**：`enable_intr_rm_local`将NS中断路由到EL3，而不是S-EL1。

## 性能对比

### 默认路由模型（`TSP_NS_INTR_ASYNC_PREEMPT=0`）

```
NS中断 → S-EL1（TSP）→ 检测 → smc #0 → EL3 → 处理
```

**开销**：
- TSP上下文保存/恢复
- TSP异常向量表处理
- TSP中断检测
- SMC调用开销

### 抢占路由模型（`TSP_NS_INTR_ASYNC_PREEMPT=1`）

```
NS中断 → EL3 → 处理
```

**开销**：
- 直接EL3处理
- 无TSP上下文切换

## 文档说明

根据`docs/design/interrupt-framework-design.rst:118-125`：

```
#. **CSS=0, TEL3=0**. Interrupt is routed to the FEL when execution is in
   secure state. This allows the secure software to trap non-secure
   interrupts. This is a valid routing model as secure software in S-EL1
   can handover the interrupt to EL3 for handling.
```

**关键**：默认路由模型允许Secure软件"trap"（捕获）NS中断，然后交给EL3处理。

但是，文档也说明：

```
#. **CSS=0, TEL3=1**. Interrupt is routed to EL3 when execution is in
   secure state. This is a valid routing model as secure software in EL3
   can handover the interrupt to Secure-EL1 for handling.
```

**关键**：抢占路由模型让NS中断直接到EL3，这是更高效的方式。

## 实际影响

### 如果`TSP_NS_INTR_ASYNC_PREEMPT=0`

**会发生什么**：
1. NS中断路由到TSP（S-EL1）
2. TSP检测到不是Secure中断
3. TSP返回`TSP_PREEMPTED`
4. TSP执行`smc #0`返回到EL3
5. EL3处理NS中断

**问题**：
- ❌ **性能开销**：额外的上下文切换
- ❌ **延迟增加**：NS中断处理延迟
- ❌ **复杂性**：TSP需要处理NS中断

### 如果`TSP_NS_INTR_ASYNC_PREEMPT=1`

**会发生什么**：
1. NS中断直接路由到EL3
2. EL3直接处理NS中断
3. 返回到Non-Secure世界

**优势**：
- ✅ **性能更好**：无额外上下文切换
- ✅ **延迟更低**：NS中断直接处理
- ✅ **逻辑清晰**：TSP只处理Secure中断

## 结论

### 可以不需要吗？

**技术上可以，但不推荐。**

**原因**：
1. **性能问题**：默认路由模型会导致额外的上下文切换开销
2. **延迟增加**：NS中断需要先到TSP，再返回到EL3
3. **逻辑混乱**：TSP不应该处理NS中断

### 推荐配置

**强烈推荐使用`TSP_NS_INTR_ASYNC_PREEMPT=1`**：
- ✅ 性能更好
- ✅ 延迟更低
- ✅ 逻辑清晰
- ✅ 符合设计意图

### 什么时候可以不需要？

**只有在以下情况下可以考虑不使用**：
1. **不需要NS中断抢占**：如果不需要NS中断抢占Secure执行
2. **性能不重要**：如果性能开销可以接受
3. **特殊需求**：如果有特殊的安全或调试需求

### 总结

| 配置 | 性能 | 延迟 | 逻辑 | 推荐 |
|------|------|------|------|------|
| `TSP_NS_INTR_ASYNC_PREEMPT=0` | ❌ 差 | ❌ 高 | ❌ 混乱 | ❌ 不推荐 |
| `TSP_NS_INTR_ASYNC_PREEMPT=1` | ✅ 好 | ✅ 低 | ✅ 清晰 | ✅ **强烈推荐** |

**建议**：**使用`TSP_NS_INTR_ASYNC_PREEMPT=1`**

