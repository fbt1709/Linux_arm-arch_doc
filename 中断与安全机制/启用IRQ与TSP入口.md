# enable_irq() 与进入TSP的关系

## 问题

是不是只有`enable_irq()`才能够进入TSP？

## 答案

**不是！** 进入TSP是通过**SMC调用**，不是通过`enable_irq()`。

`enable_irq()`只是让**Non-Secure世界**能够处理pending的SGI中断。

## 执行流程分析

### 完整时序

```
1. disable_irq() (78行)
   PSTATE.I = 1 (禁用IRQ)
   ↓
2. 发送SGI (84行)
   SGI进入pending状态（因为中断被禁用）
   ↓
3. 调用STD SMC (90行)  ← 这里进入TSP！
   tftf_smc(tsp_svc_params)
   ↓
4. CPU切换到EL3，TSPD接收SMC
   ↓
5. TSPD切换到TSP (S-EL1)
   ↓
6. TSP执行tsp_yield_smc_entry (477行)
   msr daifclr, #DAIF_FIQ_BIT | DAIF_IRQ_BIT  ← TSP自己使能中断！
   ↓
7. TSP执行tsp_smc_handler
   ↓
8. TSP检测到pending的NS中断
   ↓
9. TSP被抢占，返回TSP_SMC_PREEMPTED
   ↓
10. 返回到Non-Secure世界 (90行返回)
    shared_data.tsp_result.ret0 = TSP_SMC_PREEMPTED
    ↓
11. enable_irq() (99行)  ← 这里只是让Non-Secure处理SGI
    PSTATE.I = 0 (使能IRQ)
    ↓
12. pending的SGI被处理
    sgi_handler()被调用
```

## 关键点

### 1. 进入TSP的时机（90行）

```c
shared_data.tsp_result = tftf_smc(tsp_svc_params);
```

**这里就进入TSP了！** 不需要等待`enable_irq()`。

### 2. TSP自己使能中断（478行）

```assembly
func tsp_yield_smc_entry
    msr daifclr, #DAIF_FIQ_BIT | DAIF_IRQ_BIT  // TSP使能中断
    bl  tsp_smc_handler
    msr daifset, #DAIF_FIQ_BIT | DAIF_IRQ_BIT  // TSP禁用中断
    restore_args_call_smc
```

**TSP在入口点自己使能中断**，不依赖Non-Secure世界的`enable_irq()`。

### 3. enable_irq()的作用（99行）

```c
/* Set PSTATE.I to 1. The SGI will be handled after this. */
enable_irq();
```

这个`enable_irq()`是**在Non-Secure世界执行的**，作用是：
- 让Non-Secure世界能够处理pending的SGI
- 使`sgi_handler()`能够被调用
- **不影响TSP的进入**

## 两个不同的中断使能

### 1. Non-Secure世界的enable_irq()（99行）

```c
// 在Non-Secure世界执行
enable_irq();  // PSTATE.I = 0 (Non-Secure的PSTATE)
```

**作用**：让Non-Secure世界能够处理中断

### 2. TSP的daifclr（478行）

```assembly
// 在TSP (S-EL1)执行
msr daifclr, #DAIF_FIQ_BIT | DAIF_IRQ_BIT  // PSTATE.I = 0 (Secure的PSTATE)
```

**作用**：让TSP能够处理中断（包括被NS中断抢占）

## 为什么需要disable_irq()？

### 目的：让SGI进入pending状态

```c
disable_irq();        // 78行 - 禁用中断
tftf_send_sgi(...);   // 84行 - 发送SGI，进入pending
tftf_smc(...);        // 90行 - 调用SMC，进入TSP
```

**流程**：
1. 禁用中断 → SGI无法立即处理
2. 发送SGI → SGI进入pending状态
3. 调用SMC → 进入TSP
4. TSP使能中断 → 检测到pending的NS中断
5. TSP被抢占 → 返回TSP_SMC_PREEMPTED

如果不禁用中断，SGI会立即被处理，无法测试抢占机制。

## 总结

| 操作 | 位置 | 作用 | 是否影响进入TSP |
|------|------|------|----------------|
| `disable_irq()` | Non-Secure (78行) | 让SGI进入pending | ❌ 不影响 |
| `tftf_smc()` | Non-Secure (90行) | **进入TSP** | ✅ **这里进入TSP** |
| `msr daifclr` | TSP (478行) | TSP使能中断 | ❌ 不影响进入 |
| `enable_irq()` | Non-Secure (99行) | 处理pending的SGI | ❌ 不影响进入TSP |

### 关键结论

1. **进入TSP是通过SMC调用（90行）**，不是通过`enable_irq()`
2. **TSP自己使能中断（478行）**，不依赖Non-Secure的`enable_irq()`
3. **`enable_irq()`（99行）只是让Non-Secure世界处理SGI**，不影响TSP的进入
4. **`disable_irq()`（78行）是为了让SGI进入pending状态**，测试抢占机制

**所以：不是只有enable_irq才能进入TSP，进入TSP是通过SMC调用！**

