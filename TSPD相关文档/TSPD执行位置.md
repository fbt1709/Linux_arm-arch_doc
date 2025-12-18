# TSPD 执行位置说明

## 关键概念

**TSPD一直在EL3执行，不需要"切换到SPD世界"！**

## 架构层次

### ARM TrustZone 架构

```
┌─────────────────────────────────────┐
│         EL3 (Secure Monitor)        │  ← TSPD在这里执行！
│  ┌───────────────────────────────┐  │
│  │  TSPD (Secure Payload         │  │
│  │   Dispatcher)                 │  │
│  └───────────────────────────────┘  │
└─────────────────────────────────────┘
           ↕ SMC调用
┌─────────────────────────────────────┐
│      S-EL1 (Secure World)           │
│  ┌───────────────────────────────┐  │
│  │  TSP (Secure Payload)          │  │
│  └───────────────────────────────┘  │
└─────────────────────────────────────┘
           ↕ SMC调用
┌─────────────────────────────────────┐
│   EL1/EL2 (Non-Secure World)        │
│  ┌───────────────────────────────┐  │
│  │  Normal World (Linux/OS)       │  │
│  └───────────────────────────────┘  │
└─────────────────────────────────────┘
```

## TSPD的执行位置

### 1. TSPD一直在EL3执行

```c
// services/spd/tspd/tspd_main.c:341
static uintptr_t tspd_smc_handler(uint32_t smc_fid, ...)
{
    // 这个函数在EL3执行！
    // TSPD作为runtime service注册在EL3
    // 当SMC被调用时，CPU已经在EL3
    // TSPD的代码直接在这里执行，不需要切换
}
```

### 2. TSPD的注册

```c
// TSPD作为runtime service注册在EL3
// 在BL31初始化时注册
runtime_svc_init();
// TSPD的handler被注册为处理Trusted OS范围的SMC
```

### 3. 执行流程

#### 场景1：Non-Secure调用SMC

```
1. Non-Secure世界执行SMC指令
   ↓
2. CPU自动切换到EL3（硬件行为）
   ↓
3. EL3异常向量表 → BL31异常处理
   ↓
4. BL31路由SMC到TSPD
   ↓
5. TSPD的tspd_smc_handler在EL3执行  ← TSPD在这里执行！
   ↓
6. TSPD决定：切换到TSP或本地处理
   ↓
7a. 如果切换到TSP：
    - TSPD设置TSP的上下文
    - TSPD执行ERET到S-EL1
    - TSP在S-EL1执行
   ↓
7b. 如果本地处理：
    - TSPD在EL3直接处理
    - TSPD返回结果
```

#### 场景2：TSP返回结果

```
1. TSP在S-EL1执行，完成计算
   ↓
2. TSP执行SMC指令返回
   ↓
3. CPU自动切换到EL3（硬件行为）
   ↓
4. EL3异常向量表 → BL31异常处理
   ↓
5. BL31路由SMC到TSPD
   ↓
6. TSPD的tspd_smc_handler在EL3执行  ← TSPD在这里执行！
   ↓
7. TSPD接收TSP的返回值
   ↓
8. TSPD保存SECURE上下文
   ↓
9. TSPD恢复NON_SECURE上下文
   ↓
10. TSPD设置返回值并ERET到Non-Secure
```

## 关键点

### 1. EL3不是"世界"

- **EL3是特权级别**，不是"世界"
- **世界**是指：SECURE世界（S-EL1）和NON-SECURE世界（EL1/EL2）
- **TSPD在EL3**，它协调两个世界，但不属于任何一个"世界"

### 2. TSPD不需要"切换"

- TSPD的代码**一直在EL3执行**
- 当SMC被调用时，CPU已经在EL3
- TSPD的handler直接执行，**不需要切换**
- TSPD只是**设置上下文**然后**ERET**到目标世界

### 3. 上下文切换 vs 代码执行

```c
// TSPD在EL3执行这段代码
static uintptr_t tspd_smc_handler(...)
{
    // 代码在EL3执行
    if (需要切换到TSP) {
        // 设置TSP的上下文
        cm_el1_sysregs_context_restore(SECURE);
        cm_set_next_eret_context(SECURE);
        
        // ERET切换到S-EL1（TSP）
        SMC_RET0(&tsp_ctx->cpu_ctx);
        // ↑ 这里ERET后，CPU切换到S-EL1
        //   但TSPD的代码已经执行完了
    }
    
    // 如果不需要切换，TSPD继续在EL3执行
    // 处理完成后返回
}
```

## 对比：三个执行环境

| 执行环境 | 特权级别 | 代码位置 | 说明 |
|---------|---------|---------|------|
| **TSPD** | EL3 | `services/spd/tspd/` | 一直在EL3执行，协调两个世界 |
| **TSP** | S-EL1 | `bl32/tsp/` | SECURE世界，通过ERET切换 |
| **Normal World** | EL1/EL2 | Linux/OS | NON-SECURE世界，通过ERET切换 |

## 实际执行示例

### 处理TSP返回值的代码（681行）

```c
// 这段代码在EL3执行！
} else {
    // TSPD在EL3执行这些代码
    assert(handle == cm_get_context(SECURE));
    cm_el1_sysregs_context_save(SECURE);  // 保存TSP上下文
    
    ns_cpu_context = cm_get_context(NON_SECURE);
    cm_el1_sysregs_context_restore(NON_SECURE);  // 恢复Non-Secure上下文
    cm_set_next_eret_context(NON_SECURE);  // 设置返回目标
    
    SMC_RET3(ns_cpu_context, x1, x2, x3);  // ERET返回到Non-Secure
    // ↑ 这里ERET后，CPU切换到Non-Secure世界
    //   但TSPD的代码已经执行完了
}
```

**关键**：
- 这段代码**在EL3执行**
- TSPD**不需要切换到"SPD世界"**，因为它就在EL3
- ERET只是**切换CPU的执行环境**，不是切换TSPD的执行位置

## 总结

1. **TSPD一直在EL3执行**
   - TSPD的代码作为runtime service注册在EL3
   - 当SMC被调用时，TSPD的handler在EL3直接执行

2. **不需要"切换到SPD世界"**
   - EL3不是"世界"，是特权级别
   - TSPD就在EL3，不需要切换

3. **TSPD的作用**
   - 在EL3协调SECURE和NON-SECURE世界
   - 设置上下文，然后ERET到目标世界
   - 处理完成后，ERET返回到调用者

4. **执行流程**
   - SMC调用 → CPU切换到EL3 → TSPD在EL3执行
   - TSPD设置上下文 → ERET到目标世界
   - 目标世界执行 → SMC返回 → CPU切换到EL3 → TSPD在EL3执行
   - TSPD处理结果 → ERET返回到调用者

**TSPD就是EL3的代码，它一直在那里执行！**

