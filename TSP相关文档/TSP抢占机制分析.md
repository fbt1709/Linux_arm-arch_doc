# TSP抢占机制分析：为什么TSP"不能执行"是正常的

## 注释的含义

```c
/*
 * Invoke an STD SMC. Should be pre-empted because of the SGI that is
 * waiting.
 */
shared_data.tsp_result = tftf_smc(tsp_svc_params);
```

**注释说**：这个SMC**应该被pending的SGI抢占**。

## "不能执行"说明什么？

### 关键理解

**TSP"不能执行"（看不到打印）说明抢占机制正常工作！**

这正是测试想要验证的：**SMC应该被pending的SGI抢占**。

## 抢占机制的工作原理

### 1. ehf_allow_ns_preemption的作用（645行）

```c
// services/spd/tspd/tspd_main.c:645
ehf_allow_ns_preemption(TSP_PREEMPTED);
```

**作用**：
1. **设置优先级掩码**（364行）：允许NS中断抢占Secure执行
2. **预设返回值**（362行）：如果TSP被抢占，x0 = TSP_PREEMPTED
3. **允许抢占**：让pending的NS中断能够打断Secure执行

### 2. 抢占时机

```
1. TSPD调用ehf_allow_ns_preemption(TSP_PREEMPTED) (645行)
   ↓
2. 设置优先级掩码，允许NS中断抢占
   ↓
3. TSPD ERET到TSP (651行)
   ↓
4. TSP执行tsp_yield_smc_entry
   ↓
5. TSP使能中断 (478行: daifclr)
   ↓
6. 立即检测到pending的NS中断
   ↓
7. 由于优先级掩码已设置，NS中断可以抢占
   ↓
8. 中断异常触发，TSP被抢占
   ↓
9. 返回到EL3，TSPD处理抢占
   ↓
10. 返回TSP_SMC_PREEMPTED（已预设）
```

### 3. 为什么TSP"不能执行"？

**因为抢占机制设计就是让TSP在使能中断后立即被抢占！**

```c
// bl31/ehf.c:336-369
void ehf_allow_ns_preemption(uint64_t preempt_ret_code)
{
    // ...
    // 预设返回值（356-362行）
    ns_ctx = cm_get_context(NON_SECURE);
    write_ctx_reg(get_gpregs_ctx(ns_ctx), CTX_GPREG_X0, preempt_ret_code);
    
    // 设置优先级掩码，允许NS中断抢占（364行）
    old_pmr = plat_ic_set_priority_mask(pe_data->ns_pri_mask);
    // ...
}
```

**关键**：
- `ehf_allow_ns_preemption`**预设了返回值**为`TSP_PREEMPTED`
- 这意味着**预期TSP会被抢占**
- 如果TSP被立即抢占，说明抢占机制**正常工作**

## 执行流程详解

### 正常流程（TSP被抢占）

```
1. disable_irq() (78行)
   PSTATE.I = 1，中断禁用
   ↓
2. 发送SGI (84行)
   SGI进入pending状态
   ↓
3. 调用STD SMC (90行)
   ↓
4. TSPD接收，调用ehf_allow_ns_preemption (645行)
   - 预设返回值：x0 = TSP_PREEMPTED
   - 设置优先级掩码：允许NS中断抢占
   ↓
5. TSPD ERET到TSP (651行)
   ↓
6. TSP执行tsp_yield_smc_entry
   ↓
7. TSP使能中断 (478行)
   msr daifclr, #DAIF_FIQ_BIT | DAIF_IRQ_BIT
   ↓
8. 立即检测到pending的NS中断
   ↓
9. 由于优先级掩码已设置，NS中断可以抢占
   ↓
10. 中断异常触发，跳转到TSP异常向量表
    ↓
11. 调用tsp_common_int_handler
    ↓
12. 检测到NS中断，返回TSP_PREEMPTED
    ↓
13. 执行smc #0返回到EL3
    ↓
14. TSPD处理抢占，返回SMC_PREEMPTED（已预设）
    ↓
15. 返回到Non-Secure，tftf_smc()返回TSP_SMC_PREEMPTED
```

### 关键点

1. **ehf_allow_ns_preemption预设了返回值**
   - 即使TSP还没开始执行handler，返回值已经预设为`TSP_PREEMPTED`
   - 这是**预期行为**，不是bug

2. **TSP被立即抢占是正常的**
   - 测试的目的就是验证**SMC能被pending的SGI抢占**
   - 如果TSP被立即抢占，说明抢占机制**正常工作**

3. **TSP"不能执行"是设计如此**
   - 抢占机制设计就是让TSP在使能中断后立即被抢占
   - 这是**测试想要验证的行为**

## 如果TSP能执行说明什么？

如果TSP能执行（能看到打印），可能说明：
1. **抢占机制没有工作**
   - `ehf_allow_ns_preemption`没有正确设置
   - 或者优先级掩码设置有问题
   - 或者NS中断没有正确路由到EL3

2. **测试失败**
   - 如果TSP能完整执行，说明SMC**没有被抢占**
   - 这与测试的预期不符

## 总结

### "不能执行"说明什么？

**说明抢占机制正常工作！**

1. ✅ **TSP确实被调用了**（ERET到S-EL1）
2. ✅ **抢占机制正常工作**（TSP在使能中断后立即被抢占）
3. ✅ **返回值正确**（返回TSP_SMC_PREEMPTED）
4. ✅ **测试通过**（验证了SMC能被pending的SGI抢占）

### 这是正常行为

- **注释说**："Should be pre-empted"（应该被抢占）
- **实际行为**：TSP被立即抢占
- **结论**：**这是预期的、正常的行为！**

### 如果TSP能执行

如果TSP能执行（能看到打印），反而说明：
- ❌ 抢占机制没有工作
- ❌ 测试可能失败
- ❌ 需要检查配置

**所以，TSP"不能执行"是正常的，说明抢占机制正常工作！**

