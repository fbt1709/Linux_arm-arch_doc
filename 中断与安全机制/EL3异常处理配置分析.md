# EL3_EXCEPTION_HANDLING 配置分析

## 问题

**`EL3_EXCEPTION_HANDLING` 是不是必须打开才能让TSP抢占机制工作？**

## 答案

**不一定，但推荐打开。** 这取决于你的配置组合。

## 两个关键配置

### 1. `TSP_NS_INTR_ASYNC_PREEMPT`

**作用**：控制NS中断是否路由到EL3来抢占Secure执行

**位置**：`services/spd/tspd/tspd.mk:46`

```makefile
$(eval $(call add_define,TSP_NS_INTR_ASYNC_PREEMPT))
```

**行为**：
- **= 1**：NS中断路由到EL3，可以抢占Secure-EL1
  - 在Yielding SMC期间，NS中断路由到EL3（`CSS=0, TEL3=1`）
  - 调用`enable_intr_rm_local(INTR_TYPE_NS, SECURE)`（626-633行）
- **= 0**：使用默认路由模型
  - NS中断路由到FEL（当前EL），不能抢占Secure执行

### 2. `EL3_EXCEPTION_HANDLING`

**作用**：控制是否使用EHF（Exception Handling Framework）

**位置**：平台Makefile（如`plat/qemu/qemu_sbsa/platform.mk:20`）

**行为**：
- **= 1**：使用EHF框架
  - 需要调用`ehf_allow_ns_preemption()`来允许NS抢占
  - 注释说明："With EL3 exception handling, while an SMC is being processed, Non-secure interrupts can't preempt Secure execution."
- **= 0**：不使用EHF框架
  - 可能使用其他中断处理机制

## 代码分析

### TSPD中的配置使用

```c
// services/spd/tspd/tspd_main.c:626-646
#if TSP_NS_INTR_ASYNC_PREEMPT
    /*
     * Enable the routing of NS interrupts to EL3
     * during processing of a Yielding SMC Call on
     * this core.
     */
    enable_intr_rm_local(INTR_TYPE_NS, SECURE);
#endif

#if EL3_EXCEPTION_HANDLING
    /*
     * With EL3 exception handling, while an SMC is
     * being processed, Non-secure interrupts can't
     * preempt Secure execution. However, for
     * yielding SMCs, we want preemption to happen;
     * so explicitly allow NS preemption in this
     * case, and supply the preemption return code
     * for TSP.
     */
    ehf_allow_ns_preemption(TSP_PREEMPTED);
#endif
```

### 关键理解

1. **`TSP_NS_INTR_ASYNC_PREEMPT=1`**：让NS中断路由到EL3
2. **`EL3_EXCEPTION_HANDLING=1`**：需要显式允许NS抢占（因为EHF默认阻止）

## 配置组合分析

### 组合1：`TSP_NS_INTR_ASYNC_PREEMPT=1`, `EL3_EXCEPTION_HANDLING=1` ✅ **推荐**

**行为**：
- NS中断路由到EL3（`enable_intr_rm_local`）
- 调用`ehf_allow_ns_preemption`允许抢占
- **抢占机制正常工作**

**这是标准配置，推荐使用。**

### 组合2：`TSP_NS_INTR_ASYNC_PREEMPT=1`, `EL3_EXCEPTION_HANDLING=0` ❓ **可能工作**

**行为**：
- NS中断路由到EL3（`enable_intr_rm_local`）
- **不调用**`ehf_allow_ns_preemption`
- 如果EHF未启用，可能使用其他机制处理中断

**问题**：
- 如果没有EHF，NS中断可能默认就能抢占？
- 但需要确认是否有其他机制阻止抢占
- **需要测试验证**

### 组合3：`TSP_NS_INTR_ASYNC_PREEMPT=0`, `EL3_EXCEPTION_HANDLING=1` ❌ **不工作**

**行为**：
- NS中断**不路由到EL3**（使用默认路由）
- 调用`ehf_allow_ns_preemption`（但无效，因为中断不路由到EL3）
- **抢占机制不工作**

### 组合4：`TSP_NS_INTR_ASYNC_PREEMPT=0`, `EL3_EXCEPTION_HANDLING=0` ❌ **不工作**

**行为**：
- NS中断**不路由到EL3**（使用默认路由）
- **不调用**`ehf_allow_ns_preemption`
- **抢占机制不工作**

## 文档说明

根据`docs/design/interrupt-framework-design.rst:437-441`：

```
When the build flag TSP_NS_INTR_ASYNC_PREEMPT is defined to 1, then the
non secure interrupts are routed to EL3 when execution is in secure state
i.e CSS=0, TEL3=1 for non-secure interrupts. This effectively preempts
Secure-EL1.
```

**关键**：`TSP_NS_INTR_ASYNC_PREEMPT=1`是**必要条件**，让NS中断路由到EL3。

## 结论

### 必须配置

1. **`TSP_NS_INTR_ASYNC_PREEMPT=1`**：**必须**，让NS中断路由到EL3

### 推荐配置

2. **`EL3_EXCEPTION_HANDLING=1`**：**推荐**，使用EHF框架，需要显式允许NS抢占

### 为什么推荐`EL3_EXCEPTION_HANDLING=1`？

1. **明确的抢占控制**：`ehf_allow_ns_preemption`提供明确的抢占控制机制
2. **预设返回值**：自动预设`TSP_PREEMPTED`返回值（362行）
3. **优先级管理**：EHF提供优先级掩码管理，确保抢占正确工作
4. **标准配置**：这是ATF的标准配置，经过充分测试

### 如果`EL3_EXCEPTION_HANDLING=0`会怎样？

**可能的问题**：
- 如果没有EHF，NS中断可能默认就能抢占，但：
  - 没有预设返回值机制
  - 没有优先级掩码管理
  - 抢占行为可能不确定

**建议**：
- **推荐使用`EL3_EXCEPTION_HANDLING=1`**
- 如果必须使用`EL3_EXCEPTION_HANDLING=0`，需要：
  1. 确认平台是否支持
  2. 测试抢占机制是否正常工作
  3. 检查返回值是否正确

## 检查你的配置

### 1. 检查`TSP_NS_INTR_ASYNC_PREEMPT`

```bash
grep -r "TSP_NS_INTR_ASYNC_PREEMPT" arm-trusted-firmware/services/spd/tspd/
```

### 2. 检查`EL3_EXCEPTION_HANDLING`

```bash
grep -r "EL3_EXCEPTION_HANDLING" arm-trusted-firmware/plat/*/platform.mk
```

### 3. 编译时检查

```bash
make SPD=tspd EL3_EXCEPTION_HANDLING=1 TSP_NS_INTR_ASYNC_PREEMPT=1
```

## 总结

**`EL3_EXCEPTION_HANDLING`不是绝对必须的，但强烈推荐打开。**

**最小配置**：
- `TSP_NS_INTR_ASYNC_PREEMPT=1`：**必须**
- `EL3_EXCEPTION_HANDLING=1`：**强烈推荐**

**如果`EL3_EXCEPTION_HANDLING=0`**：
- 需要测试验证抢占机制是否正常工作
- 可能缺少预设返回值和优先级管理
- 行为可能不确定

**建议**：使用标准配置 `TSP_NS_INTR_ASYNC_PREEMPT=1` + `EL3_EXCEPTION_HANDLING=1`

