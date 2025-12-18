# ARM 中断分组与路由机制详解

## 目录
1. [中断分组概述](#中断分组概述)
2. [中断分组与 FIQ/IRQ 详解](#中断分组与-fiqiirq-详解)
3. [GIC 中断分组](#gic-中断分组)
4. [中断路由模型](#中断路由模型)
5. [SCR_EL3 寄存器控制](#screl3-寄存器控制)
6. [ARM 异常向量表详解](#arm-异常向量表详解)
7. [GIC 中断类型详解：PPI 和 LPI](#gic-中断类型详解ppi-和-lpi)
8. [各种场景下的中断处理](#各种场景下的中断处理)
9. [代码实现分析](#代码实现分析)
10. [总结](#总结)

---

## 中断分组概述

ARM 架构中的中断根据其安全属性和处理方式被分为不同的组。这种分组机制确保了安全世界（Secure World）和非安全世界（Non-secure World）之间的隔离。

### 中断分组类型

1. **Group 0 (G0)**: 安全中断，总是路由到 EL3
2. **Group 1 Secure (G1S)**: 安全 Group 1 中断，可以路由到 S-EL1 或 EL3
3. **Group 1 Non-secure (G1NS)**: 非安全 Group 1 中断，可以路由到 NS-EL1/EL2 或 EL3

### Group 0 与 Group 1S 安全中断的区别

虽然 Group 0 和 Group 1S 都是安全中断，但它们在处理方式上有重要区别：

| 特性 | Group 0 (安全) | Group 1S (安全) |
|------|---------------|----------------|
| **GIC 配置** | IGROUP = 0, IGRPMOD = 0 | IGROUP = 1, IGRPMOD = 1 |
| **TF-A 类型** | `INTR_TYPE_EL3` | `INTR_TYPE_S_EL1` |
| **路由目标** | 总是路由到 EL3 | 可路由到 S-EL1 或 EL3（取决于路由模型） |
| **处理位置** | 总是在 EL3 处理 | **可配置**：可以委托给 S-EL1 或由 EL3 处理（取决于路由模型） |
| **典型用途** | 安全看门狗、平台安全中断、系统级安全事件 | 安全定时器、Secure Payload 特定中断 |
| **上下文切换** | 不需要（已在 EL3） | 可能需要（从 NS 到 Secure） |
| **Handler 注册** | 注册为 `INTR_TYPE_EL3` handler | 注册为 `INTR_TYPE_S_EL1` handler |

**关键区别**:
- **Group 0**: 是"系统级"安全中断，**必须**由 EL3 直接处理，**不能**委托给 S-EL1。这确保了关键安全功能（如看门狗）始终由最高特权级别处理。
- **Group 1S**: 是"应用级"安全中断，**可以选择**：
  - **路由模型 RM0**: 在 Secure 世界时委托给 S-EL1（如 TSP）处理，在 NS 世界时由 EL3 处理
  - **路由模型 RM1**: 无论从哪个世界都由 EL3 直接处理（不委托给 S-EL1）
  - 这提供了更灵活的安全服务架构，可以根据需求选择处理方式

**代码实现区别**:

```c
// 从 plat_gicv3.c
uint32_t plat_ic_get_interrupt_type(uint32_t id)
{
    unsigned int group;
    group = gicv3_get_interrupt_group(id, plat_my_core_pos());
    
    switch (group) {
    case INTR_GROUP0:      // Group 0 → INTR_TYPE_EL3
        return INTR_TYPE_EL3;
    case INTR_GROUP1S:     // Group 1S → INTR_TYPE_S_EL1
        return INTR_TYPE_S_EL1;
    case INTR_GROUP1NS:    // Group 1NS → INTR_TYPE_NS
        return INTR_TYPE_NS;
    }
}
```

**Handler 注册示例**:

```c
// Group 0 中断 handler (在 SPMD 中)
register_interrupt_type_handler(INTR_TYPE_EL3,
                                spmd_group0_interrupt_handler_nwd,
                                flags);
// Group 0 handler 在 EL3 直接处理，不委托给 S-EL1

// Group 1S 中断 handler (在 TSPD 中)
register_interrupt_type_handler(INTR_TYPE_S_EL1,
                                tspd_sel1_interrupt_handler,
                                flags);
// Group 1S handler 会切换上下文并跳转到 S-EL1 处理
```

**实际使用场景对比**:

**场景 A: Group 0 中断处理**
```
NS-EL1 执行中
    ↓
Group 0 中断发生（例如：安全看门狗）
    ↓
硬件路由到 EL3
    ↓
EL3 异常向量 → handle_interrupt_exception()
    ↓
plat_ic_get_pending_interrupt_type() → INTR_TYPE_EL3
    ↓
get_interrupt_type_handler() → spmd_group0_interrupt_handler_nwd()
    ↓
Handler 在 EL3 直接处理中断
    ↓
处理完成，返回 NS-EL1
```
**特点**: 整个处理过程都在 EL3，不需要上下文切换到 S-EL1。

**场景 B: Group 1S 中断处理**
```
NS-EL1 执行中
    ↓
Group 1S 中断发生（例如：安全定时器）
    ↓
硬件路由到 EL3
    ↓
EL3 异常向量 → handle_interrupt_exception()
    ↓
plat_ic_get_pending_interrupt_type() → INTR_TYPE_S_EL1
    ↓
get_interrupt_type_handler() → tspd_sel1_interrupt_handler()
    ↓
Handler 保存 NS 上下文，恢复 Secure 上下文
    ↓
设置 ELR_EL3 指向 TSP 中断入口
    ↓
ERET 跳转到 S-EL1
    ↓
TSP 在 S-EL1 处理中断
    ↓
处理完成，返回 EL3，再返回 NS-EL1
```
**特点**: 需要上下文切换，委托给 S-EL1 的 Secure Payload 处理。

**场景 C: Group 1S 中断处理（路由模型 RM1 - EL3 直接处理）**
```
NS-EL1 或 S-EL1 执行中
    ↓
Group 1S 中断发生
    ↓
硬件路由到 EL3（路由模型 RM1）
    ↓
EL3 异常向量 → handle_interrupt_exception()
    ↓
plat_ic_get_pending_interrupt_type() → INTR_TYPE_S_EL1
    ↓
get_interrupt_type_handler() → EL3 的 S-EL1 中断 handler
    ↓
Handler 在 EL3 直接处理中断
    ↓
处理完成，返回原上下文
```
**特点**: 整个处理过程都在 EL3，**不委托**给 S-EL1，无需上下文切换。

**路由模型选择**:
- **RM0**: 适合有 Secure Payload（如 TSP）的场景，可以委托处理
- **RM1**: 适合没有 Secure Payload 或需要 EL3 统一管理的场景

**Group 1S 中断路由模型对比**:

| 路由模型 | 从 NS 世界 | 从 Secure 世界 | 处理位置 | 适用场景 |
|---------|-----------|---------------|---------|---------|
| **RM0** | 路由到 EL3 | 路由到 S-EL1 | S-EL1（Secure 时）<br>EL3（NS 时） | 有 Secure Payload<br>（如 TSP） |
| **RM1** | 路由到 EL3 | 路由到 EL3 | EL3（总是） | 无 Secure Payload<br>或需要 EL3 统一管理 |

**关键点**:
- Group 1S 中断**不是必须**路由到 EL1
- 可以通过路由模型选择处理方式
- RM0 提供灵活性，允许委托给 Secure Payload
- RM1 提供统一性，所有处理都在 EL3

---

## 中断分组与 FIQ/IRQ 详解

### 中断分组（Interrupt Grouping）的本质

中断分组是 ARM 架构中用于区分中断安全属性和处理方式的核心机制。它通过 GIC（Generic Interrupt Controller）的配置寄存器来实现。

#### 为什么需要中断分组？

1. **安全隔离**: 确保安全世界（Secure World）和非安全世界（Non-secure World）的中断不会相互干扰
2. **权限控制**: 不同特权级别（EL1/EL2/EL3）可以处理不同安全级别的中断
3. **灵活路由**: 允许系统根据安全状态和路由模型动态决定中断处理位置

#### 中断分组的三个层次

```
中断分组层次结构:
├─ Group 0 (G0)
│  └─ 安全中断，总是路由到 EL3
│     └─ 典型用途: 安全看门狗、平台安全中断
│
├─ Group 1 Secure (G1S)
│  └─ 安全 Group 1 中断，可路由到 S-EL1 或 EL3
│     └─ 典型用途: 安全定时器、Secure Payload 中断
│
└─ Group 1 Non-secure (G1NS)
   └─ 非安全 Group 1 中断，可路由到 NS-EL1/EL2 或 EL3
      └─ 典型用途: 普通外设中断、非安全定时器
```

### FIQ 和 IRQ 的本质

#### FIQ/IRQ 是什么？

**FIQ (Fast Interrupt Request)** 和 **IRQ (Interrupt Request)** 是 ARM 处理器的**两条物理中断信号线**：

1. **硬件层面**: 它们是 CPU 与中断控制器（GIC）之间的物理连接
2. **信号层面**: 当 GIC 检测到中断时，会通过 IRQ 或 FIQ 信号线通知 CPU
3. **异常层面**: CPU 根据信号线类型（IRQ 或 FIQ）进入不同的异常向量

#### ARM 异常向量表

ARMv8-A 架构定义了不同的异常向量，IRQ 和 FIQ 使用不同的向量：

```
异常向量表 (AArch64):
├─ 同步异常 (Synchronous)
├─ IRQ 异常 (IRQ)          ← IRQ 信号线触发
├─ FIQ 异常 (FIQ)          ← FIQ 信号线触发
└─ SError 异常 (SError)
```

#### 为什么需要两条信号线？

**设计原因**:
1. **优先级区分**: FIQ 通常具有更高的优先级，可以抢占 IRQ 处理
2. **安全隔离**: 不同安全属性的中断使用不同信号线，便于硬件自动路由
3. **灵活配置**: 允许系统根据安全状态动态选择使用哪条信号线

### 中断分组与 FIQ/IRQ 的映射关系

#### 核心映射规则（GICv3）

不同的中断分组和当前安全状态决定了使用 IRQ 还是 FIQ 信号线：

| 中断类型 | 当前安全状态 | 使用的信号线 | SCR_EL3 控制位 | 说明 |
|---------|------------|------------|---------------|------|
| **Group 0 (EL3)** | Secure | **FIQ** | **SCR_EL3.FIQ** | 安全中断，总是 EL3 |
| **Group 0 (EL3)** | Non-secure | **FIQ** | **SCR_EL3.FIQ** | 安全中断，总是 EL3 |
| **Group 1S (S-EL1)** | Secure | **IRQ** | **SCR_EL3.IRQ** | 可委托给 S-EL1 |
| **Group 1S (S-EL1)** | Non-secure | **FIQ** | **SCR_EL3.FIQ** | 从 NS 路由到 EL3 |
| **Group 1NS (NS)** | Secure | **FIQ** | **SCR_EL3.FIQ** | 从 Secure 路由到 EL3 |
| **Group 1NS (NS)** | Non-secure | **IRQ** | **SCR_EL3.IRQ** | 可路由到 NS-EL1/EL2 |

#### 映射规则的代码实现

```c
// plat_gicv3.c: plat_interrupt_type_to_line()
uint32_t plat_interrupt_type_to_line(uint32_t type,
                                     uint32_t security_state)
{
    switch (type) {
    case INTR_TYPE_S_EL1:  // Group 1S 中断
        /*
         * S-EL1 中断的信号线映射:
         * - 从 Secure 世界: 使用 IRQ 信号线 (SCR_EL3.IRQ 控制)
         * - 从 NS 世界: 使用 FIQ 信号线 (SCR_EL3.FIQ 控制)
         * 
         * 原因: 当在 Secure 世界时，如果路由到 S-EL1，使用 IRQ 线
         *       当在 NS 世界时，必须路由到 EL3，使用 FIQ 线
         */
        if (security_state == SECURE) {
            return __builtin_ctz(SCR_IRQ_BIT);  // bit 1
        } else {
            return __builtin_ctz(SCR_FIQ_BIT);  // bit 2
        }
        
    case INTR_TYPE_NS:  // Group 1NS 中断
        /*
         * NS 中断的信号线映射:
         * - 从 Secure 世界: 使用 FIQ 信号线 (SCR_EL3.FIQ 控制)
         * - 从 NS 世界: 使用 IRQ 信号线 (SCR_EL3.IRQ 控制)
         * 
         * 原因: 当在 Secure 世界时，必须路由到 EL3，使用 FIQ 线
         *       当在 NS 世界时，如果路由到 NS-EL1/EL2，使用 IRQ 线
         */
        if (security_state == SECURE) {
            return __builtin_ctz(SCR_FIQ_BIT);  // bit 2
        } else {
            return __builtin_ctz(SCR_IRQ_BIT);  // bit 1
        }
        
    case INTR_TYPE_EL3:  // Group 0 中断
        /*
         * EL3 中断的信号线映射:
         * - 无论从哪个安全状态: 总是使用 FIQ 信号线
         * 
         * 原因: EL3 中断总是路由到 EL3，使用 FIQ 线统一处理
         */
        return __builtin_ctz(SCR_FIQ_BIT);  // bit 2
    }
}
```

### 为什么这样映射？

#### 设计原理

**核心思想**: **不同安全状态下的中断使用不同的信号线，便于硬件自动路由**

1. **从 Secure 世界发生的中断**:
   - **S-EL1 中断 (Group 1S)**: 使用 **IRQ** 线
     - 如果 SCR_EL3.IRQ = 0 → 路由到 S-EL1（委托处理）
     - 如果 SCR_EL3.IRQ = 1 → 路由到 EL3（EL3 处理）
   - **NS 中断 (Group 1NS)**: 使用 **FIQ** 线
     - 必须路由到 EL3（SCR_EL3.FIQ 通常 = 1）
     - 因为 Secure 世界不能直接处理 NS 中断

2. **从 NS 世界发生的中断**:
   - **S-EL1 中断 (Group 1S)**: 使用 **FIQ** 线
     - 必须路由到 EL3（SCR_EL3.FIQ 通常 = 1）
     - 因为 NS 世界不能直接处理 Secure 中断
   - **NS 中断 (Group 1NS)**: 使用 **IRQ** 线
     - 如果 SCR_EL3.IRQ = 0 → 路由到 NS-EL1/EL2（直接处理）
     - 如果 SCR_EL3.IRQ = 1 → 路由到 EL3（EL3 处理）

3. **EL3 中断 (Group 0)**: 总是使用 **FIQ** 线
   - 总是路由到 EL3（SCR_EL3.FIQ 通常 = 1）

#### 映射规则总结表

```
中断分组 → 信号线映射规则:

┌─────────────────────────────────────────────────────────┐
│ 当前在 Secure 世界 (S-EL0/1)                            │
├─────────────────────────────────────────────────────────┤
│ Group 0 (EL3)    → FIQ 线 → SCR_EL3.FIQ 控制 → 总是 EL3 │
│ Group 1S (S-EL1) → IRQ 线 → SCR_EL3.IRQ 控制 → S-EL1/EL3│
│ Group 1NS (NS)   → FIQ 线 → SCR_EL3.FIQ 控制 → 总是 EL3 │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ 当前在 NS 世界 (NS-EL0/1/2)                             │
├─────────────────────────────────────────────────────────┤
│ Group 0 (EL3)    → FIQ 线 → SCR_EL3.FIQ 控制 → 总是 EL3 │
│ Group 1S (S-EL1) → FIQ 线 → SCR_EL3.FIQ 控制 → 总是 EL3 │
│ Group 1NS (NS)   → IRQ 线 → SCR_EL3.IRQ 控制 → NS-EL1/2/EL3│
└─────────────────────────────────────────────────────────┘
```

### 中断分组、信号线、路由模型的完整关系

#### 三层关系链

```
第一层: 中断分组 (GIC 配置)
    ↓ 决定中断的安全属性
第二层: 信号线选择 (IRQ/FIQ)
    ↓ 根据当前安全状态和中断类型
第三层: 路由控制 (SCR_EL3.IRQ/FIQ)
    ↓ 根据路由模型 (RM0/RM1)
最终: 硬件路由行为
```

#### 完整示例：Group 1S 中断的处理流程

**场景**: NS-EL1 执行时发生 Group 1S 中断

```
1. GIC 检测到 Group 1S 中断
   ↓
2. GIC 根据当前安全状态 (NS) 和中断类型 (S-EL1)
   选择使用 FIQ 信号线
   ↓
3. CPU 收到 FIQ 信号，检查 SCR_EL3.FIQ 位
   ↓
4. 如果 SCR_EL3.FIQ = 1 (路由模型 RM0/RM1)
   → 路由到 EL3
   → 进入 EL3 的 FIQ 异常向量
   ↓
5. EL3 异常处理:
   → handle_interrupt_exception()
   → plat_ic_get_pending_interrupt_type() → INTR_TYPE_S_EL1
   → get_interrupt_type_handler() → tspd_sel1_interrupt_handler()
   ↓
6. Handler 处理:
   → 保存 NS 上下文
   → 恢复 Secure 上下文
   → ERET 跳转到 S-EL1 (TSP)
   ↓
7. TSP 在 S-EL1 处理中断
```

**关键点**:
- **中断分组** (Group 1S) → 决定中断类型为 `INTR_TYPE_S_EL1`
- **信号线选择** (FIQ) → 因为从 NS 世界发生
- **路由控制** (SCR_EL3.FIQ = 1) → 路由到 EL3
- **Handler 选择** → 根据中断类型选择对应的 handler

### GICv2 与 GICv3 的区别

#### GICv2 的信号线映射

GICv2 的映射规则更简单：

```c
// plat_gicv2.c: plat_interrupt_type_to_line()
uint32_t plat_interrupt_type_to_line(uint32_t type,
                                     uint32_t security_state)
{
    /* NS 中断总是使用 IRQ 线 */
    if (type == INTR_TYPE_NS) {
        return __builtin_ctz(SCR_IRQ_BIT);
    }
    
    /*
     * Secure 中断:
     * - 如果 FIQ 使能: 使用 FIQ 线
     * - 如果 FIQ 未使能: 使用 IRQ 线
     */
    return ((gicv2_is_fiq_enabled() != 0U) ? 
            __builtin_ctz(SCR_FIQ_BIT) : 
            __builtin_ctz(SCR_IRQ_BIT));
}
```

**GICv2 特点**:
- NS 中断总是使用 IRQ 线（与安全状态无关）
- Secure 中断根据 FIQ 是否使能来选择信号线
- 映射规则比 GICv3 简单，但灵活性较低

#### GICv3 的优势

1. **更精确的控制**: 根据安全状态动态选择信号线
2. **更好的隔离**: 不同安全状态使用不同信号线，便于硬件区分
3. **支持 Group 0**: GICv3 原生支持 Group 0 中断（EL3 中断）

### 实际硬件行为

#### 中断发生的硬件流程

```
1. 外设触发中断
   ↓
2. GIC 接收中断，根据 GICD_IGROUPR 和 GICD_IGRPMODR
   确定中断分组 (Group 0/1S/1NS)
   ↓
3. GIC 根据中断分组和当前 CPU 安全状态
   选择 IRQ 或 FIQ 信号线
   ↓
4. GIC 通过选定的信号线通知 CPU
   ↓
5. CPU 检查对应的 SCR_EL3 位:
   - IRQ 信号 → 检查 SCR_EL3.IRQ
   - FIQ 信号 → 检查 SCR_EL3.FIQ
   ↓
6. 如果对应位 = 1:
   → 路由到 EL3
   → 进入 EL3 异常向量
   ↓
7. 如果对应位 = 0:
   → 路由到 FEL (First Exception Level)
   → 进入对应异常级别的异常向量
```

#### SCR_EL3 位的实际作用

**SCR_EL3.IRQ (bit 1)**:
- `= 0`: IRQ 信号路由到 FEL（当前安全状态下能处理中断的最低异常级别）
- `= 1`: IRQ 信号路由到 EL3

**SCR_EL3.FIQ (bit 2)**:
- `= 0`: FIQ 信号路由到 FEL
- `= 1`: FIQ 信号路由到 EL3

**关键**: 这两个位是**硬件控制位**，CPU 硬件根据这些位的值自动决定中断路由目标。

### 总结：中断分组与 FIQ/IRQ 的关系

1. **中断分组** (Group 0/1S/1NS):
   - 由 GIC 配置寄存器决定
   - 定义中断的安全属性
   - 决定中断类型 (`INTR_TYPE_EL3/S_EL1/NS`)

2. **信号线选择** (IRQ/FIQ):
   - 由中断类型和当前安全状态决定
   - 通过 `plat_interrupt_type_to_line()` 函数确定
   - 决定使用 SCR_EL3 的哪个位（IRQ 或 FIQ）

3. **路由控制** (SCR_EL3.IRQ/FIQ):
   - 由路由模型 (RM0/RM1) 决定
   - 控制中断是否路由到 EL3
   - 最终写入硬件寄存器 SCR_EL3

4. **硬件路由**:
   - CPU 硬件根据 SCR_EL3 位的值自动路由
   - 决定中断最终到达哪个异常级别

**完整关系链**:
```
中断分组 → 信号线选择 → 路由控制 → 硬件路由
(GIC配置)  (IRQ/FIQ)   (SCR_EL3)  (CPU硬件)
```

---

## GIC 中断分组

### GIC 寄存器配置

GIC 使用以下寄存器来配置中断分组：

#### 1. GICD_IGROUPR (Interrupt Group Register)
- **作用**: 将中断配置为 Group 0 或 Group 1
- **位定义**:
  - `0`: Group 0 (安全中断)
  - `1`: Group 1 (可配置为安全或非安全)

#### 2. GICD_IGRPMODR (Interrupt Group Modifier Register) - GICv2 特有
- **作用**: 将 Group 1 中断进一步细分为 Group 1S 或 Group 1NS
- **位定义**:
  - `0`: Group 1NS (非安全)
  - `1`: Group 1S (安全)

#### 3. GICD_CTLR (Distributor Control Register)
- **作用**: 全局启用/禁用不同组的中断
- **关键位**:
  - `EnableGrp0`: 启用 Group 0 中断
  - `EnableGrp1NS`: 启用 Group 1NS 中断
  - `EnableGrp1S`: 启用 Group 1S 中断

### 中断分组配置示例

```c
// 配置中断 ID 32 为 Group 0 (安全中断)
gicd_set_igroupr(gicd_base, 32, IGROUP);

// 配置中断 ID 33 为 Group 1NS (非安全中断)
gicd_set_igroupr(gicd_base, 33, IGROUP);
gicd_set_igrpmodr(gicd_base, 33, IGRPMOD);  // GICv2 only

// 配置中断 ID 34 为 Group 1S (安全 Group 1 中断)
gicd_set_igroupr(gicd_base, 34, IGROUP);
gicd_set_igrpmodr(gicd_base, 34, 0);  // GICv2 only
```

---

## 中断路由模型

中断路由模型决定了中断在哪个异常级别（EL）被处理。TF-A 支持两种路由模式：

### 路由模式定义

```c
// 从 interrupt_mgmt.h
#define INTR_ROUTING_MODE_PE    0  // Platform-specific routing
#define INTR_ROUTING_MODE_ANY   1  // Any EL can handle
```

### 中断类型与路由模型

#### 1. S-EL1 中断 (Secure EL1 Interrupts)
- **INTR_SEL1_VALID_RM0** (0x2 = 0b10): 
  - 从 NS 世界：路由到 EL3 (BIT[1] = 1)
  - 从 Secure 世界：路由到 S-EL1 (BIT[0] = 0，委托给 Secure Payload 处理)
- **INTR_SEL1_VALID_RM1** (0x3 = 0b11): 
  - 从 NS 和 Secure 都路由到 EL3 (BIT[0] = 1, BIT[1] = 1，**在 EL3 直接处理，不委托给 S-EL1**)

**重要**: Group 1S 中断**不是必须**路由到 EL1！可以选择：
- **RM0**: 在 Secure 世界时委托给 S-EL1 处理，在 NS 世界时由 EL3 处理
- **RM1**: 无论从哪个世界都由 EL3 直接处理（不委托给 S-EL1）

### 路由模型的决定因素

路由模型（RM0/RM1）是由**代码在注册中断 handler 时通过 `flags` 参数设置的**。

#### Flags 参数结构

```c
// flags 参数的结构（从 interrupt_mgmt.h）
// BIT[0]: 从 Secure 世界的路由模型 (INTR_RM_FROM_SEC_SHIFT)
//         0 = 路由到当前 EL (S-EL1)，1 = 路由到 EL3
// BIT[1]: 从 NS 世界的路由模型 (INTR_RM_FROM_NS_SHIFT)
//         0 = 路由到 FEL (First Exception Level，即当前执行的 EL)
//         1 = 路由到 EL3
//         
//         FEL 的含义：
//         - 如果当前在 NS-EL1 执行，FEL = NS-EL1
//         - 如果当前在 NS-EL2 执行，FEL = NS-EL2
//         - 硬件自动根据当前执行级别决定路由目标
```

#### 路由模型设置示例

**示例 1: TSPD 设置 RM0** (`tspd_main.c`):
```c
flags = 0;
set_interrupt_rm_flag(flags, NON_SECURE);  // 设置 BIT[1] = 1
// 结果: flags = 0x2 = RM0
// BIT[0] = 0 (Secure → S-EL1)
// BIT[1] = 1 (NS → EL3)

register_interrupt_type_handler(INTR_TYPE_S_EL1,
                                tspd_sel1_interrupt_handler,
                                flags);
```

**示例 2: 设置 RM1** (假设代码):
```c
flags = 0;
set_interrupt_rm_flag(flags, SECURE);      // 设置 BIT[0] = 1
set_interrupt_rm_flag(flags, NON_SECURE);  // 设置 BIT[1] = 1
// 结果: flags = 0x3 = RM1
// BIT[0] = 1 (Secure → EL3)
// BIT[1] = 1 (NS → EL3)

register_interrupt_type_handler(INTR_TYPE_S_EL1,
                                el3_sel1_interrupt_handler,
                                flags);
```

#### 路由模型的决定因素

1. **SPD (Secure Payload Dispatcher) 的选择**:
   - 如果有 Secure Payload（如 TSP），通常选择 **RM0**，允许委托给 S-EL1
   - 如果没有 Secure Payload 或需要统一管理，选择 **RM1**，由 EL3 处理

2. **系统架构需求**:
   - **RM0**: 适合需要 Secure Payload 处理特定中断的场景（如 TSP 处理定时器中断）
   - **RM1**: 适合需要 EL3 统一管理所有安全中断的场景

3. **代码配置**:
   - 路由模型在 SPD 初始化时通过 `register_interrupt_type_handler()` 设置
   - 不同的 SPD（TSPD、SPMD、OP-TEE 等）可能有不同的选择

#### 路由模型 Flags 编码

| 路由模型 | Flags 值 | BIT[1] (NS) | BIT[0] (Secure) | 说明 |
|---------|---------|-------------|----------------|------|
| **RM0** | 0x2 (0b10) | 1 (→ EL3) | 0 (→ S-EL1) | NS 路由到 EL3，Secure 路由到 S-EL1 |
| **RM1** | 0x3 (0b11) | 1 (→ EL3) | 1 (→ EL3) | NS 和 Secure 都路由到 EL3 |

#### 实际代码示例对比

**TSPD 使用 RM0** (有 Secure Payload):
```c
// tspd_main.c
flags = 0;
set_interrupt_rm_flag(flags, NON_SECURE);  // 只设置 NS 位
// flags = 0x2 = RM0
register_interrupt_type_handler(INTR_TYPE_S_EL1,
                                tspd_sel1_interrupt_handler,  // 委托给 TSP
                                flags);
```

**如果使用 RM1** (无 Secure Payload 或需要 EL3 统一管理):
```c
flags = 0;
set_interrupt_rm_flag(flags, SECURE);      // 设置 Secure 位
set_interrupt_rm_flag(flags, NON_SECURE);  // 设置 NS 位
// flags = 0x3 = RM1
register_interrupt_type_handler(INTR_TYPE_S_EL1,
                                el3_sel1_interrupt_handler,  // EL3 直接处理
                                flags);
```

**总结**: 路由模型由**代码在注册 handler 时决定**，具体取决于：
- 是否有 Secure Payload（如 TSP）
- 系统架构需求（是否需要委托处理）
- SPD 的设计选择

### FIQ 和 IRQ 与路由模型的关系

**重要澄清**: **FIQ 和 IRQ 不是路由模型**，它们是：
1. **硬件信号线**: ARM 处理器有两条中断信号线（IRQ 和 FIQ）
2. **SCR_EL3 的控制位**: SCR_EL3.IRQ 和 SCR_EL3.FIQ 控制中断是否路由到 EL3
3. **中断信号类型**: 不同的中断类型通过不同的信号线传递

**路由模型是软件概念**:
- 路由模型决定中断路由到哪个异常级别（EL1/EL2/EL3）
- 路由模型通过设置 SCR_EL3.IRQ 或 SCR_EL3.FIQ 位来实现
- 不同的中断类型使用不同的信号线（IRQ 或 FIQ）

**关系总结**:
```
路由模型 (软件概念)
    ↓ 决定
SCR_EL3.IRQ/FIQ 位的值 (硬件寄存器位)
    ↓ 控制
硬件信号线 (IRQ/FIQ) 的路由行为
```

### 路由模型如何映射到硬件寄存器

路由模型（RM0/RM1）**最终写入 `SCR_EL3` 寄存器**的 IRQ 或 FIQ 位。

#### 映射过程

```
路由模型 (flags)
    ↓
set_routing_model() 
    ↓
set_scr_el3_from_rm()
    ↓
plat_interrupt_type_to_line() → 决定使用 IRQ 还是 FIQ 位
    ↓
cm_write_scr_el3_bit() → 写入 SCR_EL3 寄存器
```

#### 代码实现流程

**步骤 1: 注册 handler 时设置路由模型** (`interrupt_mgmt.c`):
```c
register_interrupt_type_handler(INTR_TYPE_S_EL1,
                                handler,
                                flags);  // flags = RM0 或 RM1
    ↓
set_routing_model(type, flags)
    ↓
set_scr_el3_from_rm(type, flags, SECURE)
set_scr_el3_from_rm(type, flags, NON_SECURE)
```

**步骤 2: 映射到 SCR_EL3 位** (`interrupt_mgmt.c:101-119`):
```c
static void set_scr_el3_from_rm(uint32_t type,
                                uint32_t interrupt_type_flags,
                                uint32_t security_state)
{
    // 1. 从 flags 中提取路由模型位 (0 或 1)
    flag = get_interrupt_rm_flag(interrupt_type_flags, security_state);
    
    // 2. 决定使用 SCR_EL3 的哪个位 (IRQ 或 FIQ)
    bit_pos = plat_interrupt_type_to_line(type, security_state);
    
    // 3. 保存到内部数据结构
    intr_type_descs[type].scr_el3[security_state] = flag << bit_pos;
    
    // 4. 写入硬件寄存器 SCR_EL3
    if (cm_get_context(security_state) != NULL) {
        cm_write_scr_el3_bit(security_state, bit_pos, flag);
    }
}
```

**步骤 3: 决定使用 IRQ 还是 FIQ 位** (`plat_gicv3.c:156-198`):
```c
uint32_t plat_interrupt_type_to_line(uint32_t type,
                                     uint32_t security_state)
{
    switch (type) {
    case INTR_TYPE_S_EL1:
        // S-EL1 中断:
        // - 从 Secure 世界: 使用 IRQ 位
        // - 从 NS 世界: 使用 FIQ 位
        if (security_state == SECURE) {
            return __builtin_ctz(SCR_IRQ_BIT);  // bit 1
        } else {
            return __builtin_ctz(SCR_FIQ_BIT);  // bit 2
        }
        
    case INTR_TYPE_NS:
        // NS 中断:
        // - 从 Secure 世界: 使用 FIQ 位
        // - 从 NS 世界: 使用 IRQ 位
        if (security_state == SECURE) {
            return __builtin_ctz(SCR_FIQ_BIT);  // bit 2
        } else {
            return __builtin_ctz(SCR_IRQ_BIT);  // bit 1
        }
        
    case INTR_TYPE_EL3:
        // EL3 中断: 总是使用 FIQ 位
        return __builtin_ctz(SCR_FIQ_BIT);  // bit 2
    }
}
```

**步骤 4: 写入 SCR_EL3 寄存器** (`context_mgmt.c:2013`):
```c
void cm_write_scr_el3_bit(uint32_t security_state,
                         uint32_t bit_pos,
                         uint32_t value)
{
    cpu_context_t *ctx = cm_get_context(security_state);
    u_register_t scr_el3;
    
    scr_el3 = read_ctx_reg(get_el3state_ctx(ctx), CTX_SCR_EL3);
    
    if (value == 1) {
        scr_el3 |= (1ULL << bit_pos);  // 设置位
    } else {
        scr_el3 &= ~(1ULL << bit_pos); // 清除位
    }
    
    write_ctx_reg(get_el3state_ctx(ctx), CTX_SCR_EL3, scr_el3);
    // 最终在上下文切换时，这个值会被写入实际的 SCR_EL3 寄存器
}
```

#### 路由模型到硬件寄存器的映射表

| 路由模型 | Flags 值 | BIT[1] (NS) | BIT[0] (Secure) | 写入的寄存器位 | 说明 |
|---------|---------|-------------|----------------|---------------|------|
| **S-EL1 RM0** | 0x2 | 1 | 0 | **SCR_EL3.FIQ** (NS)<br>**SCR_EL3.IRQ** (Secure) | NS→EL3, Secure→S-EL1 |
| **S-EL1 RM1** | 0x3 | 1 | 1 | **SCR_EL3.FIQ** (NS)<br>**SCR_EL3.IRQ** (Secure) | NS→EL3, Secure→EL3 |
| **NS RM0** | 0x0 | 0 | 0 | **SCR_EL3.IRQ** (NS)<br>**SCR_EL3.FIQ** (Secure) | NS→NS-EL1, Secure→S-EL1 |
| **NS RM1** | 0x1 | 0 | 1 | **SCR_EL3.IRQ** (NS)<br>**SCR_EL3.FIQ** (Secure) | NS→NS-EL1, Secure→EL3 |

#### 关键点

1. **路由模型不是直接写入硬件寄存器**，而是通过软件逻辑映射到 `SCR_EL3` 的 IRQ/FIQ 位
2. **SCR_EL3 是实际的硬件寄存器**，控制中断是否路由到 EL3
3. **不同的中断类型使用不同的位**:
   - S-EL1 中断: Secure 用 IRQ 位，NS 用 FIQ 位
   - NS 中断: Secure 用 FIQ 位，NS 用 IRQ 位
   - EL3 中断: 总是用 FIQ 位
4. **写入时机**: 在上下文切换时，SCR_EL3 的值会被写入实际的硬件寄存器

#### 硬件寄存器写入时机

**上下文切换时写入** (`context.S:641-647`):
```assembly
func el3_exit
    // 从上下文恢复 SCR_EL3
    ldp x16, x17, [sp, #CTX_EL3STATE_OFFSET + CTX_SPSR_EL3]
    ldr x18, [sp, #CTX_EL3STATE_OFFSET + CTX_SCR_EL3]
    ...
    msr scr_el3, x18  // 写入硬件寄存器 SCR_EL3
    ...
    eret              // 返回到目标异常级别
endfunc
```

**总结**:
- **路由模型 (RM0/RM1)** → 通过 `flags` 参数设置
- **映射到 SCR_EL3** → `set_scr_el3_from_rm()` 函数
- **决定使用哪个位** → `plat_interrupt_type_to_line()` 函数
- **写入硬件寄存器** → 在上下文切换时通过 `msr scr_el3, x18` 指令

**关键硬件寄存器**: **SCR_EL3**
- **SCR_EL3.IRQ (bit 1)**: 控制 IRQ 中断是否路由到 EL3
- **SCR_EL3.FIQ (bit 2)**: 控制 FIQ 中断是否路由到 EL3
- **路由模型最终体现在这两个位的值上**

#### FIQ/IRQ 与路由模型的区别和关系

**重要澄清**: **FIQ 和 IRQ 不是路由模型**！

| 概念 | 性质 | 说明 | 示例 |
|------|------|------|------|
| **FIQ/IRQ** | 硬件信号线 | ARM 处理器的物理中断信号线 | IRQ 线、FIQ 线 |
| **SCR_EL3.IRQ/FIQ** | 硬件寄存器位 | 控制中断路由的寄存器位 | bit 1 (IRQ), bit 2 (FIQ) |
| **路由模型 (RM0/RM1)** | 软件配置 | 决定中断路由行为的软件逻辑 | RM0, RM1 |

**关系链**:
```
路由模型 (软件概念)
    ↓ 决定
SCR_EL3.IRQ/FIQ 位的值 (硬件寄存器位)
    ↓ 控制
FIQ/IRQ 信号线的路由行为 (硬件物理信号)
```

**为什么需要区分 IRQ 和 FIQ？**

不同的中断类型使用不同的信号线，这样硬件可以根据信号线类型自动决定路由：
- **S-EL1 中断从 Secure**: 使用 IRQ 信号线 → SCR_EL3.IRQ 控制
- **S-EL1 中断从 NS**: 使用 FIQ 信号线 → SCR_EL3.FIQ 控制
- **NS 中断从 Secure**: 使用 FIQ 信号线 → SCR_EL3.FIQ 控制
- **NS 中断从 NS**: 使用 IRQ 信号线 → SCR_EL3.IRQ 控制

**总结**:
- **FIQ/IRQ**: 硬件信号线，不是路由模型
- **路由模型**: 软件配置，通过设置 SCR_EL3.IRQ/FIQ 位来实现
- **关系**: 路由模型决定 SCR_EL3.IRQ/FIQ 位的值，这些位控制 FIQ/IRQ 信号线的路由行为

#### 2. NS 中断 (Non-secure Interrupts)
- **INTR_NS_VALID_RM0** (0x0 = 0b00): 
  - 从 NS 世界：路由到 EL1/EL2 (BIT[1] = 0, SCR_EL3.IRQ = 0，**直接由 NS-EL1/EL2 处理**)
  - 从 Secure 世界：路由到 S-EL1 (BIT[0] = 0)
- **INTR_NS_VALID_RM1** (0x1 = 0b01): 
  - 从 NS 世界：路由到 EL1/EL2 (BIT[1] = 0, SCR_EL3.IRQ = 0，**直接由 NS-EL1/EL2 处理**)
  - 从 Secure 世界：路由到 EL3 (BIT[0] = 1, SCR_EL3.IRQ = 1)

**重要**: 
- **从 NS-EL1 执行时，Group 1NS 中断通常直接路由到 NS-EL1**（SCR_EL3.IRQ = 0）
- **路由到 EL3 的情况**：只有在特殊配置下，显式设置 SCR_EL3.IRQ = 1 时才会路由到 EL3
- 默认路由模型（RM0）和 RM1 都让 NS 中断从 NS 世界直接路由到 NS-EL1/EL2

#### 3. EL3 中断
- **INTR_EL3_VALID_RM0**: 从 NS 路由到 EL3，从 Secure 路由到 S-EL1 然后交给 EL3
- **INTR_EL3_VALID_RM1**: 从 NS 和 Secure 都路由到 EL3

---

## SCR_EL3 和 HCR_EL2 寄存器控制

### SCR_EL3 寄存器

`SCR_EL3` (Secure Configuration Register) 控制中断路由和系统安全配置。

### 关键位定义

```c
// SCR_EL3 关键位
#define SCR_IRQ_BIT      (1ULL << 1)   // IRQ 路由到 EL3
#define SCR_FIQ_BIT      (1ULL << 2)    // FIQ 路由到 EL3
#define SCR_NS_BIT       (1ULL << 0)    // 非安全状态位
#define SCR_HCE_BIT      (1ULL << 8)    // HVC 调用使能
#define SCR_SMD_BIT      (1ULL << 7)    // SMC 禁用位
```

### SCR_EL3 配置逻辑

```c
// 从 context_mgmt.c
u_register_t get_scr_el3_from_routing_model(size_t security_state)
{
    u_register_t scr_el3 = 0;
    
    // 根据路由模型设置 IRQ/FIQ 路由位
    if (security_state == SECURE) {
        // Secure 世界的中断路由
        scr_el3 |= get_scr_el3_from_routing_model(SECURE);
    } else {
        // Non-secure 世界的中断路由
        scr_el3 |= SCR_NS_BIT;
        scr_el3 |= get_scr_el3_from_routing_model(NON_SECURE);
    }
    
    return scr_el3;
}
```

### 中断路由规则

| 当前状态 | 中断类型 | SCR_EL3.IRQ | SCR_EL3.FIQ | HCR_EL2.IMO/FMO | 路由目标 |
|---------|---------|-------------|-------------|-----------------|---------|
| NS-EL1  | Group 1NS | 0 | 0 | - | **NS-EL1** (FEL = EL1) |
| NS-EL1  | Group 1S  | 1 | 1 | - | EL3 |
| NS-EL1  | Group 0   | 1 | 1 | - | EL3 |
| **NS-EL2** | **Group 1NS** | **0** | **0** | **IMO=1** | **NS-EL2** (FEL = EL2) |
| **NS-EL2** | **Group 1NS** | **0** | **0** | **IMO=0** | **NS-EL1** (降级) |
| **NS-EL2** | **Group 1S** | **1** | **1** | **-** | **EL3** |
| **NS-EL2** | **Group 0** | **1** | **1** | **-** | **EL3** |
| S-EL1   | Group 1NS | 1 | 1 | - | EL3 |
| S-EL1   | Group 1S  | 0 | 0 | - | S-EL1 |
| S-EL1   | Group 0   | 1 | 1 | - | EL3 |
| EL3     | 所有类型   | - | - | - | EL3 |

**注意**: 
- **NS-EL2** 是 Hypervisor 级别，用于虚拟化
- **FEL (First Exception Level)**: 路由到 FEL 意味着路由到**当前执行的异常级别**
  - 当前在 NS-EL1 → FEL = NS-EL1 → 中断路由到 NS-EL1
  - 当前在 NS-EL2 → FEL = NS-EL2 → 中断路由到 NS-EL2（如果 HCR_EL2.IMO = 1）
- HCR_EL2.IMO (IRQ) 和 HCR_EL2.FMO (FIQ) 控制中断是否路由到 EL2
- 如果 HCR_EL2.IMO = 0，即使当前在 NS-EL2，中断也会降级路由到 NS-EL1

### HCR_EL2 寄存器

`HCR_EL2` (Hypervisor Configuration Register) 控制 EL2 的中断路由和虚拟化行为。

#### 关键位定义

```c
// HCR_EL2 关键位（中断相关）
#define HCR_IMO_BIT      (1ULL << 4)   // IRQ 路由到 EL2
#define HCR_FMO_BIT      (1ULL << 3)    // FIQ 路由到 EL2
#define HCR_AMO_BIT      (1ULL << 5)    // SError 路由到 EL2
#define HCR_RW_BIT       (1ULL << 31)   // EL2 执行状态（AArch64/AArch32）
```

#### HCR_EL2 中断路由控制

| HCR_EL2 位 | 作用 | 说明 |
|-----------|------|------|
| **IMO** (bit 4) | IRQ 路由到 EL2 | 当设置时，IRQ 中断路由到 EL2 而不是 EL1 |
| **FMO** (bit 3) | FIQ 路由到 EL2 | 当设置时，FIQ 中断路由到 EL2 而不是 EL1 |
| **AMO** (bit 5) | SError 路由到 EL2 | 当设置时，SError 异常路由到 EL2 |

#### EL2 中断路由条件

中断路由到 EL2 需要**同时满足**以下条件：

1. **当前执行在 NS-EL2**
2. **中断类型是 Group 1NS**（非安全中断）
3. **SCR_EL3.IRQ/FIQ = 0**（不路由到 EL3）
4. **HCR_EL2.IMO/FMO = 1**（允许路由到 EL2）

**重要**: 
- Group 0 和 Group 1S 中断**永远不会**路由到 EL2，因为它们有安全属性，必须由 EL3 处理
- 只有 Group 1NS 中断可以在满足条件时路由到 EL2

### EL2 中断路由说明

**EL2 (Hypervisor) 中断路由条件**:

1. **系统运行在 NS-EL2**: 当前执行在非安全 EL2（Hypervisor 模式）
2. **中断类型**: Group 1NS（非安全中断）
3. **SCR_EL3.IRQ/FIQ = 0**: 不路由到 EL3
4. **HCR_EL2 配置**: 
   - `HCR_EL2.IMO = 1`: IRQ 路由到 EL2
   - `HCR_EL2.FMO = 1`: FIQ 路由到 EL2
   - `HCR_EL2.AMO = 1`: SError 路由到 EL2

**关键点**:
- EL2 主要用于虚拟化场景（Hypervisor）
- 当系统运行在 NS-EL2 时，Group 1NS 中断可以直接路由到 EL2
- Group 0 和 Group 1S 中断仍然路由到 EL3（安全属性优先）
- EL2 可以管理虚拟机的中断，实现中断虚拟化

---

## ARM 异常向量表详解

### 异常向量表概述

ARMv8-A 架构中，每个异常级别（EL1、EL2、EL3）都有自己的异常向量表。每个向量表由 **VBAR_ELx** (Vector Base Address Register) 寄存器指定基地址。

**关键点**:
- **EL0**: 没有独立的异常向量表（不能处理异常）
- **EL1**: 使用 `VBAR_EL1` 指定向量表基地址
- **EL2**: 使用 `VBAR_EL2` 指定向量表基地址
- **EL3**: 使用 `VBAR_EL3` 指定向量表基地址

### 异常向量表的结构

每个异常向量表包含 **4 组**，每组包含 **4 个异常入口**：

```
异常向量表结构 (每个异常级别):

VBAR_ELx (基地址)
├─ 组 1: Current EL with SP_EL0      (偏移 0x000 - 0x200)
│  ├─ 同步异常 (Synchronous)         (偏移 0x000)
│  ├─ IRQ 异常                        (偏移 0x080)
│  ├─ FIQ 异常                        (偏移 0x100)
│  └─ SError 异常                     (偏移 0x180)
│
├─ 组 2: Current EL with SP_ELx      (偏移 0x200 - 0x400)
│  ├─ 同步异常 (Synchronous)         (偏移 0x200)
│  ├─ IRQ 异常                        (偏移 0x280)
│  ├─ FIQ 异常                        (偏移 0x300)
│  └─ SError 异常                     (偏移 0x380)
│
├─ 组 3: Lower EL using AArch64      (偏移 0x400 - 0x600)
│  ├─ 同步异常 (Synchronous)         (偏移 0x400)
│  ├─ IRQ 异常                        (偏移 0x480)
│  ├─ FIQ 异常                        (偏移 0x500)
│  └─ SError 异常                     (偏移 0x580)
│
└─ 组 4: Lower EL using AArch32      (偏移 0x600 - 0x800)
   ├─ 同步异常 (Synchronous)         (偏移 0x600)
   ├─ IRQ 异常                        (偏移 0x680)
   ├─ FIQ 异常                        (偏移 0x700)
   └─ SError 异常                     (偏移 0x780)
```

### 四组向量表的选择规则

CPU 硬件根据以下三个因素自动选择使用哪一组向量表：

1. **当前异常级别** (Current EL): 异常发生在哪个异常级别
2. **栈指针选择** (SP Selection): 当前使用的是 SP_EL0 还是 SP_ELx
3. **低异常级别的执行状态** (Lower EL Execution State): 如果从低异常级别进入，低异常级别是 AArch64 还是 AArch32

#### 选择决策表

| 组 | 偏移范围 | 选择条件 | 说明 |
|---|---------|---------|------|
| **组 1** | 0x000 - 0x200 | **Current EL with SP_EL0** | 在当前异常级别，使用 SP_EL0 作为栈指针时发生异常 |
| **组 2** | 0x200 - 0x400 | **Current EL with SP_ELx** | 在当前异常级别，使用 SP_ELx 作为栈指针时发生异常 |
| **组 3** | 0x400 - 0x600 | **Lower EL using AArch64** | 从低异常级别进入，低异常级别处于 AArch64 执行状态 |
| **组 4** | 0x600 - 0x800 | **Lower EL using AArch32** | 从低异常级别进入，低异常级别处于 AArch32 执行状态 |

### 详细选择规则

#### 1. Current EL with SP_EL0 (组 1: 0x000 - 0x200)

**使用条件**:
- 异常发生在**当前异常级别**（例如在 EL3 执行时发生异常）
- 当前使用 **SP_EL0** 作为栈指针（`SPSel = 0`）

**典型场景**:
- 在 EL3 执行时，使用 SP_EL0 作为栈指针
- 发生同步异常、IRQ、FIQ 或 SError

**代码示例** (TF-A):
```assembly
// runtime_exceptions.S:130-146
vector_entry sync_exception_sp_el0
    // 在 EL3 使用 SP_EL0 时发生的同步异常
    // 通常不应该发生，因为 EL3 代码通常使用 SP_EL3
    b   report_unhandled_exception
end_vector_entry sync_exception_sp_el0
```

**实际使用**:
- **EL3**: 很少使用，因为 EL3 代码通常使用 SP_EL3
- **EL1/EL2**: 在用户态（使用 SP_EL0）执行时发生异常

### SP_EL0 与 SP_EL3 的使用场景详解

#### 为什么需要两个栈指针？

ARMv8-A 架构为每个异常级别提供了两个栈指针：
- **SP_EL0**: 用户栈指针（User Stack Pointer）
- **SP_ELx**: 特权栈指针（Privileged Stack Pointer，x 是当前异常级别）

**设计原因**:
1. **隔离**: 用户态和内核态使用不同的栈，提供更好的隔离和安全性
2. **灵活性**: 允许在同一个异常级别内切换栈指针，无需改变异常级别
3. **性能**: 避免在用户态和内核态之间切换时频繁保存/恢复栈指针

#### SP_EL3 的使用场景

**SP_EL3** 是 EL3 的特权栈指针，用于：

1. **异常入口时的上下文保存**:
   - 当异常发生时，CPU 硬件**自动使用 SP_EL3** 作为栈指针
   - 用于保存异常发生时的 CPU 上下文（寄存器、系统寄存器等）
   - 这是**硬件自动行为**，不需要软件干预

2. **异常处理框架的上下文管理**:
   ```assembly
   // 异常发生时，硬件自动使用 SP_EL3
   // 软件在 SP_EL3 指向的栈上保存上下文
   prepare_el3_entry  // 保存通用寄存器、系统寄存器到 SP_EL3 栈
   ```

3. **嵌套异常处理**:
   - 在处理一个异常时又发生另一个异常（嵌套异常）
   - 使用 SP_EL3 保存嵌套异常的上下文

**代码示例**:
```assembly
// runtime_exceptions.S:532-537
func handle_interrupt_exception
    // 1. 保存上下文到 SP_EL3 栈
    bl  prepare_el3_entry
    
    // 2. 保存 EL3 系统寄存器到 SP_EL3 栈
    mrs x0, spsr_el3
    mrs x1, elr_el3
    stp x0, x1, [sp, #CTX_EL3STATE_OFFSET + CTX_SPSR_EL3]
    // 此时 sp 指向 SP_EL3
```

#### SP_EL0 的使用场景

**SP_EL0** 是 EL3 的用户栈指针，用于：

1. **C 运行时栈**:
   - 在 EL3 执行 **C 代码**时使用 SP_EL0 作为栈指针
   - 这是 TF-A 的设计选择：**C 代码使用 SP_EL0，汇编代码使用 SP_EL3**

2. **SMC Handler 执行**:
   - 处理 SMC 调用时，切换到 SP_EL0 执行 C 代码
   - 提供独立的 C 运行时栈空间

3. **中断 Handler 执行**:
   - 处理中断时，切换到 SP_EL0 执行 C 代码
   - 避免与异常处理栈（SP_EL3）冲突

**代码示例**:
```assembly
// runtime_exceptions.S:359-360, 398
sync_handler64:
    // 1. 从上下文恢复 C 运行时栈地址
    ldr x12, [x6, #CTX_EL3STATE_OFFSET + CTX_RUNTIME_SP]
    
    // 2. 切换到 SP_EL0
    msr spsel, #MODE_SP_EL0
    
    // 3. 设置 SP_EL0 为 C 运行时栈
    mov sp, x12
    
    // 4. 现在可以安全地调用 C 函数
    blr x15  // 调用 SMC handler (C 函数)
```

#### 栈指针切换流程

**完整的栈指针切换流程**:

```
1. 异常发生
   ↓
2. CPU 硬件自动使用 SP_EL3 作为栈指针
   ↓
3. 软件保存上下文到 SP_EL3 栈
   ↓
4. 切换到 SP_EL0（用于 C 代码执行）
   msr spsel, #MODE_SP_EL0
   mov sp, x12  // 设置 SP_EL0 为 C 运行时栈
   ↓
5. 执行 C 代码（使用 SP_EL0）
   ↓
6. 准备退出 EL3
   ↓
7. 保存 SP_EL0 的值（C 运行时栈）到上下文
   mov x17, sp
   msr spsel, #MODE_SP_ELX  // 切换回 SP_EL3
   str x17, [sp, #CTX_EL3STATE_OFFSET + CTX_RUNTIME_SP]
   ↓
8. 恢复上下文，ERET 返回
```

**代码实现** (TF-A):

**切换到 SP_EL0**:
```assembly
// runtime_exceptions.S:539-543
/* Switch to the runtime stack i.e. SP_EL0 */
ldr x2, [sp, #CTX_EL3STATE_OFFSET + CTX_RUNTIME_SP]  // 加载 C 运行时栈地址
mov x20, sp  // 保存 SP_EL3（上下文指针）
msr spsel, #MODE_SP_EL0  // 切换到 SP_EL0
mov sp, x2  // 设置 SP_EL0 为 C 运行时栈
```

**切换回 SP_EL3**:
```assembly
// context.S:603-605
/* Save the current SP_EL0 i.e. the EL3 runtime stack */
mov x17, sp  // 保存 SP_EL0（C 运行时栈）
msr spsel, #MODE_SP_ELX  // 切换回 SP_EL3
str x17, [sp, #CTX_EL3STATE_OFFSET + CTX_RUNTIME_SP]  // 保存到上下文
```

#### 使用场景对比表

| 栈指针 | 使用场景 | 典型用途 | 代码位置 |
|--------|---------|---------|---------|
| **SP_EL3** | 异常处理、上下文保存 | 保存 CPU 上下文、嵌套异常处理 | 异常向量入口、`prepare_el3_entry` |
| **SP_EL0** | C 代码执行 | SMC handler、中断 handler、C 运行时栈 | `sync_handler64`、`handle_interrupt_exception` |

#### 为什么 EL3 代码通常使用 SP_EL3？

**关键点**: 虽然 EL3 有两个栈指针，但 TF-A 的设计是：
- **异常处理框架**（汇编代码）使用 **SP_EL3**
- **C 代码执行**（handler 函数）使用 **SP_EL0**

**原因**:
1. **硬件自动行为**: 异常发生时，CPU 硬件自动使用 SP_EL3，软件必须遵循
2. **上下文隔离**: SP_EL3 用于保存异常上下文，SP_EL0 用于 C 代码执行，避免冲突
3. **设计清晰**: 明确区分异常处理栈（SP_EL3）和 C 运行时栈（SP_EL0）

#### 实际使用示例

**场景 1: SMC 调用处理**

```
NS-EL1 调用 SMC
    ↓
异常路由到 EL3，硬件使用 SP_EL3
    ↓
进入 sync_exception_aarch64 向量
    ↓
prepare_el3_entry() → 保存上下文到 SP_EL3 栈
    ↓
切换到 SP_EL0 → 执行 C 代码
    ↓
调用 SMC handler (C 函数) → 使用 SP_EL0 栈
    ↓
切换回 SP_EL3 → 恢复上下文
    ↓
ERET 返回 NS-EL1
```

**场景 2: 中断处理**

```
NS-EL1 发生中断，路由到 EL3
    ↓
异常路由到 EL3，硬件使用 SP_EL3
    ↓
进入 irq_aarch64 或 fiq_aarch64 向量
    ↓
prepare_el3_entry() → 保存上下文到 SP_EL3 栈
    ↓
切换到 SP_EL0 → 执行 C 代码
    ↓
调用中断 handler (C 函数) → 使用 SP_EL0 栈
    ↓
切换回 SP_EL3 → 恢复上下文
    ↓
ERET 返回或跳转到 S-EL1
```

#### 总结：SP_EL0 vs SP_EL3

1. **SP_EL3**:
   - **用途**: 异常处理、上下文保存
   - **使用时机**: 异常入口时硬件自动使用，软件保存上下文
   - **典型场景**: 异常向量入口、嵌套异常处理

2. **SP_EL0**:
   - **用途**: C 代码执行、运行时栈
   - **使用时机**: 切换到 C 代码时手动切换
   - **典型场景**: SMC handler、中断 handler 执行

3. **切换时机**:
   - **异常入口**: 硬件自动使用 SP_EL3
   - **执行 C 代码**: 手动切换到 SP_EL0
   - **退出 EL3**: 手动切换回 SP_EL3

4. **关键设计原则**:
   - **SP_EL3**: 异常处理栈（汇编代码）
   - **SP_EL0**: C 运行时栈（C 代码）
   - **隔离**: 两个栈互不干扰，提供更好的安全性和可维护性

### 内核（EL1/EL2）中的栈指针使用

#### EL1 中的栈指针：SP_EL0 vs SP_EL1

在 EL1（内核态）中，内核可以选择使用 **SP_EL0** 或 **SP_EL1** 作为栈指针，选择由 **SPSel** 寄存器控制：

- **SPSel = 0**: 使用 **SP_EL0** 作为栈指针
- **SPSel = 1**: 使用 **SP_EL1** 作为栈指针

#### Linux 内核中的栈指针使用

**Linux 内核的设计**:
- **默认**: 内核在 EL1 使用 **SP_EL1** 作为栈指针（`SPSel = 1`）
- **用户态**: EL0 应用程序使用 **SP_EL0** 作为栈指针
- **异常处理**: 内核异常处理程序通常使用 **SP_EL1**

**栈指针切换流程** (Linux 内核):

```
1. 用户态执行 (EL0)
   ↓ 使用 SP_EL0（用户栈）
   
2. 系统调用或中断发生
   ↓
3. 异常路由到 EL1
   ↓ CPU 硬件自动使用 SP_EL1
   
4. 内核异常处理程序
   ↓ 使用 SP_EL1（内核栈）
   
5. 处理完成，返回用户态
   ↓ 恢复 SP_EL0（用户栈）
```

#### SP_EL1 的使用场景（内核态）

**SP_EL1** 是 EL1 的特权栈指针，用于：

1. **内核代码执行**:
   - 内核代码（C 代码和汇编代码）使用 SP_EL1
   - 提供独立的内核栈空间，与用户栈隔离

2. **异常处理**:
   - 当异常从 EL0 进入 EL1 时，CPU 硬件自动使用 SP_EL1
   - 用于保存异常上下文和内核异常处理

3. **中断处理**:
   - 中断处理程序使用 SP_EL1
   - 提供独立的中断处理栈空间

4. **系统调用处理**:
   - 系统调用处理程序使用 SP_EL1
   - 提供独立的系统调用处理栈空间

**代码示例** (Linux 内核概念):
```c
// 内核代码执行（使用 SP_EL1）
void kernel_function() {
    // 使用 SP_EL1 作为栈指针
    int local_var;
    // ...
}

// 异常处理（使用 SP_EL1）
void exception_handler() {
    // CPU 硬件自动使用 SP_EL1
    // 保存异常上下文到 SP_EL1 栈
    // ...
}
```

#### SP_EL0 的使用场景（用户态）

**SP_EL0** 是用户栈指针，用于：

1. **用户态应用程序**:
   - EL0 应用程序使用 SP_EL0 作为栈指针
   - 每个进程/线程有自己独立的 SP_EL0 值

2. **用户栈管理**:
   - 用户态代码的局部变量、函数调用栈
   - 与内核栈（SP_EL1）完全隔离

3. **上下文切换**:
   - 进程切换时，保存/恢复 SP_EL0
   - 每个进程有自己独立的用户栈

**代码示例** (用户态应用程序):
```c
// 用户态应用程序（使用 SP_EL0）
int main() {
    // 使用 SP_EL0 作为栈指针
    int local_var;
    // ...
}
```

#### EL1 中的栈指针切换

**从用户态到内核态**:
```
用户态执行 (EL0, SP_EL0)
    ↓
系统调用/中断发生
    ↓
异常路由到 EL1
    ↓ CPU 硬件自动切换到 SP_EL1
内核异常处理 (EL1, SP_EL1)
    ↓
处理完成
    ↓
ERET 返回 EL0
    ↓ 恢复 SP_EL0
用户态执行 (EL0, SP_EL0)
```

**关键点**:
- **异常入口**: CPU 硬件自动切换到 SP_EL1
- **异常退出**: ERET 指令自动恢复 SP_EL0（从保存的上下文）

#### EL2 中的栈指针：SP_EL0 vs SP_EL2

在 EL2（Hypervisor）中，类似地可以选择使用 **SP_EL0** 或 **SP_EL2**：

- **SPSel = 0**: 使用 **SP_EL0** 作为栈指针
- **SPSel = 1**: 使用 **SP_EL2** 作为栈指针

**EL2 的使用场景**:
- **Hypervisor 代码**: 通常使用 **SP_EL2**（`SPSel = 1`）
- **虚拟机管理**: 提供独立的 Hypervisor 栈空间
- **虚拟化异常处理**: 处理来自 EL1/EL0 的异常

#### 内核栈指针使用对比表

| 异常级别 | 栈指针 | 使用场景 | 典型用途 |
|---------|--------|---------|---------|
| **EL0** | **SP_EL0** | 用户态应用程序 | 用户栈、应用程序代码执行 |
| **EL1** | **SP_EL1** | 内核态代码 | 内核栈、异常处理、中断处理、系统调用 |
| **EL1** | **SP_EL0** | 特殊场景 | 某些内核代码可能临时使用（不常见） |
| **EL2** | **SP_EL2** | Hypervisor 代码 | Hypervisor 栈、虚拟化异常处理 |
| **EL3** | **SP_EL3** | 安全监控代码 | 安全监控栈、异常处理、上下文保存 |
| **EL3** | **SP_EL0** | C 运行时栈 | C 代码执行（TF-A 设计） |

#### 内核中的栈指针切换示例

**场景 1: 系统调用**

```
用户态应用程序 (EL0, SP_EL0)
    ↓
执行 SVC 指令（系统调用）
    ↓
同步异常，路由到 EL1
    ↓ CPU 硬件自动使用 SP_EL1
内核系统调用处理程序 (EL1, SP_EL1)
    ↓
处理系统调用
    ↓
ERET 返回 EL0
    ↓ 恢复 SP_EL0
用户态应用程序 (EL0, SP_EL0)
```

**场景 2: 中断处理**

```
用户态应用程序 (EL0, SP_EL0)
    ↓
中断发生
    ↓
中断路由到 EL1
    ↓ CPU 硬件自动使用 SP_EL1
内核中断处理程序 (EL1, SP_EL1)
    ↓
处理中断
    ↓
ERET 返回 EL0
    ↓ 恢复 SP_EL0
用户态应用程序 (EL0, SP_EL0)
```

**场景 3: 内核异常（嵌套异常）**

```
内核代码执行 (EL1, SP_EL1)
    ↓
内核异常发生（例如：页错误）
    ↓
异常仍在 EL1，使用 SP_EL1
    ↓
内核异常处理程序 (EL1, SP_EL1)
    ↓
处理异常
    ↓
返回内核代码
```

#### TF-A 中 EL1 栈指针的设置

**设置 EL1 上下文** (`context_mgmt.c`):
```c
// 设置 EL1 的 SPSR，指定栈指针选择
ep_info->spsr = SPSR_64(MODE_EL1, MODE_SP_ELX, DISABLE_ALL_EXCEPTIONS);
// MODE_SP_ELX = 1，表示使用 SP_EL1

// 设置 SP_EL1 的值
write_el1_ctx_common(ctx, sp_el1, stack_address);
```

**恢复 EL1 上下文** (`context.S`):
```assembly
// 恢复 SP_EL1
ldr x17, [sp, #CTX_EL1STATE_OFFSET + CTX_SP_EL1]
msr sp_el1, x17

// 设置 SPSR，指定使用 SP_EL1
ldr x16, [sp, #CTX_EL1STATE_OFFSET + CTX_SPSR_EL1]
msr spsr_el1, x16  // SPSR.M[0] = 1 表示使用 SP_EL1
```

#### 为什么内核使用 SP_EL1？

**设计原因**:
1. **隔离**: 用户栈（SP_EL0）和内核栈（SP_EL1）完全隔离
2. **安全性**: 防止用户态代码破坏内核栈
3. **性能**: 避免在用户态和内核态之间频繁切换栈指针
4. **标准实践**: 这是 ARM 架构和 Linux 内核的标准设计

#### 总结：内核中的栈指针使用

1. **EL0 (用户态)**:
   - **SP_EL0**: 用户栈指针
   - **用途**: 用户态应用程序代码执行

2. **EL1 (内核态)**:
   - **SP_EL1**: 内核栈指针（默认）
   - **用途**: 内核代码、异常处理、中断处理、系统调用
   - **SP_EL0**: 用户栈指针（用户态时使用）

3. **EL2 (Hypervisor)**:
   - **SP_EL2**: Hypervisor 栈指针（默认）
   - **用途**: Hypervisor 代码、虚拟化异常处理

4. **EL3 (安全监控)**:
   - **SP_EL3**: 安全监控栈指针（异常处理）
   - **SP_EL0**: C 运行时栈（C 代码执行，TF-A 设计）

**关键点**:
- **用户态使用 SP_EL0**: 每个进程有独立的用户栈
- **内核态使用 SP_EL1**: 内核代码使用独立的内核栈
- **异常自动切换**: 异常发生时，CPU 硬件自动切换到对应的特权栈指针
- **隔离**: 用户栈和内核栈完全隔离，提供安全性

### 为什么内核只使用 SP_EL1，而 ATF 使用 SP_EL0 + SP_EL3？

这是一个很好的设计问题，涉及到不同软件的设计目标和需求差异。

#### 设计差异对比

| 特性 | Linux 内核 (EL1) | ARM Trusted Firmware (EL3) |
|------|-----------------|---------------------------|
| **栈指针使用** | 只使用 **SP_EL1** | 使用 **SP_EL3** + **SP_EL0** |
| **SP_EL1/SP_EL3** | 用于所有内核代码 | 用于异常处理、上下文保存 |
| **SP_EL0** | 留给用户态（EL0） | 用于 C 代码执行 |
| **设计复杂度** | 简单，单一栈指针 | 复杂，双栈指针管理 |
| **使用场景** | 单一异常级别（EL1） | 跨安全状态、跨异常级别 |

#### Linux 内核为什么只使用 SP_EL1？

**1. 设计简单性**:
- Linux 内核代码都在 **EL1** 执行
- 所有内核代码（C 代码和汇编代码）可以使用同一个栈指针
- 不需要区分不同的执行上下文

**2. 栈指针分配清晰**:
```
EL0 (用户态): SP_EL0  ← 用户栈
EL1 (内核态): SP_EL1  ← 内核栈
```
- **SP_EL0**: 专门留给用户态（EL0）使用
- **SP_EL1**: 专门给内核态（EL1）使用
- 职责清晰，互不干扰

**3. 异常处理简单**:
- 异常从 EL0 进入 EL1 时，硬件自动使用 SP_EL1
- 内核异常处理程序直接使用 SP_EL1
- 不需要额外的栈指针切换

**4. 性能考虑**:
- 单一栈指针，减少切换开销
- 栈管理简单，减少代码复杂度

**代码示例** (Linux 内核概念):
```c
// 所有内核代码都使用 SP_EL1
void kernel_function() {
    // 使用 SP_EL1 作为栈指针
    int local_var;
    // ...
}

void exception_handler() {
    // CPU 硬件自动使用 SP_EL1
    // 所有内核代码统一使用 SP_EL1
    // ...
}
```

#### ATF 为什么使用 SP_EL0 + SP_EL3？

**1. 异常处理与 C 代码执行分离**:

ATF 需要区分两种不同的执行上下文：

- **异常处理上下文** (使用 SP_EL3):
  - 异常发生时，CPU 硬件自动使用 SP_EL3
  - 用于保存异常上下文（寄存器、系统寄存器）
  - 这是**硬件强制行为**，软件必须遵循

- **C 代码执行上下文** (使用 SP_EL0):
  - 执行 C 代码时，使用 SP_EL0 作为运行时栈
  - 提供独立的 C 运行时栈空间
  - 避免与异常处理栈（SP_EL3）冲突

**2. 栈空间隔离**:

```
SP_EL3: 异常处理栈
    ├─ 异常上下文保存
    ├─ 嵌套异常处理
    └─ 系统寄存器保存

SP_EL0: C 运行时栈
    ├─ C 函数调用栈
    ├─ 局部变量
    └─ 函数参数
```

**优势**:
- **隔离**: 异常处理栈和 C 运行时栈完全隔离
- **安全性**: 防止异常处理栈被 C 代码破坏
- **可维护性**: 清晰的职责分离

**3. 跨安全状态处理**:

ATF 需要处理来自不同安全状态的请求：
- **Secure 世界**: 安全世界的请求
- **Non-secure 世界**: 非安全世界的请求

使用不同的栈指针可以更好地管理不同安全状态的上下文。

**4. TF-A 的设计哲学**:

TF-A 的设计原则是：
- **异常处理框架**（汇编代码）→ 使用 **SP_EL3**
- **C 代码执行**（handler 函数）→ 使用 **SP_EL0**

这种设计提供了：
- **清晰的职责分离**: 异常处理和 C 代码执行使用不同的栈
- **更好的可维护性**: 代码结构清晰
- **更强的安全性**: 栈空间隔离

**代码示例** (TF-A):
```assembly
// 异常处理（使用 SP_EL3）
vector_entry sync_exception_aarch64
    // CPU 硬件自动使用 SP_EL3
    prepare_el3_entry  // 保存上下文到 SP_EL3 栈
    // ...
    
    // 切换到 SP_EL0（用于 C 代码）
    msr spsel, #MODE_SP_EL0
    mov sp, x12  // 设置 SP_EL0 为 C 运行时栈
    
    // 调用 C 函数（使用 SP_EL0）
    blr x15  // SMC handler (C 函数)
    
    // 切换回 SP_EL3
    msr spsel, #MODE_SP_ELX
    // ...
end_vector_entry
```

#### 设计选择的原因总结

**Linux 内核 (EL1) 使用单一栈指针的原因**:

1. **简单性**: 所有代码都在 EL1，使用单一栈指针足够
2. **清晰性**: SP_EL0 留给用户态，SP_EL1 给内核态，职责清晰
3. **性能**: 减少栈指针切换开销
4. **标准实践**: 这是 ARM 架构和 Linux 内核的标准设计

**ATF (EL3) 使用双栈指针的原因**:

1. **硬件要求**: 异常发生时，CPU 硬件强制使用 SP_EL3
2. **职责分离**: 异常处理栈（SP_EL3）和 C 运行时栈（SP_EL0）分离
3. **安全性**: 栈空间隔离，防止相互干扰
4. **灵活性**: 支持跨安全状态、跨异常级别的处理
5. **设计哲学**: TF-A 的设计选择，提供更好的代码组织结构

#### 实际使用对比

**Linux 内核栈指针使用**:
```
用户态 (EL0)
    ↓ 使用 SP_EL0
系统调用/中断
    ↓ 硬件自动切换到 SP_EL1
内核态 (EL1)
    ↓ 使用 SP_EL1（所有内核代码）
处理完成
    ↓ ERET 恢复 SP_EL0
用户态 (EL0)
```

**ATF 栈指针使用**:
```
低异常级别 (EL0/EL1/EL2)
    ↓ 异常发生
EL3 异常入口
    ↓ 硬件自动使用 SP_EL3
异常处理框架
    ↓ 保存上下文到 SP_EL3 栈
切换到 SP_EL0
    ↓ 手动切换到 SP_EL0
C 代码执行
    ↓ 使用 SP_EL0（C 运行时栈）
切换回 SP_EL3
    ↓ 手动切换回 SP_EL3
恢复上下文
    ↓ ERET 返回
低异常级别
```

#### 总结

**为什么内核只使用 SP_EL1？**
- 设计简单：所有内核代码都在 EL1，单一栈指针足够
- 职责清晰：SP_EL0 给用户态，SP_EL1 给内核态
- 性能优化：减少栈指针切换开销

**为什么 ATF 使用 SP_EL0 + SP_EL3？**
- 硬件要求：异常发生时，CPU 硬件强制使用 SP_EL3
- 职责分离：异常处理栈（SP_EL3）和 C 运行时栈（SP_EL0）分离
- 安全性：栈空间隔离，防止相互干扰
- 设计哲学：TF-A 的设计选择，提供更好的代码组织结构

**关键差异**:
- **Linux 内核**: 单一异常级别（EL1），使用单一栈指针（SP_EL1）
- **ATF**: 跨安全状态、跨异常级别，使用双栈指针（SP_EL3 + SP_EL0）

这两种设计都是合理的，取决于软件的具体需求和设计目标。

### 为什么内核也保存异常上下文，但不使用 SP_EL0？

这是一个非常好的问题！确实，内核也需要保存异常上下文，和 ATF 一样。但关键区别在于：**内核的异常上下文和 C 代码执行共享同一个栈（SP_EL1），而 ATF 将它们分离到不同的栈（SP_EL3 和 SP_EL0）**。

#### 内核的异常处理流程

**内核异常处理（使用 SP_EL1）**:

```
用户态 (EL0, SP_EL0)
    ↓
异常发生（系统调用/中断）
    ↓
硬件自动切换到 SP_EL1
    ↓
内核异常向量入口
    ↓
保存异常上下文到 SP_EL1 栈
    ├─ 通用寄存器 (x0-x30)
    ├─ SP_EL0 (用户栈指针)
    ├─ ELR_EL1 (返回地址)
    ├─ SPSR_EL1 (处理器状态)
    └─ 其他系统寄存器
    ↓
调用 C 异常处理函数
    ↓ 仍在 SP_EL1 栈上
C 函数执行（使用 SP_EL1 栈）
    ├─ 局部变量
    ├─ 函数调用栈
    └─ 函数参数
    ↓
恢复异常上下文
    ↓
ERET 返回用户态
```

**关键点**: 异常上下文和 C 代码栈都在 **同一个 SP_EL1 栈**上！

#### ATF 的异常处理流程

**ATF 异常处理（使用 SP_EL3 + SP_EL0）**:

```
低异常级别 (EL0/EL1/EL2)
    ↓
异常发生（SMC/中断）
    ↓
硬件自动切换到 SP_EL3
    ↓
EL3 异常向量入口
    ↓
保存异常上下文到 SP_EL3 栈
    ├─ 通用寄存器 (x0-x30)  → 256 字节
    ├─ EL3 系统寄存器        → 112 字节
    └─ 其他上下文信息        → 总计约 368 字节
    ↓
切换到 SP_EL0
    ↓
调用 C 异常处理函数
    ↓ 使用 SP_EL0 栈
C 函数执行（使用 SP_EL0 栈）
    ├─ 局部变量
    ├─ 函数调用栈（可能很深）
    └─ 函数参数
    ↓
切换回 SP_EL3
    ↓
恢复异常上下文
    ↓
ERET 返回
```

**关键点**: 异常上下文保存在 **SP_EL3 栈**，C 代码执行使用 **SP_EL0 栈**，两者分离！

#### 为什么内核可以共享栈，而 ATF 需要分离？

**1. 栈空间大小差异**:

| 特性 | Linux 内核 | ATF |
|------|-----------|-----|
| **栈大小** | 通常 8KB - 32KB | 通常 1KB - 4KB |
| **异常上下文** | 约 200-300 字节 | 约 368 字节 |
| **C 代码栈需求** | 通常几百字节到几 KB | 可能很深，需要大量栈空间 |
| **共享栈可行性** | ✅ 可行（栈足够大） | ❌ 不可行（栈较小） |

**内核栈足够大**:
- 内核栈通常分配 8KB - 32KB
- 异常上下文约 200-300 字节
- C 代码执行通常需要几百字节到几 KB
- **总和远小于内核栈大小**，可以安全共享

**ATF 栈较小**:
- ATF 栈通常只有 1KB - 4KB
- 异常上下文约 368 字节
- C 代码执行可能需要很深的调用栈
- **如果共享，可能导致栈溢出**

**2. 异常上下文大小对比**:

**内核异常上下文** (EL1):
```c
// 内核需要保存的上下文（约 200-300 字节）
struct pt_regs {
    // 通用寄存器
    unsigned long regs[31];      // 31 × 8 = 248 字节
    unsigned long sp;            // 8 字节
    unsigned long pc;             // 8 字节
    unsigned long pstate;         // 8 字节
    // 其他系统寄存器（可选）
};
```

**ATF 异常上下文** (EL3):
```c
// ATF 需要保存的上下文（约 368 字节）
struct cpu_context {
    // 通用寄存器
    gp_regs_ctx_t gpregs_ctx;     // 256 字节 (32 × 8)
    // EL3 系统寄存器
    el3_state_t el3state_ctx;     // 112 字节
    // 其他扩展上下文（SVE、PAuth 等）
    // 可能更大
};
```

**3. C 代码调用栈深度**:

**内核 C 代码**:
- 异常处理函数通常调用深度较浅（2-5 层）
- 栈使用量可控（几百字节到几 KB）

**ATF C 代码**:
- SMC handler 可能调用多个服务
- 可能涉及安全状态切换
- 调用栈可能很深（5-10 层或更深）
- 栈使用量可能较大（几 KB）

**4. 隔离需求**:

**内核**:
- 异常上下文和 C 代码栈共享，但都在内核空间
- 不需要严格的隔离（都在同一安全域）

**ATF**:
- 异常上下文需要严格保护（安全关键）
- C 代码执行可能涉及复杂的逻辑
- **分离可以防止 C 代码破坏异常上下文**

**5. SP_EL0 的用途**:

**内核中 SP_EL0 的用途**:
- **专门留给用户态（EL0）使用**
- 如果内核使用 SP_EL0，会与用户态冲突
- 每个进程/线程有自己独立的 SP_EL0 值
- **内核不能使用 SP_EL0**

**ATF 中 SP_EL0 的用途**:
- **EL3 没有用户态**（EL0 不存在于 EL3）
- SP_EL0 在 EL3 中可以自由使用
- **ATF 选择使用 SP_EL0 作为 C 运行时栈**

#### 实际代码对比

**内核异常处理（伪代码）**:
```c
// 内核异常向量入口
kernel_exception_entry:
    // 硬件自动使用 SP_EL1
    // 保存异常上下文到 SP_EL1 栈
    save_context_to_sp_el1()
    
    // 调用 C 异常处理函数（仍在 SP_EL1 栈上）
    bl handle_exception  // C 函数，使用 SP_EL1 栈
    
    // 恢复异常上下文（从 SP_EL1 栈）
    restore_context_from_sp_el1()
    
    // ERET 返回用户态
    eret
```

**ATF 异常处理（实际代码）**:
```assembly
// ATF 异常向量入口
sync_exception_aarch64:
    // 硬件自动使用 SP_EL3
    // 保存异常上下文到 SP_EL3 栈
    bl prepare_el3_entry
    
    // 切换到 SP_EL0
    ldr x12, [sp, #CTX_EL3STATE_OFFSET + CTX_RUNTIME_SP]
    msr spsel, #MODE_SP_EL0
    mov sp, x12
    
    // 调用 C 异常处理函数（使用 SP_EL0 栈）
    blr x15  // C 函数，使用 SP_EL0 栈
    
    // 切换回 SP_EL3
    mov x17, sp
    msr spsel, #MODE_SP_ELX
    str x17, [sp, #CTX_EL3STATE_OFFSET + CTX_RUNTIME_SP]
    
    // 恢复异常上下文（从 SP_EL3 栈）
    // ERET 返回
    b el3_exit
```

#### 为什么内核不使用 SP_EL0？

**关键原因**:

1. **SP_EL0 是用户态的栈指针**:
   - 内核中，SP_EL0 专门留给用户态（EL0）使用
   - 每个进程/线程有自己独立的 SP_EL0 值
   - **如果内核使用 SP_EL0，会与用户态冲突**

2. **内核栈足够大**:
   - 内核栈（SP_EL1）通常 8KB - 32KB
   - 异常上下文 + C 代码栈 < 内核栈大小
   - **共享栈是可行的，不需要分离**

3. **设计简单性**:
   - 单一栈指针，代码更简单
   - 不需要栈指针切换
   - 减少代码复杂度

4. **性能考虑**:
   - 避免栈指针切换开销
   - 减少内存访问
   - 提高性能

#### 总结：为什么内核不使用 SP_EL0？

| 原因 | 说明 |
|------|------|
| **SP_EL0 的用途** | SP_EL0 在内核中专门留给用户态（EL0），不能用于内核代码 |
| **栈空间足够** | 内核栈（SP_EL1）足够大（8KB-32KB），可以同时容纳异常上下文和 C 代码栈 |
| **设计简单** | 单一栈指针，代码更简单，不需要栈指针切换 |
| **性能优化** | 避免栈指针切换开销，提高性能 |
| **隔离需求低** | 内核异常上下文和 C 代码栈都在内核空间，不需要严格隔离 |

**关键区别**:
- **内核**: 异常上下文和 C 代码栈**共享 SP_EL1**（栈足够大）
- **ATF**: 异常上下文保存在 **SP_EL3**，C 代码栈使用 **SP_EL0**（栈较小，需要分离）

**两种设计都是合理的**:
- **内核设计**: 简单、高效，适合栈空间充足的情况
- **ATF 设计**: 安全、隔离，适合栈空间有限、需要严格隔离的情况

### SP_EL0 在 ATF 中的初始化

是的，**SP_EL0 在 ATF 中必须被初始化到 ATF 的内存空间中**。这是 ATF 设计的关键部分。

#### SP_EL0 的初始化流程

**1. 启动时初始化**:

在 ATF 启动时（BL31 初始化阶段），会调用 `plat_set_my_stack()` 来初始化 SP_EL0：

```assembly
// el3_common_macros.S:369-382
/* Use SP_EL0 for the C runtime stack. */
msr spsel, #0  // 切换到 SP_EL0

/* Allocate a stack whose memory will be marked as Normal-IS-WBWA */
bl plat_set_my_stack  // 初始化 SP_EL0
```

**2. `plat_set_my_stack()` 的实现**:

```assembly
// platform_up_stack.S:37-41 (单核版本)
func plat_set_my_stack
    get_up_stack platform_normal_stacks, PLATFORM_STACK_SIZE
    mov sp, x0  // 设置 SP_EL0 为栈地址
    ret
endfunc plat_set_my_stack

// platform_mp_stack.S:47-52 (多核版本)
func plat_set_my_stack
    mov x9, x30
    bl  plat_get_my_stack  // 获取当前 CPU 的栈地址
    mov sp, x0  // 设置 SP_EL0 为栈地址
    ret x9
endfunc plat_set_my_stack
```

**3. 栈内存分配**:

栈内存是在 ATF 的链接时分配的，位于 `.tzfw_normal_stacks` 段：

#### SP_EL3 的初始化

**SP_EL3 指向 `cpu_context` 结构**，而不是一个独立的栈内存区域。这是 ATF 设计的关键特点。

**1. SP_EL3 的用途**:

SP_EL3 在异常处理时指向 `cpu_context_t` 结构，这个结构用于：
- **保存异常上下文**（通用寄存器、系统寄存器）
- **作为异常处理栈**（在 `cpu_context_t` 结构上保存上下文）

**关键点**: SP_EL3 **不是指向一个独立的栈内存**，而是指向 `cpu_context_t` 结构本身！

**2. `cpu_context_t` 结构的分配**:

`cpu_context_t` 结构是**静态分配的**，存储在 ATF 的内存空间中：

```c
// psci_setup.c:32
static cpu_context_t psci_ns_context[PLATFORM_CORE_COUNT];
// 每个 CPU 有一个独立的 cpu_context_t 结构
```

**内存位置**:
- `cpu_context_t` 结构在 ATF 的 **BSS 段**或**数据段**中分配
- 每个 CPU 有独立的 `cpu_context_t` 结构
- 这些结构的地址存储在 `cpu_data_t.cpu_context[]` 指针数组中

**3. SP_EL3 的设置**:

SP_EL3 通过 `cm_set_next_context()` 函数设置：

```c
// context_mgmt.h:67-86
static inline void cm_set_next_context(void *context)
{
    // 切换到 SP_EL3
    msr spsel, #1  // MODE_SP_ELX = 1
    
    // 设置 SP_EL3 指向 cpu_context 结构
    mov sp, context
    
    // 切换回 SP_EL0
    msr spsel, #0
}
```

**设置时机**:
- 在准备退出 EL3 时（`cm_set_next_eret_context()`）
- 在异常处理时，SP_EL3 已经指向正确的 `cpu_context_t` 结构

**4. `cpu_context_t` 结构的内存布局**:

```c
// context.h
typedef struct cpu_context {
    gp_regs_ctx_t gpregs_ctx;        // 通用寄存器上下文 (256 字节)
    el3_state_t el3state_ctx;        // EL3 系统寄存器上下文 (112 字节)
    // 其他扩展上下文（SVE、PAuth 等）
} cpu_context_t;
```

**结构大小**: 约 368 字节（基础结构），加上扩展上下文可能更大

**5. SP_EL3 与 `cpu_context_t` 的关系**:

```
异常发生
    ↓
硬件自动使用 SP_EL3
    ↓
SP_EL3 指向 cpu_context_t 结构
    ↓
保存异常上下文到 cpu_context_t 结构
    ├─ 通用寄存器 → gpregs_ctx
    ├─ EL3 系统寄存器 → el3state_ctx
    └─ 其他上下文 → 扩展上下文
    ↓
异常处理完成
    ↓
从 cpu_context_t 结构恢复上下文
    ↓
ERET 返回
```

**关键点**:
- **SP_EL3 指向 `cpu_context_t` 结构本身**
- **`cpu_context_t` 结构既是上下文存储，也是异常处理栈**
- **结构在 ATF 的内存空间中静态分配**

**6. SP_EL3 的初始化流程**:

```
ATF 启动
    ↓
初始化 cpu_context_t 结构
    ├─ psci_ns_context[PLATFORM_CORE_COUNT] (静态分配)
    └─ 其他安全状态的上下文结构
    ↓
设置 cpu_data_t.cpu_context[] 指针
    ├─ cpu_context[CPU_CONTEXT_NS] = &psci_ns_context[cpu_idx]
    └─ cpu_context[CPU_CONTEXT_SECURE] = ...
    ↓
准备退出 EL3 时
    ↓
cm_set_next_eret_context(security_state)
    ↓
cm_set_next_context(ctx)  // 设置 SP_EL3 指向 cpu_context_t
    ↓
SP_EL3 指向正确的 cpu_context_t 结构
```

**7. SP_EL3 vs SP_EL0 的内存位置对比**:

| 栈指针 | 内存位置 | 内存类型 | 大小 | 用途 |
|--------|---------|---------|------|------|
| **SP_EL0** | `.tzfw_normal_stacks` 段 | 栈内存 | PLATFORM_STACK_SIZE (1KB-4KB) | C 运行时栈 |
| **SP_EL3** | `cpu_context_t` 结构（BSS/数据段） | 上下文结构 | 约 368 字节 | 异常上下文保存 |

**关键区别**:
- **SP_EL0**: 指向独立的栈内存区域（`.tzfw_normal_stacks`）
- **SP_EL3**: 指向 `cpu_context_t` 结构（上下文结构本身）

**8. 为什么 SP_EL3 指向 `cpu_context_t` 结构？**

**设计原因**:
1. **直接访问**: 异常处理时可以直接访问上下文结构，无需额外的栈空间
2. **内存效率**: 不需要额外的异常处理栈，节省内存
3. **简单性**: 上下文结构和异常处理栈合二为一，简化设计
4. **硬件要求**: 异常发生时，硬件自动使用 SP_EL3，直接指向上下文结构最方便

**总结**: SP_EL3 初始化到 `cpu_context_t` 结构，这个结构在 ATF 的内存空间中静态分配，每个 CPU 有独立的上下文结构。

```assembly
// platform_up_stack.S:49-50
/* Single cpu stack in normal memory. */
/* Used for C code during boot, PLATFORM_STACK_SIZE bytes */
declare_stack platform_normal_stacks, .tzfw_normal_stacks, \
        PLATFORM_STACK_SIZE, 1, CACHE_WRITEBACK_GRANULE
```

**栈内存特点**:
- **位置**: 在 ATF 的内存空间中（`.tzfw_normal_stacks` 段）
- **大小**: `PLATFORM_STACK_SIZE`（通常 1KB - 4KB，取决于平台配置）
- **类型**: Normal memory（可缓存、可写回）
- **对齐**: 按照 `CACHE_WRITEBACK_GRANULE` 对齐

#### SP_EL0 的保存和恢复

**1. 退出 EL3 时保存 SP_EL0**:

当退出 EL3 时，SP_EL0 的值会被保存到上下文中：

```assembly
// context.S:597-605
/* Save the current SP_EL0 i.e. the EL3 runtime stack which
 * will be used for handling the next SMC.
 * Then switch to SP_EL3.
 */
mov x17, sp  // 保存 SP_EL0 的值
msr spsel, #MODE_SP_ELX  // 切换回 SP_EL3
str x17, [sp, #CTX_EL3STATE_OFFSET + CTX_RUNTIME_SP]  // 保存到上下文
```

**保存位置**: `cpu_context_t.el3state_ctx.runtime_sp`

**2. 进入 EL3 时恢复 SP_EL0**:

当进入 EL3 处理异常时，会从上下文恢复 SP_EL0：

```assembly
// runtime_exceptions.S:357-360
/* Restore the saved C runtime stack value which will become the new
 * SP_EL0 i.e. EL3 runtime stack. It was saved in the 'cpu_context'
 * structure prior to the last ERET from EL3.
 */
ldr x12, [x6, #CTX_EL3STATE_OFFSET + CTX_RUNTIME_SP]  // 从上下文加载
msr spsel, #MODE_SP_EL0  // 切换到 SP_EL0
mov sp, x12  // 恢复 SP_EL0 的值
```

#### SP_EL0 栈内存的生命周期

**1. 启动阶段**:
```
ATF 启动
    ↓
plat_set_my_stack() 初始化 SP_EL0
    ↓
SP_EL0 指向 .tzfw_normal_stacks 段
    ↓
ATF 正常运行，使用 SP_EL0 作为 C 运行时栈
```

**2. 异常处理阶段**:
```
异常发生，进入 EL3
    ↓
硬件自动使用 SP_EL3
    ↓
保存异常上下文到 SP_EL3 栈
    ↓
从上下文恢复 SP_EL0（之前保存的值）
    ↓
切换到 SP_EL0
    ↓
执行 C 代码（使用 SP_EL0 栈）
    ↓
保存 SP_EL0 到上下文
    ↓
切换回 SP_EL3
    ↓
ERET 退出 EL3
```

**3. 多核支持**:

在多核系统中，每个 CPU 都有自己独立的 SP_EL0 栈：

```assembly
// platform_mp_stack.S:59-61
/* Per-CPU stacks in normal memory. Each CPU gets a
 * stack of PLATFORM_STACK_SIZE bytes.
 */
declare_stack platform_normal_stacks, .tzfw_normal_stacks, \
        PLATFORM_STACK_SIZE, PLATFORM_CORE_COUNT, \
        CACHE_WRITEBACK_GRANULE
```

**每个 CPU 的栈地址计算**:
```assembly
// asm_macros.S:146-152
.macro get_my_mp_stack _name, _size
    bl  plat_my_core_pos  // 获取当前 CPU 的索引
    adrp x2, (\_name + \_size)
    add x2, x2, :lo12:(\_name + \_size)
    mov x1, #\_size
    madd x0, x0, x1, x2  // 计算当前 CPU 的栈地址
.endm
```

**栈地址 = 基地址 + (CPU 索引 × 栈大小)**

#### SP_EL0 栈内存的位置

**内存布局**:

```
ATF 内存空间
├─ 代码段 (.text)
├─ 数据段 (.data)
├─ BSS 段 (.bss)
├─ 栈段 (.tzfw_normal_stacks)  ← SP_EL0 栈在这里
│  ├─ CPU 0 栈 (PLATFORM_STACK_SIZE 字节)
│  ├─ CPU 1 栈 (PLATFORM_STACK_SIZE 字节)
│  ├─ CPU 2 栈 (PLATFORM_STACK_SIZE 字节)
│  └─ ...
└─ 其他段
```

**关键点**:
- **SP_EL0 栈在 ATF 的内存空间中**
- **每个 CPU 有独立的栈空间**
- **栈内存是静态分配的**（链接时分配）
- **栈地址在运行时不会改变**

#### 为什么 SP_EL0 必须在 ATF 空间中？

**1. 安全性**:
- SP_EL0 栈用于执行 ATF 的 C 代码
- 必须位于 ATF 的内存空间中，确保安全访问
- 防止被非安全世界访问或破坏

**2. 内存管理**:
- ATF 需要管理自己的栈内存
- 栈内存必须在 ATF 的控制范围内
- 确保栈内存的分配和释放由 ATF 管理

**3. 性能**:
- 栈内存位于 ATF 的内存空间中，访问速度快
- 可以配置为可缓存内存，提高性能

**4. 隔离性**:
- SP_EL0 栈与 SP_EL3 栈分离
- 每个栈在独立的内存区域，提供更好的隔离

#### 总结：SP_EL0 在 ATF 中的初始化

1. **初始化时机**: ATF 启动时（BL31 初始化阶段）
2. **初始化函数**: `plat_set_my_stack()`
3. **栈内存位置**: ATF 内存空间中的 `.tzfw_normal_stacks` 段
4. **栈大小**: `PLATFORM_STACK_SIZE`（通常 1KB - 4KB）
5. **多核支持**: 每个 CPU 有独立的栈空间
6. **保存和恢复**: 退出 EL3 时保存，进入 EL3 时恢复
7. **生命周期**: 栈内存在整个 ATF 运行期间存在

**关键点**:
- ✅ **SP_EL0 必须在 ATF 的内存空间中初始化**
- ✅ **栈内存是静态分配的**（链接时分配）
- ✅ **每个 CPU 有独立的栈空间**
- ✅ **栈地址保存在上下文中，在异常处理时恢复**

#### 2. Current EL with SP_ELx (组 2: 0x200 - 0x400)

**使用条件**:
- 异常发生在**当前异常级别**
- 当前使用 **SP_ELx** 作为栈指针（`SPSel = 1`，x 是当前异常级别）

**典型场景**:
- 在 EL3 执行时，使用 SP_EL3 作为栈指针
- 发生同步异常、IRQ、FIQ 或 SError
- **嵌套异常**: 在处理一个异常时又发生另一个异常

**代码示例** (TF-A):
```assembly
// runtime_exceptions.S:170-178
vector_entry sync_exception_sp_elx
    // 在 EL3 使用 SP_EL3 时发生的同步异常
    // 通常表示在处理异常时又发生了异常（嵌套异常）
    // 可能表示 SP_EL3 已损坏
    b   report_unhandled_exception
end_vector_entry sync_exception_sp_elx
```

**实际使用**:
- **EL3**: 嵌套异常处理（在处理一个异常时又发生异常）
- **EL1/EL2**: 在内核态（使用 SP_EL1/SP_EL2）执行时发生异常

#### 3. Lower EL using AArch64 (组 3: 0x400 - 0x600)

**使用条件**:
- 异常从**低异常级别**进入当前异常级别
- 低异常级别处于 **AArch64** 执行状态

**典型场景**:
- 在 EL0/EL1/EL2 执行 AArch64 代码时发生异常
- 异常被路由到更高的异常级别（EL1/EL2/EL3）处理
- **最常见**: 从 NS-EL1 发生的中断路由到 EL3

**代码示例** (TF-A):
```assembly
// runtime_exceptions.S:222-233
vector_entry sync_exception_aarch64
    // 从低异常级别（AArch64）进入 EL3 的同步异常
    // 最常见的是 SMC 调用
    save_x30
    apply_at_speculative_wa
    sync_and_handle_pending_serror
    handle_sync_exception  // 处理 SMC
end_vector_entry sync_exception_aarch64

vector_entry irq_aarch64
    // 从低异常级别（AArch64）进入 EL3 的 IRQ 中断
    save_x30
    apply_at_speculative_wa
    sync_and_handle_pending_serror
    b   handle_interrupt_exception
end_vector_entry irq_aarch64

vector_entry fiq_aarch64
    // 从低异常级别（AArch64）进入 EL3 的 FIQ 中断
    save_x30
    apply_at_speculative_wa
    sync_and_handle_pending_serror
    b   handle_interrupt_exception
end_vector_entry fiq_aarch64
```

**实际使用**:
- **SMC 调用**: NS-EL1 调用 SMC → EL3 的 `sync_exception_aarch64`
- **中断路由**: NS-EL1 发生中断，路由到 EL3 → EL3 的 `irq_aarch64` 或 `fiq_aarch64`
- **系统调用**: EL0 执行系统调用 → EL1 的 `sync_exception_aarch64`

#### 4. Lower EL using AArch32 (组 4: 0x600 - 0x800)

**使用条件**:
- 异常从**低异常级别**进入当前异常级别
- 低异常级别处于 **AArch32** 执行状态

**典型场景**:
- 在 EL0/EL1/EL2 执行 AArch32 代码时发生异常
- 异常被路由到更高的异常级别处理
- **兼容性**: 支持运行 32 位应用程序

**代码示例** (TF-A):
```assembly
// runtime_exceptions.S:268-279
vector_entry sync_exception_aarch32
    // 从低异常级别（AArch32）进入 EL3 的同步异常
    // 最常见的是 AArch32 SMC 调用
    save_x30
    apply_at_speculative_wa
    sync_and_handle_pending_serror
    handle_sync_exception  // 处理 AArch32 SMC
end_vector_entry sync_exception_aarch32

vector_entry irq_aarch32
    // 从低异常级别（AArch32）进入 EL3 的 IRQ 中断
    save_x30
    apply_at_speculative_wa
    sync_and_handle_pending_serror
    b   handle_interrupt_exception
end_vector_entry irq_aarch32

vector_entry fiq_aarch32
    // 从低异常级别（AArch32）进入 EL3 的 FIQ 中断
    save_x30
    apply_at_speculative_wa
    sync_and_handle_pending_serror
    b   handle_interrupt_exception
end_vector_entry fiq_aarch32
```

**实际使用**:
- **AArch32 SMC**: AArch32 代码调用 SMC → EL3 的 `sync_exception_aarch32`
- **32 位应用中断**: AArch32 应用运行时发生中断，路由到 EL3 → EL3 的 `irq_aarch32` 或 `fiq_aarch32`
- **兼容性支持**: 支持运行 32 位操作系统和应用

### 选择决策流程图

```
异常发生
    ↓
判断异常来源:
    ├─ 在当前异常级别发生?
    │   ├─ 是 → 检查栈指针选择
    │   │   ├─ SP_EL0? → 使用组 1 (0x000 - 0x200)
    │   │   └─ SP_ELx? → 使用组 2 (0x200 - 0x400)
    │   │
    │   └─ 否 → 从低异常级别进入
    │       ├─ 低异常级别是 AArch64? → 使用组 3 (0x400 - 0x600)
    │       └─ 低异常级别是 AArch32? → 使用组 4 (0x600 - 0x800)
    │
    └─ 确定异常类型:
        ├─ 同步异常 → 偏移 +0x000
        ├─ IRQ → 偏移 +0x080
        ├─ FIQ → 偏移 +0x100
        └─ SError → 偏移 +0x180
```

### 实际应用场景

#### 场景 1: NS-EL1 发生中断，路由到 EL3

```
NS-EL1 执行 AArch64 代码
    ↓
Group 1S 中断发生（例如：安全定时器）
    ↓
硬件路由到 EL3（SCR_EL3.FIQ = 1）
    ↓
CPU 硬件选择向量表:
    - 从低异常级别进入 → 是
    - 低异常级别是 AArch64 → 是
    - 异常类型是 FIQ → 是
    ↓
使用组 3，FIQ 入口:
    VBAR_EL3 + 0x400 + 0x100 = VBAR_EL3 + 0x500
    ↓
进入 fiq_aarch64 向量入口
    ↓
执行 handle_interrupt_exception()
```

#### 场景 2: NS-EL1 调用 SMC

```
NS-EL1 执行 AArch64 代码
    ↓
执行 SMC 指令
    ↓
同步异常，路由到 EL3
    ↓
CPU 硬件选择向量表:
    - 从低异常级别进入 → 是
    - 低异常级别是 AArch64 → 是
    - 异常类型是同步异常 → 是
    ↓
使用组 3，同步异常入口:
    VBAR_EL3 + 0x400 + 0x000 = VBAR_EL3 + 0x400
    ↓
进入 sync_exception_aarch64 向量入口
    ↓
执行 handle_sync_exception()
    ↓
识别为 SMC，调用 SMC handler
```

#### 场景 3: EL3 处理异常时又发生异常（嵌套异常）

```
EL3 正在处理一个中断（使用 SP_EL3）
    ↓
在处理过程中又发生另一个异常
    ↓
CPU 硬件选择向量表:
    - 在当前异常级别发生 → 是（仍在 EL3）
    - 当前使用 SP_EL3 → 是（SPSel = 1）
    - 异常类型是 IRQ → 是
    ↓
使用组 2，IRQ 入口:
    VBAR_EL3 + 0x200 + 0x080 = VBAR_EL3 + 0x280
    ↓
进入 irq_sp_elx 向量入口
    ↓
报告未处理的中断（嵌套异常通常表示严重错误）
```

#### 场景 4: AArch32 应用调用 SMC

```
EL0 执行 AArch32 应用代码
    ↓
执行 SMC 指令（32 位）
    ↓
同步异常，路由到 EL3
    ↓
CPU 硬件选择向量表:
    - 从低异常级别进入 → 是
    - 低异常级别是 AArch32 → 是
    - 异常类型是同步异常 → 是
    ↓
使用组 4，同步异常入口:
    VBAR_EL3 + 0x600 + 0x000 = VBAR_EL3 + 0x600
    ↓
进入 sync_exception_aarch32 向量入口
    ↓
执行 handle_sync_exception()
    ↓
识别为 AArch32 SMC，调用 SMC handler
```

### 栈指针选择 (SPSel)

**SPSel 寄存器** (Stack Pointer Selection) 控制使用哪个栈指针：

- **SPSel = 0**: 使用 **SP_EL0**（用户栈指针）
- **SPSel = 1**: 使用 **SP_ELx**（x 是当前异常级别，例如 SP_EL3）

**代码示例**:
```assembly
// 切换到 SP_EL0
msr spsel, #0  // 使用 SP_EL0

// 切换到 SP_EL3
msr spsel, #1  // 使用 SP_EL3
```

**为什么需要两个栈指针？**
- **SP_EL0**: 用于用户态代码，提供隔离
- **SP_ELx**: 用于内核/特权代码，提供独立的栈空间

### 执行状态检测

CPU 硬件通过 **SPSR_ELx** (Saved Program Status Register) 检测低异常级别的执行状态：

- **SPSR_ELx.M[4] = 0**: AArch64 执行状态
- **SPSR_ELx.M[4] = 1**: AArch32 执行状态

**代码示例**:
```c
// 检查执行状态
uint64_t spsr = read_spsr_el3();
if ((spsr & SPSR_M_MASK) == SPSR_MODE_AARCH64) {
    // AArch64 执行状态
} else {
    // AArch32 执行状态
}
```

### 异常向量表偏移量定义

```c
// arch.h
#define CURRENT_EL_SP0        0x0    // 组 1: Current EL with SP_EL0
#define CURRENT_EL_SPX        0x200  // 组 2: Current EL with SP_ELx
#define LOWER_EL_AARCH64      0x400  // 组 3: Lower EL using AArch64
#define LOWER_EL_AARCH32      0x600  // 组 4: Lower EL using AArch32

#define SYNC_EXCEPTION        0x0    // 同步异常偏移
#define IRQ_EXCEPTION         0x80   // IRQ 异常偏移
#define FIQ_EXCEPTION         0x100  // FIQ 异常偏移
#define SERROR_EXCEPTION      0x180  // SError 异常偏移
```

### 完整向量表地址计算

```
向量表入口地址 = VBAR_ELx + 组偏移 + 异常类型偏移

示例:
- EL3 的 FIQ 中断，从 AArch64 低异常级别进入:
  地址 = VBAR_EL3 + 0x400 + 0x100 = VBAR_EL3 + 0x500

- EL1 的同步异常，从 AArch32 低异常级别进入:
  地址 = VBAR_EL1 + 0x600 + 0x000 = VBAR_EL1 + 0x600
```

### 总结：异常向量表选择规则

1. **组 1 (Current EL with SP_EL0)**:
   - 当前异常级别，使用 SP_EL0
   - 偏移: 0x000 - 0x200
   - 典型场景: EL1/EL2 用户态异常

2. **组 2 (Current EL with SP_ELx)**:
   - 当前异常级别，使用 SP_ELx
   - 偏移: 0x200 - 0x400
   - 典型场景: 嵌套异常处理

3. **组 3 (Lower EL using AArch64)**:
   - 从低异常级别进入，AArch64 执行状态
   - 偏移: 0x400 - 0x600
   - 典型场景: **最常见的场景**，SMC 调用、中断路由

4. **组 4 (Lower EL using AArch32)**:
   - 从低异常级别进入，AArch32 执行状态
   - 偏移: 0x600 - 0x800
   - 典型场景: 32 位应用兼容性支持

**关键点**:
- CPU 硬件**自动选择**使用哪一组向量表
- 选择基于：当前异常级别、栈指针选择、低异常级别执行状态
- 不同的异常类型（同步、IRQ、FIQ、SError）使用不同的偏移量
- **组 3 是最常用的**，因为大多数异常都是从低异常级别进入 EL3

---

## GIC 中断类型详解：PPI 和 LPI

### GIC 中断类型概述

ARM GIC (Generic Interrupt Controller) 支持四种中断类型：

| 中断类型 | ID 范围 | 全称 | 特点 |
|---------|---------|------|------|
| **SGI** | 0-15 | Software Generated Interrupt | 软件生成中断，用于核间通信 |
| **PPI** | 16-31 | Private Peripheral Interrupt | 私有外设中断，每个 CPU 私有 |
| **SPI** | 32-1019 | Shared Peripheral Interrupt | 共享外设中断，所有 CPU 可见 |
| **LPI** | 8192+ | Locality-specific Peripheral Interrupt | 位置特定外设中断（GICv3+，基于消息） |

**中断 ID 定义**:
```c
// gic_common.h
#define MIN_SGI_ID      U(0)    // 0-15
#define MIN_PPI_ID      U(16)   // 16-31
#define MIN_SPI_ID      U(32)   // 32-1019
// LPI 从 8192 开始（GICv3+）
```

### PPI (Private Peripheral Interrupt) 详解

#### PPI 的定义和特点

**PPI (Private Peripheral Interrupt)** 是**每个 CPU 核心私有的中断**，ID 范围为 16-31。

**关键特点**:
1. **私有性**: 每个 CPU 核心有自己独立的 PPI 集合
2. **本地性**: PPI 只能被对应的 CPU 核心感知和处理
3. **固定范围**: ID 16-31，共 16 个中断
4. **硬件连接**: 直接连接到特定 CPU 核心的中断控制器

#### PPI 的典型用途

**1. 通用定时器中断**:
- **CNTP (Physical Timer)**: 物理定时器中断（PPI ID 通常为 30）
- **CNTV (Virtual Timer)**: 虚拟定时器中断（PPI ID 通常为 27）
- **CNTPS (Secure Physical Timer)**: 安全物理定时器中断（PPI ID 通常为 29）

**2. 性能监控单元 (PMU) 中断**:
- **PMU 溢出中断**: 性能计数器溢出时触发

**3. 看门狗定时器中断**:
- **安全看门狗**: 安全世界的看门狗定时器

**4. 调试中断**:
- **调试事件中断**: 调试相关的中断

#### PPI 在 Linux 内核中的使用

**1. 定时器中断**:

Linux 内核使用 PPI 处理每个 CPU 核心的本地定时器中断：

```c
// Linux 内核中的定时器中断处理（概念代码）
// 每个 CPU 核心都有自己的定时器中断（PPI）

// 定时器中断注册
void timer_setup_per_cpu(void)
{
    // 为每个 CPU 注册定时器中断处理程序
    request_percpu_irq(TIMER_PPI_ID,      // PPI ID 30 (CNTP)
                       timer_interrupt,   // 中断处理函数
                       "timer");
}

// 定时器中断处理
static irqreturn_t timer_interrupt(int irq, void *dev_id)
{
    // 更新系统时间
    update_wall_time();
    
    // 触发调度器
    scheduler_tick();
    
    // 更新进程统计信息
    update_process_times();
    
    return IRQ_HANDLED;
}
```

**2. 性能监控中断**:

```c
// PMU 中断处理（概念代码）
void pmu_setup_per_cpu(void)
{
    // 为每个 CPU 注册 PMU 中断处理程序
    request_percpu_irq(PMU_PPI_ID,        // PPI ID（通常为 26）
                       pmu_interrupt,
                       "pmu");
}

static irqreturn_t pmu_interrupt(int irq, void *dev_id)
{
    // 处理性能计数器溢出
    handle_pmu_overflow();
    
    return IRQ_HANDLED;
}
```

**3. 看门狗中断**:

```c
// 看门狗中断处理（概念代码）
void watchdog_setup_per_cpu(void)
{
    // 为每个 CPU 注册看门狗中断处理程序
    request_percpu_irq(WATCHDOG_PPI_ID,   // PPI ID（通常为 28）
                       watchdog_interrupt,
                       "watchdog");
}
```

**Linux 内核中的 PPI 使用场景总结**:

| PPI ID | 典型用途 | Linux 内核使用 |
|--------|---------|---------------|
| **16-25** | 平台特定 PPI | 平台驱动使用 |
| **26** | PMU 中断 | 性能监控 |
| **27** | CNTV (虚拟定时器) | 虚拟化环境 |
| **28** | 看门狗中断 | 系统监控 |
| **29** | CNTPS (安全定时器) | 安全世界（ATF） |
| **30** | CNTP (物理定时器) | **系统定时器（最常见）** |
| **31** | 平台特定 | 平台驱动使用 |

#### PPI 在 ATF 中的使用

**1. 安全定时器中断**:

ATF 使用 PPI 处理安全世界的定时器中断：

```c
// ATF 中的安全定时器中断处理
// bl32/tsp/tsp_timer.c

// 安全定时器中断 ID（PPI 29）
#define TSP_IRQ_SECURE_PHYSICAL_TIMER  29

// 定时器中断处理
void tsp_secure_timer_handler(void)
{
    // 处理安全定时器中断
    // 用于 TSP 的定时操作
}
```

**代码示例** (ATF):
```c
// bl32/tsp/tsp_timer.c:34-39
void tsp_timer_init(void)
{
    uint64_t cval;
    uint32_t ctl;
    
    // 设置安全物理定时器
    cval = read_cntpct_el0() + (read_cntfrq_el0() >> 1);
    write_cntps_cval_el1(cval);  // 设置比较值
    
    ctl = CNTP_CTL_ENABLE;
    set_cntp_ctl_imask(ctl, 0);   // 使能中断
    write_cntps_ctl_el1(ctl);     // 启动定时器
}
```

**2. SDEI 中的 PPI 使用**:

```c
// services/std_svc/sdei/sdei_main.c:212
// SDEI 支持将 PPI 绑定到事件
assert(plat_ic_is_ppi((unsigned) map->intr) != 0);

// SDEI 中的 PPI 绑定
int sdei_interrupt_bind(int intr_num, ...)
{
    // PPI 或 SPI 都可以绑定
    if (!(plat_ic_is_ppi(intr_num) || plat_ic_is_spi(intr_num)))
        return -1;
    
    // 绑定中断到 SDEI 事件
    // ...
}
```

**ATF 中的 PPI 使用场景总结**:

| 场景 | PPI ID | 用途 | 代码位置 |
|------|--------|------|---------|
| **安全定时器** | 29 | TSP 定时器中断 | `bl32/tsp/tsp_timer.c` |
| **SDEI 事件** | 16-31 | 私有事件绑定 | `services/std_svc/sdei/` |
| **平台特定** | 16-31 | 平台驱动使用 | 平台代码 |

#### PPI 的配置和管理

**1. PPI 的识别**:

```c
// plat_gicv3.c:210-213
bool plat_ic_is_ppi(unsigned int id)
{
    return (id >= MIN_PPI_ID) && (id < MIN_SPI_ID);
    // 判断 ID 是否在 16-31 范围内
}
```

**2. PPI 的配置**:

```c
// PPI 配置示例（概念代码）
void configure_ppi(unsigned int ppi_id, unsigned int cpu_id)
{
    // PPI 是每个 CPU 私有的，不需要配置目标 CPU
    // 只需要配置优先级、触发类型等
    
    // 设置优先级
    gicd_set_ipriorityr(ppi_id, priority);
    
    // 设置触发类型（边沿/电平）
    gicd_set_icfgr(ppi_id, trigger_type);
    
    // 使能中断
    gicd_set_isenabler(ppi_id);
}
```

**3. PPI 与 SPI 的区别**:

| 特性 | PPI | SPI |
|------|-----|-----|
| **ID 范围** | 16-31 | 32-1019 |
| **可见性** | 单个 CPU 私有 | 所有 CPU 可见 |
| **目标 CPU** | 固定（硬件连接） | 可配置（软件选择） |
| **典型用途** | 定时器、PMU、看门狗 | 外设中断（UART、网络等） |
| **配置位置** | 每个 CPU 独立配置 | 全局配置（GICD） |

### LPI (Locality-specific Peripheral Interrupt) 详解

#### LPI 的定义和特点

**LPI (Locality-specific Peripheral Interrupt)** 是 **GICv3/v4 中引入的基于消息的中断类型**，ID 从 8192 开始。

**关键特点**:
1. **基于消息**: 通过写入 GIC 寄存器触发，而非传统的中断线
2. **大规模支持**: 理论上支持数百万个中断（ID 从 8192 开始）
3. **内存表配置**: 配置信息存储在内存表中，而非寄存器
4. **ITS 支持**: 需要 ITS (Interrupt Translation Service) 组件
5. **虚拟化友好**: 特别适合虚拟化环境

#### LPI 的架构

**LPI 架构组件**:

```
外设设备
    ↓ 写入 GITS_TRANSLATER 寄存器
ITS (Interrupt Translation Service)
    ↓ 翻译消息为 LPI
LPI 属性表 (内存)
    ├─ 优先级
    ├─ 目标 CPU
    └─ 使能状态
    ↓
LPI 挂起表 (内存)
    └─ 挂起状态
    ↓
目标 CPU 接收中断
```

**关键组件**:
1. **ITS (Interrupt Translation Service)**: 将设备消息翻译为 LPI
2. **LPI 属性表**: 存储 LPI 的配置信息（优先级、目标 CPU 等）
3. **LPI 挂起表**: 存储 LPI 的挂起状态
4. **GITS_TRANSLATER**: 设备写入此寄存器触发 LPI

#### LPI 在 Linux 内核中的使用

**1. PCIe MSI/MSI-X 中断**:

LPI 最常见的用途是支持 PCIe 设备的 MSI/MSI-X 中断：

```c
// Linux 内核中的 PCIe MSI 中断处理（概念代码）

// PCIe 设备通过 ITS 触发 LPI
void pcie_msi_setup(struct pci_dev *dev)
{
    // 分配 LPI ID
    int lpi_id = its_alloc_lpi();
    
    // 配置 LPI 属性
    its_set_lpi_property(lpi_id, 
                         priority,    // 优先级
                         target_cpu,  // 目标 CPU
                         enabled);    // 使能状态
    
    // 映射设备到 LPI
    its_map_device(dev, lpi_id);
    
    // 设备写入 GITS_TRANSLATER 触发 LPI
}

// LPI 中断处理
static irqreturn_t pcie_msi_handler(int irq, void *dev_id)
{
    // 处理 PCIe 设备中断
    handle_pcie_interrupt();
    
    return IRQ_HANDLED;
}
```

**2. 虚拟化环境中的 LPI**:

在虚拟化环境中，LPI 用于虚拟设备的中断：

```c
// KVM 中的 LPI 使用（概念代码）
void kvm_setup_vlpi(struct kvm_vcpu *vcpu, int vlpi_id)
{
    // 为虚拟 CPU 设置虚拟 LPI
    its_set_vlpi(vcpu, vlpi_id);
    
    // 映射虚拟 LPI 到物理 LPI
    its_map_vlpi_to_plpi(vlpi_id, plpi_id);
}
```

**3. ITS 配置和管理**:

```c
// Linux 内核中的 ITS 初始化（概念代码）
void its_init(void)
{
    // 初始化 ITS 硬件
    its_hw_init();
    
    // 分配 LPI 属性表
    lpi_prop_table = alloc_lpi_prop_table();
    
    // 分配 LPI 挂起表
    lpi_pend_table = alloc_lpi_pend_table();
    
    // 配置 ITS 基地址
    its_set_base(lpi_prop_table, lpi_pend_table);
}
```

**Linux 内核中的 LPI 使用场景总结**:

| 场景 | 用途 | 优势 |
|------|------|------|
| **PCIe MSI/MSI-X** | PCIe 设备中断 | 支持大量设备，灵活路由 |
| **虚拟化** | 虚拟设备中断 | 高效的虚拟中断处理 |
| **大规模外设** | 需要大量中断源的设备 | 突破传统中断数量限制 |

#### LPI 在 ATF 中的使用

**1. 安全世界的 LPI 管理**:

ATF 需要管理安全世界的 LPI 配置：

```c
// ATF 中的 LPI 管理（概念代码）
void el3_lpi_setup(void)
{
    // 初始化安全世界的 ITS
    its_init_secure();
    
    // 配置安全 LPI 属性表
    setup_secure_lpi_prop_table();
    
    // 确保非安全世界不能访问安全 LPI
    configure_lpi_security();
}
```

**2. 虚拟化支持**:

在支持虚拟化的系统中，ATF 需要管理 LPI 的安全属性：

```c
// ATF 中的虚拟化 LPI 管理（概念代码）
void el3_vlpi_manage(void)
{
    // 管理虚拟 LPI 到物理 LPI 的映射
    // 确保安全隔离
    // ...
}
```

**ATF 中的 LPI 使用场景总结**:

| 场景 | 用途 | 说明 |
|------|------|------|
| **安全 LPI 管理** | 安全世界的 LPI 配置 | 确保安全隔离 |
| **虚拟化支持** | 虚拟 LPI 管理 | 支持安全虚拟化 |
| **ITS 初始化** | ITS 硬件初始化 | EL3 初始化 ITS |

#### LPI 的配置和管理

**1. LPI 属性表结构**:

```c
// LPI 属性表项（每个 LPI 一个字节）
struct lpi_property {
    uint8_t priority:4;      // 优先级（4 位）
    uint8_t enabled:1;        // 使能位
    uint8_t group:1;         // 组（Group 0/1）
    uint8_t reserved:2;      // 保留位
};
```

**2. LPI 挂起表结构**:

```c
// LPI 挂起表（每个 LPI 一个位）
// 位 = 1 表示中断挂起
// 位 = 0 表示中断未挂起
```

**3. ITS 命令**:

```c
// ITS 命令示例（概念代码）
void its_map_device_to_lpi(struct device *dev, int lpi_id)
{
    // 发送 ITS MAPD 命令
    // 映射设备到 LPI
    its_send_command(ITS_CMD_MAPD, dev, lpi_id);
}

void its_map_interrupt(int device_id, int event_id, int lpi_id)
{
    // 发送 ITS MAPTI 命令
    // 映射中断事件到 LPI
    its_send_command(ITS_CMD_MAPTI, device_id, event_id, lpi_id);
}
```

#### LPI 与传统中断的对比

| 特性 | 传统中断 (SGI/PPI/SPI) | LPI |
|------|----------------------|-----|
| **触发方式** | 硬件中断线 | 消息写入寄存器 |
| **配置存储** | GIC 寄存器 | 内存表 |
| **数量限制** | 有限（最多 1020 个） | 理论上无限制（从 8192 开始） |
| **路由方式** | 硬件路由 | 软件配置（ITS） |
| **虚拟化支持** | 需要软件模拟 | 原生支持 |
| **延迟** | 较低（硬件路由） | 稍高（需要 ITS 翻译） |
| **适用场景** | 传统外设 | 大规模外设、虚拟化 |

### IPI (Inter-Processor Interrupt) 详解

#### 为什么内核需要 IPI？

在多处理器（SMP）系统中，**IPI (Inter-Processor Interrupt)** 是处理器之间通信和协调的关键机制。IPI 在 ARM 架构中通过 **SGI (Software Generated Interrupt)** 实现。

**核心原因**:
1. **多核协调**: 多个 CPU 核心需要协调工作，不能只依赖共享内存
2. **实时通知**: 需要实时通知其他 CPU 核心执行特定操作
3. **缓存一致性**: 维护多核系统的缓存一致性
4. **任务调度**: 在多核间迁移和调度任务
5. **系统管理**: 系统级别的操作需要所有 CPU 核心参与

#### IPI 的实现方式

**在 ARM 架构中，IPI 通过 SGI 实现**:

```c
// ARM GIC 中，SGI 用于实现 IPI
// SGI ID: 0-15

// 发送 IPI 的寄存器
// ICC_SGI0R_EL1: SGI 0-7
// ICC_SGI1R_EL1: SGI 8-15
```

**IPI 发送方式**:
1. **广播 IPI**: 发送给所有 CPU 核心
2. **目标 IPI**: 发送给特定的 CPU 核心
3. **组 IPI**: 发送给一组 CPU 核心

#### Linux 内核中 IPI 的使用场景

**1. TLB 刷新 (TLB Shootdown)**:

当某个 CPU 修改了页表时，需要通知其他 CPU 刷新 TLB：

```c
// Linux 内核中的 TLB 刷新（概念代码）

// CPU 0 修改了页表
void flush_tlb_all(void)
{
    // 发送 IPI 给所有其他 CPU
    smp_call_function(flush_tlb_local, NULL, 1);
    
    // 本地 CPU 也刷新 TLB
    flush_tlb_local(NULL);
}

// IPI 处理函数
static void flush_tlb_local(void *info)
{
    // 刷新本地 CPU 的 TLB
    __flush_tlb_all();
}
```

**为什么需要 IPI？**
- CPU 0 修改页表后，其他 CPU 的 TLB 中可能还有旧的映射
- 必须通知所有 CPU 刷新 TLB，否则会导致地址转换错误
- **IPI 确保所有 CPU 同步刷新 TLB**

**2. 缓存刷新 (Cache Flush)**:

当需要保持缓存一致性时，使用 IPI 通知其他 CPU：

```c
// Linux 内核中的缓存刷新（概念代码）

void flush_cache_all(void)
{
    // 发送 IPI 给所有 CPU，刷新缓存
    on_each_cpu(flush_cache_local, NULL, 1);
}

static void flush_cache_local(void *info)
{
    // 刷新本地 CPU 的缓存
    __flush_cache_all();
}
```

**3. 任务调度和唤醒 (Scheduler Wake-up)**:

当需要唤醒其他 CPU 上的任务时，使用 IPI：

```c
// Linux 内核中的调度器唤醒（概念代码）

// CPU 0 需要唤醒 CPU 1 上的任务
void wake_up_idle_cpu(int cpu)
{
    // 发送 IPI 给目标 CPU
    smp_send_reschedule(cpu);
}

// IPI 处理函数
void smp_reschedule_interrupt(void)
{
    // 触发调度器，检查是否有新任务
    scheduler_ipi();
}
```

**为什么需要 IPI？**
- CPU 0 发现 CPU 1 上有高优先级任务需要运行
- CPU 1 可能处于 idle 状态，需要被唤醒
- **IPI 确保 CPU 1 及时响应并执行任务**

**4. 函数调用 (Function Call)**:

在多核系统中，需要所有 CPU 执行某个函数：

```c
// Linux 内核中的函数调用（概念代码）

// 需要在所有 CPU 上执行某个函数
void smp_call_function(void (*func)(void *), void *info, int wait)
{
    // 发送 IPI 给所有其他 CPU
    // 每个 CPU 收到 IPI 后执行 func(info)
    // ...
}

// 示例：在所有 CPU 上禁用中断
void disable_interrupts_all_cpus(void)
{
    smp_call_function(disable_local_irqs, NULL, 1);
}
```

**5. 停止其他 CPU (Stop Other CPUs)**:

系统关机或重启时，需要停止其他 CPU：

```c
// Linux 内核中的停止 CPU（概念代码）

void smp_send_stop(void)
{
    // 发送 IPI 给所有其他 CPU，要求它们停止
    smp_call_function(stop_this_cpu, NULL, 0);
}

static void stop_this_cpu(void *unused)
{
    // 当前 CPU 停止运行
    local_irq_disable();
    while (1)
        cpu_relax();
}
```

**6. 性能监控 (Performance Monitoring)**:

性能监控工具需要同步所有 CPU 的状态：

```c
// Linux 内核中的性能监控（概念代码）

void perf_event_interrupt_all_cpus(void)
{
    // 发送 IPI 给所有 CPU，收集性能数据
    smp_call_function(collect_perf_data, NULL, 1);
}
```

#### Linux 内核中 IPI 的类型

Linux 内核定义了多种 IPI 类型，每种对应不同的 SGI ID：

| IPI 类型 | SGI ID | 用途 | 说明 |
|---------|--------|------|------|
| **RESCHEDULE** | 通常为 0 | 触发调度 | 唤醒 CPU 进行任务调度 |
| **CALL_FUNCTION** | 通常为 1 | 函数调用 | 在所有 CPU 上执行函数 |
| **CALL_FUNCTION_SINGLE** | 通常为 2 | 单 CPU 函数调用 | 在特定 CPU 上执行函数 |
| **TLB_FLUSH** | 通常为 3 | TLB 刷新 | 刷新所有 CPU 的 TLB |
| **IRQ_WORK** | 通常为 4 | 中断工作 | 延迟执行的工作 |
| **WAKEUP** | 通常为 5 | 唤醒 CPU | 唤醒 idle 的 CPU |

**代码示例** (Linux 内核概念):
```c
// Linux 内核中的 IPI 类型定义（概念代码）
enum ipi_msg_type {
    IPI_RESCHEDULE,           // 重新调度
    IPI_CALL_FUNCTION,        // 函数调用
    IPI_CALL_FUNCTION_SINGLE, // 单 CPU 函数调用
    IPI_CPU_STOP,            // 停止 CPU
    IPI_TIMER,                // 定时器
    IPI_IRQ_WORK,             // 中断工作
    IPI_WAKEUP,               // 唤醒
    // ...
};

// 发送 IPI
void smp_send_ipi(int cpu, enum ipi_msg_type msg)
{
    // 通过 GIC 发送 SGI
    gic_send_sgi(msg, cpu);
}
```

#### IPI 的工作流程

**典型的 IPI 工作流程**:

```
CPU 0 需要通知 CPU 1
    ↓
CPU 0 写入 GIC 寄存器（ICC_SGI1R_EL1）
    ↓
GIC 生成 SGI 中断
    ↓
SGI 路由到 CPU 1
    ↓
CPU 1 接收中断，进入中断处理程序
    ↓
CPU 1 执行 IPI 处理函数
    ↓
CPU 1 完成操作，返回
```

**代码示例** (概念):
```c
// CPU 0 发送 IPI 给 CPU 1
void send_ipi_to_cpu1(void)
{
    // 写入 GIC 寄存器，发送 SGI
    write_sysreg(ICC_SGI1R_EL1, 
                 SGI_TARGET_LIST(1) |  // 目标 CPU 1
                 SGI_INTID(IPI_RESCHEDULE));  // SGI ID
}

// CPU 1 的 IPI 处理函数
void handle_ipi(void)
{
    unsigned int ipi = get_pending_ipi();
    
    switch (ipi) {
    case IPI_RESCHEDULE:
        scheduler_ipi();
        break;
    case IPI_CALL_FUNCTION:
        generic_smp_call_function_interrupt();
        break;
    case IPI_TLB_FLUSH:
        flush_tlb_local(NULL);
        break;
    // ...
    }
}
```

#### 为什么不能只用共享内存？

**问题**: 为什么需要 IPI，不能只用共享内存通信？

**答案**: 共享内存通信有以下问题：

1. **轮询开销**: CPU 需要不断轮询共享内存，浪费 CPU 资源
2. **延迟**: 轮询方式延迟高，无法实时响应
3. **CPU 唤醒**: 如果目标 CPU 处于 idle 状态，无法及时响应
4. **同步问题**: 需要额外的同步机制（锁、原子操作等）

**IPI 的优势**:
1. **实时通知**: 硬件中断机制，实时响应
2. **低延迟**: 直接触发中断，延迟低
3. **唤醒 CPU**: 可以唤醒 idle 的 CPU
4. **硬件保证**: 硬件保证中断的可靠传递

#### IPI 在 ATF 中的使用

**1. 安全世界的核间通信**:

```c
// ATF 中的安全 IPI（概念代码）
void secure_ipi_send(int target_cpu, int ipi_id)
{
    // 发送安全 IPI（SGI）
    // 用于安全世界内部的核间通信
    gic_raise_secure_sgi(ipi_id, target_cpu);
}
```

**2. 电源管理协调**:

```c
// ATF 中的电源管理 IPI（概念代码）
void psci_cpu_on_ipi(int target_cpu)
{
    // 发送 IPI 唤醒目标 CPU
    // 用于 CPU_ON 操作
    gic_raise_sgi(IPI_CPU_ON, target_cpu);
}
```

#### IPI 的性能考虑

**IPI 的开销**:
1. **发送开销**: 写入 GIC 寄存器（几个 CPU 周期）
2. **中断处理**: 中断处理程序执行（几十到几百个 CPU 周期）
3. **缓存影响**: 可能影响缓存性能

**优化策略**:
1. **批量操作**: 合并多个 IPI 请求
2. **延迟执行**: 非关键操作可以延迟执行
3. **选择性发送**: 只发送给需要的 CPU

#### IPI 使用场景总结

| 场景 | 为什么需要 IPI | 替代方案的问题 |
|------|---------------|---------------|
| **TLB 刷新** | 必须实时通知所有 CPU | 轮询延迟高，可能遗漏 |
| **缓存刷新** | 保持缓存一致性 | 轮询无法保证实时性 |
| **任务调度** | 唤醒 idle CPU | 共享内存无法唤醒 CPU |
| **函数调用** | 在所有 CPU 上执行函数 | 轮询开销大，延迟高 |
| **系统管理** | 协调所有 CPU | 需要可靠的同步机制 |

#### 总结：为什么内核需要 IPI？

**核心原因**:
1. **多核协调**: 多核系统需要实时协调机制
2. **缓存一致性**: 维护多核系统的缓存和 TLB 一致性
3. **任务调度**: 在多核间高效调度和迁移任务
4. **系统管理**: 系统级别的操作需要所有 CPU 参与
5. **实时性**: 需要实时通知，不能依赖轮询

**IPI 的优势**:
- ✅ **实时通知**: 硬件中断机制，实时响应
- ✅ **低延迟**: 直接触发中断，延迟低
- ✅ **唤醒能力**: 可以唤醒 idle 的 CPU
- ✅ **可靠性**: 硬件保证中断的可靠传递
- ✅ **效率**: 比轮询方式更高效

**没有 IPI 的问题**:
- ❌ **延迟高**: 轮询方式延迟高
- ❌ **无法唤醒**: 无法唤醒 idle 的 CPU
- ❌ **资源浪费**: 轮询浪费 CPU 资源
- ❌ **同步复杂**: 需要复杂的同步机制

**结论**: IPI 是多核系统中**不可或缺的机制**，它提供了高效、实时、可靠的处理器间通信方式，是 Linux 内核多核支持的基础。

### 四种中断类型对比总结

| 中断类型 | ID 范围 | 特点 | Linux 使用 | ATF 使用 |
|---------|---------|------|-----------|---------|
| **SGI** | 0-15 | 软件生成，核间通信 | **IPI、调度** | 安全核间通信 |
| **PPI** | 16-31 | 每个 CPU 私有 | 定时器、PMU | 安全定时器 |
| **SPI** | 32-1019 | 共享外设中断 | 外设中断（UART、网络等） | 平台外设中断 |
| **LPI** | 8192+ | 基于消息，大规模支持 | PCIe MSI、虚拟化 | 安全 LPI 管理 |

### 实际使用示例

#### 示例 1: Linux 内核中的定时器中断（PPI）

```c
// Linux 内核定时器中断处理流程

1. 系统启动时：
   - 为每个 CPU 注册定时器中断（PPI 30）
   - 配置定时器硬件

2. 定时器中断发生时：
   - 硬件触发 PPI 30
   - CPU 接收中断，进入中断处理程序
   - 更新系统时间
   - 触发调度器

3. 中断处理完成：
   - 清除中断挂起位
   - 返回被中断的上下文
```

#### 示例 2: Linux 内核中的 PCIe MSI 中断（LPI）

```c
// Linux 内核 PCIe MSI 中断处理流程

1. PCIe 设备初始化时：
   - 分配 LPI ID（例如：8192）
   - 配置 LPI 属性（优先级、目标 CPU）
   - 映射设备到 LPI

2. 设备需要发送中断时：
   - 设备写入 GITS_TRANSLATER 寄存器
   - ITS 将消息翻译为 LPI
   - LPI 路由到目标 CPU

3. CPU 接收中断：
   - 读取 LPI 挂起表
   - 调用对应的中断处理程序
   - 处理设备中断
```

#### 示例 3: ATF 中的安全定时器中断（PPI）

```c
// ATF 安全定时器中断处理流程

1. TSP 初始化时：
   - 配置安全物理定时器（CNTPS）
   - 注册定时器中断处理程序（PPI 29）

2. 定时器中断发生时：
   - 硬件触发 PPI 29
   - 中断路由到 EL3
   - EL3 委托给 TSP（S-EL1）
   - TSP 处理定时器中断

3. 中断处理完成：
   - TSP 完成定时操作
   - 返回 EL3，再返回非安全世界
```

### 总结

**PPI 和 LPI 的关键区别**:

1. **PPI**:
   - **用途**: 每个 CPU 核心的私有外设中断
   - **典型应用**: 定时器、PMU、看门狗
   - **配置**: 每个 CPU 独立配置
   - **Linux**: 主要用于定时器和性能监控
   - **ATF**: 主要用于安全定时器

2. **LPI**:
   - **用途**: 基于消息的大规模中断
   - **典型应用**: PCIe MSI、虚拟化设备
   - **配置**: 通过 ITS 和内存表配置
   - **Linux**: 主要用于 PCIe 设备和虚拟化
   - **ATF**: 主要用于安全 LPI 管理

**选择建议**:
- **使用 PPI**: 当需要每个 CPU 核心的私有中断（如定时器）
- **使用 SPI**: 当需要共享外设中断（如 UART、网络）
- **使用 LPI**: 当需要大量中断源或虚拟化支持（如 PCIe 设备）

---

## 各种场景下的中断处理

### 场景 1: 在 NS-EL1 执行时发生 Group 1NS 中断

**路由决定因素**: 取决于 NS 中断的路由模型配置和 SCR_EL3.IRQ 位的设置。

#### 情况 A: 路由到 NS-EL1（默认情况）

```
┌─────────────────────────────────────────┐
│ NS-EL1 执行中                            │
│ (SCR_EL3.IRQ = 0, SCR_EL3.NS = 1)      │
│ 路由模型: INTR_NS_VALID_RM0 (默认)       │
└─────────────────────────────────────────┘
              │
              │ Group 1NS 中断发生
              ▼
┌─────────────────────────────────────────┐
│ 硬件检查:                                │
│ - 中断是 Group 1NS (非安全中断)         │
│ - SCR_EL3.IRQ = 0 (不路由到 EL3)        │
│ → 中断直接路由到 NS-EL1                  │
└─────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────┐
│ NS-EL1 中断处理程序执行                   │
│ (无需上下文切换，硬件自动处理)            │
└─────────────────────────────────────────┘
```

**代码路径**:
- 中断直接由 NS-EL1 的异常向量处理
- 无需 EL3 介入
- 这是**默认和最常见**的情况

#### 情况 B: 路由到 EL3（特殊配置）

```
┌─────────────────────────────────────────┐
│ NS-EL1 执行中                            │
│ (SCR_EL3.IRQ = 1, SCR_EL3.NS = 1)      │
│ 路由模型: 特殊配置（需要显式设置）        │
└─────────────────────────────────────────┘
              │
              │ Group 1NS 中断发生
              ▼
┌─────────────────────────────────────────┐
│ 硬件检查:                                │
│ - 中断是 Group 1NS (非安全中断)         │
│ - SCR_EL3.IRQ = 1 (路由到 EL3)         │
│ → 中断路由到 EL3                         │
└─────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────┐
│ EL3 异常向量                             │
│ → handle_interrupt_exception()           │
└─────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────┐
│ EL3 NS 中断处理程序                       │
│ (例如: 需要 EL3 进行特殊处理)            │
└─────────────────────────────────────────┘
```

**代码路径**:
- 需要显式配置路由模型，设置 SCR_EL3.IRQ = 1
- 这种情况**不常见**，通常用于需要 EL3 统一管理 NS 中断的场景

**关键点**:
- **默认情况（RM0）**: NS 中断从 NS-EL1 执行时，SCR_EL3.IRQ = 0，直接路由到 NS-EL1
- **特殊配置**: 如果设置了 SCR_EL3.IRQ = 1，NS 中断会路由到 EL3
- **实际使用**: 大多数情况下，NS 中断在 NS-EL1 执行时直接由 NS-EL1 处理，不需要 EL3 介入

#### NS 中断路由决定因素详解

**什么时候路由到 NS-EL1（默认）**:
- **路由模型**: INTR_NS_VALID_RM0 或 INTR_NS_VALID_RM1（两者都设置 BIT[1] = 0）
- **SCR_EL3.IRQ = 0**: 不路由到 EL3
- **结果**: 中断直接由 NS-EL1 的异常向量处理
- **这是标准行为**: 非安全中断在非安全世界执行时，通常直接由非安全世界处理

**什么时候路由到 EL3（特殊配置）**:
- **需要显式设置**: SCR_EL3.IRQ = 1
- **路由模型**: 需要修改默认路由模型，设置 BIT[1] = 1
- **使用场景**:
  - 需要 EL3 统一管理所有中断
  - 需要 EL3 进行中断过滤、监控或统计
  - 需要 EL3 进行调试或性能分析
- **实际代码**: 这种情况**很少使用**，因为会带来性能开销

**路由模型对比**:

| 路由模型 | Flags 值 | BIT[1] (NS) | SCR_EL3.IRQ | 从 NS-EL1 路由到 | 说明 |
|---------|---------|-------------|-------------|----------------|------|
| **RM0** | 0x0 (0b00) | 0 | 0 | **NS-EL1** | 默认路由，直接处理 |
| **RM1** | 0x1 (0b01) | 0 | 0 | **NS-EL1** | 从 Secure 路由到 EL3，从 NS 仍路由到 NS-EL1 |
| **特殊** | 需要设置 | 1 | 1 | **EL3** | 需要显式配置，不常见 |

**总结**:
- **99% 的情况**: NS 中断从 NS-EL1 执行时，直接路由到 NS-EL1（SCR_EL3.IRQ = 0）
- **1% 的情况**: 特殊配置下，NS 中断路由到 EL3（SCR_EL3.IRQ = 1）
- **原因**: 非安全中断在非安全世界执行时，通常不需要 EL3 介入，直接处理更高效

#### NS 中断路由到 NS-EL1 还是 NS-EL2？

**关键概念: FEL (First Exception Level)**

FEL 是指**能够处理中断的第一个异常级别**，也就是**当前执行的异常级别**。

**路由规则**:
- 当 `SCR_EL3.IRQ = 0` 时，NS 中断路由到 **FEL**（当前执行的 EL）
- **如果当前在 NS-EL1 执行** → FEL = NS-EL1 → 中断路由到 NS-EL1
- **如果当前在 NS-EL2 执行** → FEL = NS-EL2 → 中断路由到 NS-EL2

**硬件自动决定**:
- 硬件会根据当前执行的异常级别自动选择路由目标
- 不需要软件配置，这是 ARM 架构的硬件行为

**示例场景**:

**场景 A: 在 NS-EL1 执行时发生 NS 中断**
```
当前执行: NS-EL1
SCR_EL3.IRQ = 0
→ FEL = NS-EL1
→ 中断路由到 NS-EL1
```

**场景 B: 在 NS-EL2 执行时发生 NS 中断**
```
当前执行: NS-EL2 (Hypervisor)
SCR_EL3.IRQ = 0
HCR_EL2.IMO = 1 (允许路由到 EL2)
→ FEL = NS-EL2
→ 中断路由到 NS-EL2
```

**场景 C: 在 NS-EL2 执行，但 HCR_EL2.IMO = 0**
```
当前执行: NS-EL2
SCR_EL3.IRQ = 0
HCR_EL2.IMO = 0 (不允许路由到 EL2)
→ 中断路由到 NS-EL1 (降级到 EL1)
```

**路由决策流程图**:

```
NS 中断 (Group 1NS) 发生
    ↓
检查 SCR_EL3.IRQ
    ├─ IRQ = 1 → 路由到 EL3
    └─ IRQ = 0 → 检查当前执行级别
            ├─ 当前在 NS-EL1 
            │   → FEL = NS-EL1
            │   → 路由到 NS-EL1 ✓
            │
            └─ 当前在 NS-EL2 
                → 检查 HCR_EL2.IMO
                    ├─ IMO = 1 
                    │   → FEL = NS-EL2
                    │   → 路由到 NS-EL2 ✓
                    └─ IMO = 0 
                        → 降级路由到 NS-EL1 ✓
```

**关键点**:
- **路由到哪个 EL 由当前执行级别决定**（硬件自动）
- **NS-EL1**: 如果当前在 NS-EL1 执行，NS 中断路由到 NS-EL1（FEL = EL1）
- **NS-EL2**: 如果当前在 NS-EL2 执行，且 HCR_EL2.IMO = 1，NS 中断路由到 NS-EL2（FEL = EL2）
- **HCR_EL2 控制**: 即使当前在 NS-EL2，如果 HCR_EL2.IMO = 0，中断仍会降级路由到 NS-EL1
- **FEL 概念**: FEL = First Exception Level = 当前执行的异常级别

#### 总结：NS 中断路由到 NS-EL1 还是 NS-EL2？

| 条件 | 路由目标 | 说明 |
|------|---------|------|
| **当前在 NS-EL1 执行**<br>SCR_EL3.IRQ = 0 | **NS-EL1** | FEL = EL1，硬件自动路由到当前 EL |
| **当前在 NS-EL2 执行**<br>SCR_EL3.IRQ = 0<br>HCR_EL2.IMO = 1 | **NS-EL2** | FEL = EL2，硬件自动路由到当前 EL |
| **当前在 NS-EL2 执行**<br>SCR_EL3.IRQ = 0<br>HCR_EL2.IMO = 0 | **NS-EL1** | 降级路由，因为 EL2 不允许处理中断 |
| **任何情况**<br>SCR_EL3.IRQ = 1 | **EL3** | 强制路由到 EL3 |

**核心原则**:
1. **硬件自动决定**: 路由到 NS-EL1 还是 NS-EL2 由**当前执行的异常级别**决定（FEL）
2. **NS-EL1**: 如果当前在 NS-EL1，中断路由到 NS-EL1
3. **NS-EL2**: 如果当前在 NS-EL2，且 HCR_EL2.IMO = 1，中断路由到 NS-EL2
4. **降级规则**: 如果 HCR_EL2.IMO = 0，即使当前在 NS-EL2，中断也会降级到 NS-EL1

---

### 场景 2: 在 NS-EL1 执行时发生 Group 1S 中断

**流程说明**: 当 NS-EL1 执行时发生 Group 1S 中断，硬件会路由到 EL3。EL3 异常向量调用中断管理框架，框架根据中断类型查找并调用注册的 handler（`tspd_sel1_interrupt_handler`），handler 负责上下文切换并跳转到 S-EL1。

```
┌─────────────────────────────────────────┐
│ NS-EL1 执行中                            │
│ (SCR_EL3.IRQ = 0, SCR_EL3.NS = 1)      │
└─────────────────────────────────────────┘
              │
              │ Group 1S 中断发生
              ▼
┌─────────────────────────────────────────┐
│ 硬件检查:                                │
│ - 中断是 Group 1S (安全中断)             │
│ - 当前在 NS 世界，但中断是安全的          │
│ → 硬件强制路由到 EL3                     │
└─────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────┐
│ EL3 异常向量 (irq_aarch64/fiq_aarch64)  │
│ → 跳转到 handle_interrupt_exception()   │
└─────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────┐
│ handle_interrupt_exception()             │
│ 1. 保存 EL3 状态                         │
│ 2. 调用 plat_ic_get_pending_interrupt_type()│
│    获取中断类型 (INTR_TYPE_S_EL1)       │
│ 3. 调用 get_interrupt_type_handler()     │
│    获取注册的 handler                    │
└─────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────┐
│ 调用 tspd_sel1_interrupt_handler()      │
│ (这是 TSPD 注册的 S-EL1 中断 handler)   │
└─────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────┐
│ tspd_sel1_interrupt_handler() 执行:      │
│ 1. 保存 NS 上下文                        │
│ 2. 恢复 Secure 上下文                    │
│ 3. 设置 ELR_EL3 指向 TSP 中断入口        │
│ 4. 返回，触发 ERET 到 S-EL1              │
└─────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────┐
│ S-EL1 中断处理程序执行                    │
│ (TSP 中断处理，例如定时器中断)            │
└─────────────────────────────────────────┘
```

**代码路径**:

1. **EL3 异常向量** (`runtime_exceptions.S`):
```assembly
vector_entry irq_aarch64
    b   handle_interrupt_exception
end_vector_entry irq_aarch64

func handle_interrupt_exception
    // 1. 保存 EL3 状态
    bl  prepare_el3_entry
    
    // 2. 获取中断类型
    bl  plat_ic_get_pending_interrupt_type
    // 返回: INTR_TYPE_S_EL1
    
    // 3. 获取注册的 handler
    bl  get_interrupt_type_handler
    // 返回: tspd_sel1_interrupt_handler 的地址
    
    // 4. 调用 handler
    blr x21  // x21 = handler 地址
endfunc
```

2. **TSPD Handler** (`tspd_main.c`):
```c
static uint64_t tspd_sel1_interrupt_handler(uint32_t id,
                                            uint32_t flags,
                                            void *handle,
                                            void *cookie)
{
    // 1. 获取 TSP 上下文
    tsp_ctx = &tspd_sp_context[linear_id];
    
    // 2. 保存当前状态（如果是 NS 上下文）
    if (handle == cm_get_context(NON_SECURE)) {
        cm_el1_sysregs_context_save(NON_SECURE);
    }
    
    // 3. 恢复 Secure 上下文
    cm_el1_sysregs_context_restore(SECURE);
    
    // 4. 设置 ELR_EL3 指向 TSP 中断入口
    cm_set_elr_spsr_el3(SECURE, 
                        (uint64_t)&tsp_vectors->sel1_intr_entry,
                        SPSR_64(MODE_EL1, ...));
    
    // 5. 返回后，handle_interrupt_exception 会执行 ERET
    //    跳转到 S-EL1 执行 TSP 中断处理
    return 0;
}
```

**关键点说明**:
- **EL3 异常向量** (`irq_aarch64`) 是硬件路由的入口点，由硬件自动跳转
- **`handle_interrupt_exception`** 是 EL3 中断管理框架的统一处理函数
- 它通过 `get_interrupt_type_handler` 查找注册的 handler
- **TSPD 在初始化时**注册了 `tspd_sel1_interrupt_handler` 作为 S-EL1 中断的 handler
- Handler 负责上下文切换和设置跳转地址，然后通过 ERET 跳转到 S-EL1

---

### 场景 3: 在 S-EL1 执行时发生 Group 1NS 中断

```
┌─────────────────────────────────────────┐
│ S-EL1 执行中 (TSP)                      │
│ (SCR_EL3.IRQ = 0, SCR_EL3.NS = 0)      │
└─────────────────────────────────────────┘
              │
              │ Group 1NS 中断发生
              ▼
┌─────────────────────────────────────────┐
│ 硬件检查:                                │
│ - 中断是 Group 1NS (非安全中断)         │
│ - 当前在 Secure 世界                     │
│ → 必须路由到 EL3 进行上下文切换          │
└─────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────┐
│ EL3 异常向量                             │
│ → tspd_handle_sp_preemption()           │
└─────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────┐
│ 保存 Secure 上下文                       │
│ 恢复 NS 上下文                           │
│ 返回 NS-EL1，SMC_PREEMPTED               │
└─────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────┐
│ NS-EL1 中断处理程序执行                   │
└─────────────────────────────────────────┘
```

**代码路径** (`tspd_main.c`):
```c
uint64_t tspd_handle_sp_preemption(void *handle)
{
    // 1. 保存 Secure 上下文
    cm_el1_sysregs_context_save(SECURE);
    
    // 2. 恢复 NS 上下文
    ns_cpu_context = cm_get_context(NON_SECURE);
    cm_el1_sysregs_context_restore(NON_SECURE);
    cm_set_next_eret_context(NON_SECURE);
    
    // 3. 返回 SMC_PREEMPTED 表示被抢占
    SMC_RET1(ns_cpu_context, SMC_PREEMPTED);
}
```

---

### 场景 4: 在 S-EL1 执行时发生 Group 1S 中断

```
┌─────────────────────────────────────────┐
│ S-EL1 执行中 (TSP)                      │
│ (SCR_EL3.IRQ = 0, SCR_EL3.NS = 0)      │
└─────────────────────────────────────────┘
              │
              │ Group 1S 中断发生
              ▼
┌─────────────────────────────────────────┐
│ 硬件检查:                                │
│ - 中断是 Group 1S (安全中断)             │
│ - 当前在 Secure 世界                     │
│ - SCR_EL3.IRQ = 0                        │
│ → 直接路由到 S-EL1                       │
└─────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────┐
│ S-EL1 中断处理程序执行                    │
│ (TSP 中断处理，无需上下文切换)            │
└─────────────────────────────────────────┘
```

**代码路径** (`tsp_interrupt.c`):
```c
uint64_t tsp_sel1_interrupt_handler(uint32_t id,
                                    uint32_t flags,
                                    void *handle,
                                    void *cookie)
{
    // 直接处理安全中断，无需上下文切换
    if (id == TSP_IRQ_SEC_PHY_TIMER) {
        // 处理安全定时器中断
        tsp_generic_timer_handler();
    }
    
    return 0;
}
```

---

### 场景 5: 在 NS-EL1 执行时发生 Group 0 中断

```
┌─────────────────────────────────────────┐
│ NS-EL1 执行中                            │
│ (SCR_EL3.IRQ = 0, SCR_EL3.NS = 1)      │
└─────────────────────────────────────────┘
              │
              │ Group 0 中断发生
              ▼
┌─────────────────────────────────────────┐
│ 硬件检查:                                │
│ - 中断是 Group 0 (总是路由到 EL3)       │
│ → 强制路由到 EL3                         │
└─────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────┐
│ EL3 异常向量                             │
│ → EL3 中断处理程序                        │
└─────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────┐
│ EL3 中断处理 (例如: 安全看门狗)          │
│ 处理完成后返回原上下文                    │
└─────────────────────────────────────────┘
```

---

### 场景 6: 在 NS-EL2 执行时发生 Group 1NS 中断

```
┌─────────────────────────────────────────┐
│ NS-EL2 执行中 (Hypervisor)              │
│ (SCR_EL3.IRQ = 0, SCR_EL3.NS = 1)      │
│ (HCR_EL2.IMO = 1, HCR_EL2.FMO = 1)     │
└─────────────────────────────────────────┘
              │
              │ Group 1NS 中断发生
              ▼
┌─────────────────────────────────────────┐
│ 硬件检查:                                │
│ - 中断是 Group 1NS (非安全中断)         │
│ - 当前在 NS-EL2                          │
│ - SCR_EL3.IRQ = 0 (不路由到 EL3)        │
│ - HCR_EL2.IMO = 1 (允许路由到 EL2)      │
│ → 直接路由到 NS-EL2                      │
└─────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────┐
│ NS-EL2 中断处理程序执行                   │
│ (Hypervisor 中断处理，例如虚拟机中断)     │
│ 无需上下文切换                            │
└─────────────────────────────────────────┘
```

**代码路径**:
- 中断直接由 NS-EL2 的异常向量处理
- Hypervisor 可以：
  - 直接处理中断
  - 注入到虚拟机（Guest VM）
  - 根据虚拟化配置决定处理方式

**关键点**:
- EL2 是 Hypervisor 级别，用于虚拟化
- Group 1NS 中断在 NS-EL2 可以直接处理
- Group 0 和 Group 1S 中断仍然路由到 EL3（安全属性优先）

---

### 场景 7: 在 S-EL1 执行时发生 Group 0 中断

```
┌─────────────────────────────────────────┐
│ S-EL1 执行中 (TSP)                      │
│ (SCR_EL3.IRQ = 0, SCR_EL3.NS = 0)      │
└─────────────────────────────────────────┘
              │
              │ Group 0 中断发生
              ▼
┌─────────────────────────────────────────┐
│ 硬件检查:                                │
│ - 中断是 Group 0 (总是路由到 EL3)       │
│ → 强制路由到 EL3                         │
└─────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────┐
│ EL3 异常向量                             │
│ → EL3 中断处理程序                        │
└─────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────┐
│ EL3 中断处理                             │
│ 处理完成后返回 S-EL1                     │
└─────────────────────────────────────────┘
```

---

## 代码实现分析

### 1. 中断路由模型设置

```c
// 从 interrupt_mgmt.c
int32_t set_routing_model(uint32_t type, uint32_t flags)
{
    // 验证路由模型
    switch (type) {
    case INTR_TYPE_S_EL1:
        if (validate_sel1_interrupt_rm(flags) != 0)
            return -EINVAL;
        break;
    case INTR_TYPE_NS:
        if (validate_ns_interrupt_rm(flags) != 0)
            return -EINVAL;
        break;
    case INTR_TYPE_EL3:
        if (validate_el3_interrupt_rm(flags) != 0)
            return -EINVAL;
        break;
    }
    
    // 设置路由模型
    interrupt_rm_flags[type] = flags;
    
    return 0;
}
```

### 2. SCR_EL3 配置

```c
// 从 context_mgmt.c
u_register_t get_scr_el3_from_routing_model(size_t security_state)
{
    u_register_t scr_el3 = 0;
    uint32_t rm_flags;
    
    // 获取 S-EL1 中断路由模型
    rm_flags = interrupt_rm_flags[INTR_TYPE_S_EL1];
    if (get_interrupt_rm_flag(rm_flags, security_state)) {
        scr_el3 |= SCR_IRQ_BIT | SCR_FIQ_BIT;
    }
    
    // 获取 NS 中断路由模型
    rm_flags = interrupt_rm_flags[INTR_TYPE_NS];
    if (get_interrupt_rm_flag(rm_flags, security_state)) {
        scr_el3 |= SCR_IRQ_BIT | SCR_FIQ_BIT;
    }
    
    // EL3 中断总是路由到 EL3
    rm_flags = interrupt_rm_flags[INTR_TYPE_EL3];
    if (get_interrupt_rm_flag(rm_flags, security_state)) {
        scr_el3 |= SCR_IRQ_BIT | SCR_FIQ_BIT;
    }
    
    return scr_el3;
}
```

### 3. 中断处理流程

```c
// EL3 异常向量中的中断处理
void el3_interrupt_handler(void)
{
    uint32_t id;
    uint32_t flags = 0;
    void *handle;
    interrupt_type_handler_t handler;
    
    // 1. 获取中断 ID
    id = plat_ic_get_pending_interrupt_id();
    
    // 2. 确定中断类型
    uint32_t type = get_interrupt_type(id);
    
    // 3. 获取处理程序
    handler = get_interrupt_type_handler(type);
    
    // 4. 确定当前上下文
    if (read_scr_el3() & SCR_NS_BIT) {
        handle = cm_get_context(NON_SECURE);
        set_interrupt_src_ss(flags, NON_SECURE);
    } else {
        handle = cm_get_context(SECURE);
        set_interrupt_src_ss(flags, SECURE);
    }
    
    // 5. 调用处理程序
    handler(id, flags, handle, NULL);
}
```

---

## 总结

### 关键要点

1. **中断分组**:
   - Group 0: 总是路由到 EL3
   - Group 1S: 安全中断，根据路由模型决定
   - Group 1NS: 非安全中断，根据路由模型决定

2. **路由规则**:
   - 安全中断在非安全世界执行时，必须路由到 EL3
   - 非安全中断在安全世界执行时，必须路由到 EL3
   - 同安全状态的中断可以直接在当前 EL 处理
   - **Group 1S 中断不是必须路由到 EL1**：
     - 可以选择路由模型 RM0（委托给 S-EL1）或 RM1（EL3 直接处理）
     - 这取决于系统配置和是否有 Secure Payload

3. **EL2 中断路由**:
   - **什么时候路由到 EL2**:
     - 系统运行在 **NS-EL2** (Hypervisor 模式)
     - 中断类型是 **Group 1NS** (非安全中断)
     - **SCR_EL3.IRQ/FIQ = 0** (不路由到 EL3)
     - **HCR_EL2.IMO/FMO = 1** (允许路由到 EL2)
   - **不会路由到 EL2 的情况**:
     - Group 0 中断（总是路由到 EL3）
     - Group 1S 中断（安全中断，路由到 EL3 或 S-EL1）
     - 系统运行在 NS-EL1 或更低级别

4. **上下文切换**:
   - 跨安全状态的中断需要保存/恢复上下文
   - EL3 负责协调上下文切换

5. **SCR_EL3 和 HCR_EL2 控制**:
   - `SCR_EL3.IRQ/FIQ` 控制中断是否路由到 EL3
   - `HCR_EL2.IMO/FMO` 控制中断是否路由到 EL2
   - 安全属性优先级最高，会覆盖寄存器设置

### 最佳实践

1. **配置中断分组**: 在 GIC 初始化时正确配置中断分组
2. **设置路由模型**: 根据安全需求配置合适的路由模型
3. **处理抢占**: 正确处理被抢占的 SMC 调用
4. **上下文管理**: 确保上下文切换时正确保存/恢复状态

---

## 参考资料

- ARM Trusted Firmware-A Documentation
- ARM Architecture Reference Manual
- GIC Architecture Specification
- `arm-trusted-firmware/include/bl31/interrupt_mgmt.h`
- `arm-trusted-firmware/services/spd/tspd/tspd_main.c`
- `arm-trusted-firmware/lib/el3_runtime/aarch64/context_mgmt.c`

