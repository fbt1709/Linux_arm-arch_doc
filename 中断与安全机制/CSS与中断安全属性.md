# CSS（当前安全状态）vs 中断安全属性

## 问题

**为什么CSS = 0（Secure状态），不是说了是non secure状态嘛？是tsp改的？**

## 答案

**CSS（Current Security State）是当前执行的安全状态，不是中断的安全属性。**

**关键理解**：
- ✅ **CSS = 0（Secure状态）**：表示**CPU当前在Secure状态执行**（TSP在S-EL1执行）
- ✅ **SGI的安全属性是Non-Secure**：表示**中断本身是Non-Secure的**（在GIC中配置）
- ✅ **这是两个不同的概念**：CSS是CPU的状态，中断安全属性是中断的属性
- ✅ **CSS不是TSP改的**：CSS由`SCR_EL3.NS`位决定，当TSP在S-EL1执行时，`SCR_EL3.NS=0`（Secure状态）

## 关键概念

### 1. CSS（Current Security State）- 当前安全状态

**定义**：
- **CSS = 0**：CPU当前在**Secure状态**执行
- **CSS = 1**：CPU当前在**Non-Secure状态**执行

**决定因素**：
- 由`SCR_EL3.NS`位决定
- 当执行在S-EL1/S-EL0时，`SCR_EL3.NS=0`（Secure状态）
- 当执行在NS-EL1/NS-EL0时，`SCR_EL3.NS=1`（Non-Secure状态）

**关键**：
- ✅ **CSS是CPU的状态**：表示CPU当前在哪个安全状态执行
- ✅ **CSS会改变**：当CPU在Secure和Non-Secure之间切换时，CSS会改变
- ✅ **CSS不是TSP改的**：CSS由ARM架构的`SCR_EL3.NS`位决定

### 2. 中断安全属性 - 中断本身的属性

**定义**：
- **Secure中断（Group 0）**：中断本身是Secure的
- **Non-Secure中断（Group 1）**：中断本身是Non-Secure的

**决定因素**：
- 由GIC的`IGROUPR`（Interrupt Group Register）决定
- 在GIC初始化时配置，不会改变

**关键**：
- ✅ **中断安全属性是中断的属性**：表示中断本身是Secure还是Non-Secure
- ✅ **中断安全属性不会改变**：一旦在GIC中配置，就不会改变
- ✅ **中断安全属性由TFTF设置**：TFTF在GIC中配置SGI为Non-Secure（Group 1）

## 完整流程分析

### 阶段1：TFTF设置SGI为Non-Secure

```c
// test_normal_int_switch.c:67
rc = tftf_irq_register_handler_sgi(IRQ_NS_SGI_0, sgi_handler);
```

**执行的操作**：
- ✅ **在GIC中配置SGI为Non-Secure**：设置`IGROUPR`寄存器，将SGI配置为Group 1（Non-Secure）
- ✅ **SGI的安全属性**：Non-Secure（Group 1）

**状态**：
- ✅ **CPU当前安全状态**：Non-Secure（CSS=1，因为TFTF在NS-EL1执行）
- ✅ **SGI的安全属性**：Non-Secure（Group 1）

### 阶段2：TSPD返回到TSP（Secure状态）

```c
// services/spd/tspd/tspd_main.c:651
SMC_RET3(&tsp_ctx->cpu_ctx, smc_fid, x1, x2);
```

**执行的操作**：
- ✅ **设置SCR_EL3.NS=0**：当ERET到TSP时，`SCR_EL3.NS=0`（Secure状态）
- ✅ **返回到TSP（S-EL1）**：通过`el3_exit`和`ERET`返回到TSP

**状态**：
- ✅ **CPU当前安全状态**：Secure（CSS=0，因为TSP在S-EL1执行）
- ✅ **SGI的安全属性**：Non-Secure（Group 1，没有改变）

**关键**：
- ✅ **CSS改变为0（Secure状态）**：因为CPU现在在S-EL1执行
- ✅ **SGI的安全属性仍然是Non-Secure**：没有改变
- ✅ **CSS不是TSP改的**：CSS由`SCR_EL3.NS`位决定，当ERET到S-EL1时，`SCR_EL3.NS=0`

### 阶段3：TSP Enable中断

```assembly
// bl32/tsp/aarch64/tsp_entrypoint.S:478
msr	daifclr, #DAIF_FIQ_BIT | DAIF_IRQ_BIT
```

**状态**：
- ✅ **CPU当前安全状态**：Secure（CSS=0，因为TSP在S-EL1执行）
- ✅ **SGI的安全属性**：Non-Secure（Group 1）
- ✅ **Pending的NS SGI触发中断异常**

### 阶段4：中断路由决策

```
NS SGI pending（在GIC中）
  ↓
硬件检测到pending中断
  ↓
检查当前安全状态：CSS=0（Secure状态）← 关键：这是CPU的状态
  ↓
检查路由模型：TEL3=0（路由到FEL）
  ↓
根据路由模型（CSS=0, TEL3=0），中断路由到FEL（S-EL1/TSP）
```

**关键**：
- ✅ **CSS=0（Secure状态）**：表示CPU当前在Secure状态执行（TSP在S-EL1）
- ✅ **SGI的安全属性是Non-Secure**：但这是中断的属性，不影响CSS
- ✅ **路由决策**：根据CSS（当前安全状态）和TEL3（路由模型）决定路由目标

## SCR_EL3.NS位的作用

### SCR_EL3.NS位决定CSS

**SCR_EL3.NS位**：
- **NS=0**：CPU在Secure状态执行（CSS=0）
- **NS=1**：CPU在Non-Secure状态执行（CSS=1）

**设置时机**：
- 当ERET到S-EL1时，`SCR_EL3.NS=0`（Secure状态）
- 当ERET到NS-EL1时，`SCR_EL3.NS=1`（Non-Secure状态）

**代码位置**：
```c
// lib/el3_runtime/aarch64/context_mgmt.c:426
scr_el3 &= ~(SCR_TWE_BIT | SCR_TWI_BIT | SCR_SMD_BIT);
// 当设置Secure上下文时，SCR_EL3.NS=0
```

## 关键理解

### 1. CSS是CPU的状态，不是中断的属性

**CSS（Current Security State）**：
- ✅ **表示CPU当前在哪个安全状态执行**
- ✅ **由SCR_EL3.NS位决定**
- ✅ **会随着CPU在Secure和Non-Secure之间切换而改变**

**中断安全属性**：
- ✅ **表示中断本身是Secure还是Non-Secure**
- ✅ **由GIC的IGROUPR寄存器决定**
- ✅ **不会改变**

### 2. 为什么CSS=0（Secure状态）？

**原因**：
- ✅ **TSP在S-EL1执行**：当TSP在S-EL1执行时，CPU处于Secure状态
- ✅ **SCR_EL3.NS=0**：当ERET到S-EL1时，`SCR_EL3.NS=0`（Secure状态）
- ✅ **CSS=0（Secure状态）**：因为CPU当前在Secure状态执行

**不是TSP改的**：
- ✅ **CSS由ARM架构决定**：当CPU在S-EL1执行时，`SCR_EL3.NS=0`，CSS=0
- ✅ **TSP只是执行在S-EL1**：TSP没有改变CSS，CSS是CPU的状态

### 3. 两个不同的概念

**CSS（当前安全状态）**：
- **CSS=0**：CPU当前在Secure状态执行（TSP在S-EL1）
- **CSS=1**：CPU当前在Non-Secure状态执行（TFTF在NS-EL1）

**中断安全属性**：
- **Secure中断（Group 0）**：中断本身是Secure的
- **Non-Secure中断（Group 1）**：中断本身是Non-Secure的（SGI）

## 时序图

```
TFTF (NS-EL1)                  EL3 (TSPD)                    TSP (S-EL1)
─────────────────              ───────────                    ───────────
CSS=1 (Non-Secure)             │                            │
  │                             │                            │
  │ tftf_smc()                  │                            │
  │                             │                            │
  ├────────────────────────────>│                            │
  │                             │                            │
  │                             │ tspd_smc_handler
  │                             │   - 设置SCR_EL3.NS=0
  │                             │   - SMC_RET3返回到TSP
  │                             │                            │
  │                             │ ERET                       │
  │                             ├──────────────────────────>│
  │                             │                            │ CSS=0 (Secure)
  │                             │                            │
  │                             │                            │ tsp_yield_smc_entry
  │                             │                            │ msr daifclr (enable中断)
  │                             │                            │
  │                             │                            │ ← NS SGI触发中断异常
  │                             │                            │
  │                             │                            │ 路由决策：
  │                             │                            │ CSS=0 (Secure状态)
  │                             │                            │ TEL3=0 (路由到FEL)
  │                             │                            │ → 路由到S-EL1/TSP
```

## 代码位置总结

### 1. SCR_EL3.NS位设置

**位置**：`lib/el3_runtime/aarch64/context_mgmt.c:426`
```c
scr_el3 &= ~(SCR_TWE_BIT | SCR_TWI_BIT | SCR_SMD_BIT);
// 当设置Secure上下文时，SCR_EL3.NS=0
```

### 2. CSS的定义

**位置**：`docs/design/interrupt-framework-design.rst:90`
```
CSS. Current Security State. 0 when secure and 1 when non-secure
```

### 3. 路由模型使用CSS

**位置**：`docs/design/interrupt-framework-design.rst:118`
```
CSS=0, TEL3=0. Interrupt is routed to the FEL when execution is in
secure state.
```

## 总结

### 关键点

1. **CSS是CPU的状态，不是中断的属性**：CSS表示CPU当前在哪个安全状态执行
2. **CSS由SCR_EL3.NS位决定**：当CPU在S-EL1执行时，`SCR_EL3.NS=0`，CSS=0
3. **CSS不是TSP改的**：CSS是ARM架构的状态，由CPU的执行状态决定
4. **中断安全属性是独立的**：SGI的安全属性（Non-Secure）不影响CSS

### 流程总结

```
1. TFTF在NS-EL1执行：CSS=1（Non-Secure状态）
2. TFTF设置SGI为Non-Secure（在GIC中配置）
3. TSPD返回到TSP（S-EL1）：CSS=0（Secure状态）← 关键：CPU现在在Secure状态
4. TSP enable中断，pending的NS SGI触发中断异常
5. 根据路由模型（CSS=0, TEL3=0），中断路由到FEL（S-EL1/TSP）
```

### 答案

**CSS = 0（Secure状态）表示CPU当前在Secure状态执行（TSP在S-EL1执行），不是中断的安全属性。**

**原因**：
- ✅ **CSS是CPU的状态**：表示CPU当前在哪个安全状态执行
- ✅ **CSS由SCR_EL3.NS位决定**：当TSP在S-EL1执行时，`SCR_EL3.NS=0`，CSS=0
- ✅ **CSS不是TSP改的**：CSS是ARM架构的状态，由CPU的执行状态决定
- ✅ **SGI的安全属性是Non-Secure**：但这是中断的属性，不影响CSS

