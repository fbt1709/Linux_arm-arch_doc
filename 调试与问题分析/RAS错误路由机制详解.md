# RAS错误（SError）路由机制详解

## 核心问题

**RAS错误是直接路由到EL3，还是先经过当前运行等级的异常处理再转给EL3？**

## ARM架构中的SError路由机制

### 路由优先级（从高到低）

ARM架构中，SError的路由有明确的优先级顺序：

```
1. HCR_EL2.AMO = 1 → 路由到EL2（最高优先级）
   ↓
2. HCR_EL2.AMO = 0 且 SCR_EL3.EA = 1 → 路由到EL3
   ↓
3. 否则 → 路由到当前EL（FEL - Faulting Exception Level）
```

### 关键点

**SError不会"经过"当前EL再转给EL3**，而是**直接路由到目标EL**：

- 如果配置路由到EL3 → **直接路由到EL3**，不经过当前EL
- 如果配置路由到EL2 → **直接路由到EL2**，不经过当前EL
- 如果配置路由到当前EL → **在当前EL处理**

## 详细路由机制

### 情况1：HCR_EL2.AMO = 1（EL2存在且启用）

```
当前EL（EL1）执行
    ↓
SError发生
    ↓
硬件检查：HCR_EL2.AMO = 1
    ↓
直接路由到EL2（不经过EL1的异常处理）
    ↓
EL2异常处理函数执行
    ↓
EL2可以：
  - 处理异常
  - 转发到EL3（如果配置）
```

**关键**：SError**直接**路由到EL2，不经过EL1。

### 情况2：HCR_EL2.AMO = 0 且 SCR_EL3.EA = 1

```
当前EL（EL1）执行
    ↓
SError发生
    ↓
硬件检查：
  - HCR_EL2.AMO = 0
  - SCR_EL3.EA = 1
    ↓
直接路由到EL3（不经过EL1的异常处理）
    ↓
EL3异常处理函数执行（serror_aarch64）
    ↓
EL3处理错误
```

**关键**：SError**直接**路由到EL3，不经过EL1。

### 情况3：HCR_EL2.AMO = 0 且 SCR_EL3.EA = 0

```
当前EL（EL1）执行
    ↓
SError发生
    ↓
硬件检查：
  - HCR_EL2.AMO = 0
  - SCR_EL3.EA = 0
    ↓
路由到当前EL（EL1）
    ↓
EL1异常处理函数执行
    ↓
EL1处理错误（或panic）
```

**关键**：SError在当前EL处理，不路由到更高EL。

## 为什么SPSR_EL3显示EL2h？

### 路由路径分析

如果SPSR_EL3.M[3:0] = 0x9 (EL2h)，说明：

```
1. SError在EL1发生
   ↓
2. 硬件检查路由配置
   - HCR_EL2.AMO = 1? → 是，路由到EL2
   ↓
3. 硬件直接切换到EL2（不经过EL1异常处理）
   - 保存EL1的状态到SPSR_EL2
   - 切换到EL2，使用SP_EL2
   ↓
4. EL2异常处理函数执行
   ↓
5. EL2可能转发异常到EL3
   ↓
6. 硬件切换到EL3
   - 保存EL2的状态到SPSR_EL3 ← 这就是你看到的EL2h
   - 切换到EL3，使用SP_EL3
   ↓
7. EL3异常处理函数执行
```

**关键点**：
- SError**直接**路由到EL2，不经过EL1
- EL2可能转发到EL3
- SPSR_EL3保存的是EL2的状态（因为异常从EL2转发到EL3）

## 对比：同步异常 vs 异步异常（SError）

### 同步异常（如Data Abort）

```
当前EL执行
    ↓
同步异常发生
    ↓
在当前EL处理（除非配置了陷阱）
    ↓
如果需要，可以转发到更高EL
```

### 异步异常（SError）

```
当前EL执行
    ↓
SError发生
    ↓
根据路由配置，直接路由到目标EL
  - 不经过当前EL的异常处理
  - 直接切换到目标EL
```

## 实际代码流程

### EL3异常处理入口

```assembly
// bl31/aarch64/runtime_exceptions.S

vector_entry serror_aarch64
#if FFH_SUPPORT
    save_x30
    apply_at_speculative_wa
    sync_and_handle_pending_serror  // 同步挂起的SError
    b   handle_lower_el_async_ea    // 处理来自低异常等级的异步EA
#else
    b   report_unhandled_exception
#endif
end_vector_entry serror_aarch64
```

**关键**：`handle_lower_el_async_ea`表示这是**来自低异常等级的异步EA**，说明SError是从低EL路由上来的。

### 路由决策（硬件自动完成）

路由决策是**硬件自动完成**的，在异常发生时就决定了：

```
SError发生
    ↓
硬件检查路由配置（硬件自动，不需要软件干预）
    ↓
根据优先级直接路由到目标EL
    ↓
目标EL的异常处理函数执行
```

## 总结

### RAS错误路由机制

1. **直接路由，不经过当前EL**
   - 如果配置路由到EL3 → 直接到EL3
   - 如果配置路由到EL2 → 直接到EL2
   - 如果配置路由到当前EL → 在当前EL处理

2. **路由优先级**
   - HCR_EL2.AMO = 1 → EL2（最高优先级）
   - HCR_EL2.AMO = 0 且 SCR_EL3.EA = 1 → EL3
   - 否则 → 当前EL

3. **SPSR保存的状态**
   - SPSR_EL3保存的是**异常到达EL3时的来源EL的状态**
   - 如果经过EL2，SPSR_EL3就保存EL2的状态
   - 如果直接从EL1到EL3，SPSR_EL3就保存EL1的状态

### 你的情况

**SPSR_EL3.M[3:0] = 0x9 (EL2h)** 说明：
- SError直接路由到EL2（不经过EL1）
- EL2可能转发到EL3
- SPSR_EL3保存的是EL2的状态

**这是正常的**，只要最终在EL3处理并终止（terminal in EL3）。

