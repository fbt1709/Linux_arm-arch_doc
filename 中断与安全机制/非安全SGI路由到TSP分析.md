# 为什么Non-Secure SGI能被路由到TSP？

## 问题

**我在TFTF设置的SGI为非安全中断，为什么还能够被路由到TSP中呢？**

## 答案

**这是ARM中断路由机制的设计特性。** SGI的安全属性（Secure/Non-Secure）和中断路由模型是两个独立的概念：

1. **SGI的安全属性**：决定SGI是Secure还是Non-Secure（在GIC中配置）
2. **中断路由模型**：决定在某个安全状态下，中断应该路由到哪里（FEL还是EL3）

**即使SGI是Non-Secure的，当执行在Secure状态时，根据路由模型（`CSS=0, TEL3=0`），它仍然可以路由到FEL（S-EL1/TSP）。**

## 关键概念

### 1. SGI的安全属性（在GIC中配置）

**GIC寄存器配置**：
- **IGROUPR（Interrupt Group Register）**：决定中断是Secure（Group 0）还是Non-Secure（Group 1）
- **TFTF设置**：`IRQ_NS_SGI_0`表示这是一个Non-Secure SGI（Group 1）

**关键**：
- ✅ **SGI的安全属性是固定的**：一旦在GIC中配置为Non-Secure，它就是Non-Secure中断
- ✅ **安全属性不会改变**：无论中断路由到哪里，它的安全属性都是Non-Secure

### 2. 中断路由模型（在EL3中配置）

**路由模型定义**：
- **CSS（Current Security State）**：当前安全状态（0=Secure, 1=Non-Secure）
- **TEL3（Target Exception Level 3）**：目标异常级别（0=FEL, 1=EL3）

**默认路由模型**（`TSP_NS_INTR_ASYNC_PREEMPT=0`）：
- **Secure状态时**：`CSS=0, TEL3=0` → 路由到**FEL（S-EL1/TSP）**
- **Non-Secure状态时**：`CSS=1, TEL3=0` → 路由到**FEL（NS-EL1）**

## 文档说明

根据`docs/design/interrupt-framework-design.rst:118-123`：

```
Non-secure interrupts

#. **CSS=0, TEL3=0**. Interrupt is routed to the FEL when execution is in
   secure state. This allows the secure software to trap non-secure
   interrupts, perform its book-keeping and hand the interrupt to the
   non-secure software through EL3. This is a valid routing model as secure
   software is in control of how its execution is preempted by non-secure
   interrupts.
```

**关键理解**：
- ✅ **允许Secure软件"trap"（捕获）Non-Secure中断**：这是ARM设计的一个特性
- ✅ **Secure软件可以控制如何被Non-Secure中断抢占**：通过路由模型，Secure软件可以决定如何处理Non-Secure中断
- ✅ **这是有效的路由模型**：`CSS=0, TEL3=0`对于Non-Secure中断是有效的路由模型

## 完整流程分析

### 阶段1：TFTF设置SGI为Non-Secure

```c
// test_normal_int_switch.c:67
rc = tftf_irq_register_handler_sgi(IRQ_NS_SGI_0, sgi_handler);
```

**执行的操作**：
- ✅ **在GIC中配置SGI为Non-Secure**：设置`IGROUPR`寄存器，将SGI配置为Group 1（Non-Secure）
- ✅ **注册中断处理函数**：在TFTF中注册`sgi_handler`作为SGI的处理函数

**状态**：
- ✅ **SGI的安全属性**：Non-Secure（Group 1）
- ✅ **SGI的路由模型**：使用默认路由模型（`CSS=0, TEL3=0`）

### 阶段2：TSP在Secure状态执行

```
TSP在S-EL1执行（Secure状态）
  ↓
CSS = 0（Secure状态）
  ↓
TSP enable中断
  ↓
Pending的NS SGI触发中断异常
```

**状态**：
- ✅ **当前安全状态**：Secure（CSS=0）
- ✅ **SGI的安全属性**：Non-Secure（Group 1）
- ✅ **路由模型**：`CSS=0, TEL3=0` → 路由到FEL（S-EL1/TSP）

### 阶段3：中断路由到TSP

```
NS SGI pending（在GIC中）
  ↓
硬件检测到pending中断
  ↓
检查中断路由配置
  ↓
CSS=0（Secure状态），TEL3=0（路由到FEL）
  ↓
中断路由到FEL（S-EL1/TSP）← 关键：即使SGI是Non-Secure的
  ↓
中断异常触发，跳转到TSP的异常向量表
```

**关键**：
- ✅ **SGI的安全属性是Non-Secure**：但这不影响路由决策
- ✅ **路由模型决定路由目标**：`CSS=0, TEL3=0`决定路由到FEL（S-EL1）
- ✅ **TSP可以"trap"（捕获）Non-Secure中断**：这是ARM设计的一个特性

### 阶段4：TSP检测并返回

```c
// bl32/tsp/tsp_interrupt.c:83-96
id = plat_ic_get_pending_interrupt_id();

if (id != TSP_IRQ_SEC_PHY_TIMER) {
    return tsp_handle_preemption();  // 返回TSP_PREEMPTED
}
```

**关键**：
- ✅ **TSP检测到NS中断**：TSP检测到这不是Secure中断（不是Secure Timer）
- ✅ **TSP返回TSP_PREEMPTED**：TSP不处理NS中断，只是检测并返回
- ✅ **TSP通过SMC返回到EL3**：TSP执行`smc #0`返回到EL3

### 阶段5：EL3返回到TFTF

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
  ↓
TFTF enable中断
  ↓
Pending的NS SGI再次触发中断异常
  ↓
TFTF的中断处理函数（sgi_handler）处理NS SGI中断
```

**关键**：
- ✅ **最终在TFTF中处理NS中断**：虽然中断先路由到TSP，但最终还是在TFTF中处理

## 关键理解

### 1. 安全属性和路由模型是独立的

**安全属性**：
- 决定中断是Secure还是Non-Secure
- 在GIC中配置（IGROUPR寄存器）
- 不会改变

**路由模型**：
- 决定在某个安全状态下，中断应该路由到哪里
- 在EL3中配置（通过`enable_intr_rm_local`或默认路由）
- 可以根据需要改变

### 2. 为什么允许Non-Secure中断路由到Secure世界？

**ARM设计的原因**：
- ✅ **允许Secure软件"trap"（捕获）Non-Secure中断**：Secure软件可以控制如何被Non-Secure中断抢占
- ✅ **提供灵活性**：Secure软件可以决定如何处理Non-Secure中断
- ✅ **符合安全原则**：Secure软件在控制Non-Secure中断的处理流程

### 3. 路由模型的作用

**`CSS=0, TEL3=0`（默认路由模型）**：
- **作用**：当执行在Secure状态时，Non-Secure中断路由到FEL（S-EL1）
- **目的**：允许Secure软件（TSP）检测并处理Non-Secure中断
- **结果**：TSP可以"trap"（捕获）Non-Secure中断，然后决定如何处理

**`CSS=0, TEL3=1`（抢占路由模型，`TSP_NS_INTR_ASYNC_PREEMPT=1`）**：
- **作用**：当执行在Secure状态时，Non-Secure中断路由到EL3
- **目的**：让EL3直接处理Non-Secure中断，不需要经过TSP
- **结果**：EL3可以直接处理Non-Secure中断，返回到Non-Secure世界

## 代码位置总结

### 1. TFTF设置SGI为Non-Secure

**位置**：`test_normal_int_switch.c:67`
```c
rc = tftf_irq_register_handler_sgi(IRQ_NS_SGI_0, sgi_handler);
```

### 2. 默认路由模型

**位置**：`docs/design/interrupt-framework-design.rst:118-123`
```
CSS=0, TEL3=0. Interrupt is routed to the FEL when execution is in
secure state. This allows the secure software to trap non-secure
interrupts...
```

### 3. TSP检测NS中断

**位置**：`bl32/tsp/tsp_interrupt.c:83-96`
```c
id = plat_ic_get_pending_interrupt_id();
if (id != TSP_IRQ_SEC_PHY_TIMER) {
    return tsp_handle_preemption();  // 检测到NS中断，返回
}
```

## 总结

### 关键点

1. **SGI的安全属性是Non-Secure**：在GIC中配置，不会改变
2. **路由模型决定路由目标**：`CSS=0, TEL3=0`决定路由到FEL（S-EL1）
3. **这是ARM设计的一个特性**：允许Secure软件"trap"（捕获）Non-Secure中断
4. **TSP可以检测并返回**：TSP检测到NS中断，通过SMC返回到EL3
5. **最终在TFTF中处理**：虽然中断先路由到TSP，但最终还是在TFTF中处理

### 流程总结

```
1. TFTF设置SGI为Non-Secure（在GIC中配置IGROUPR）
2. TSP在Secure状态执行（CSS=0）
3. Pending的NS SGI触发中断异常
4. 根据路由模型（CSS=0, TEL3=0），中断路由到FEL（S-EL1/TSP）
5. TSP检测到NS中断，返回TSP_PREEMPTED
6. TSP通过SMC返回到EL3
7. EL3返回到TFTF
8. TFTF处理NS SGI中断
```

### 答案

**即使SGI是Non-Secure的，当执行在Secure状态时，根据路由模型（`CSS=0, TEL3=0`），它仍然可以路由到FEL（S-EL1/TSP）。**

**原因**：
- ✅ **安全属性和路由模型是独立的**：SGI的安全属性（Non-Secure）不影响路由决策
- ✅ **路由模型决定路由目标**：`CSS=0, TEL3=0`决定路由到FEL（S-EL1）
- ✅ **这是ARM设计的一个特性**：允许Secure软件"trap"（捕获）Non-Secure中断，控制如何被Non-Secure中断抢占

