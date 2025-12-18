# TSP_STD_FID 执行流程分析

## 问题

`TSP_STD_FID(TSP_ADD)` 这个SMC调用会到TSP吗？还是只会到TSPD？

## 答案

**会到TSP！** 流程是：TSPD → TSP → TSPD → Non-Secure

## 定义分析

### 1. TSP_STD_FID 的定义

```c
// tf-a-tests/include/runtime_services/secure_el1_payloads/tsp.h:47
#define TSP_STD_FID(fid)	((TSP_BARE_FID(fid) | 0x72000000))
```

### 2. TSP_YIELD_FID 的定义（在ATF中）

```c
// arm-trusted-firmware/include/bl32/tsp/tsp.h:57
#define TSP_YIELD_FID(fid)	((TSP_BARE_FID(fid) | 0x72000000))
```

### 3. 对比

| 宏 | 值 | Bit 31 | 类型 |
|---|----|--------|------|
| `TSP_STD_FID(fid)` | `0x72000000 | fid` | 0 | **Yielding SMC** |
| `TSP_YIELD_FID(fid)` | `0x72000000 | fid` | 0 | **Yielding SMC** |
| `TSP_FAST_FID(fid)` | `0x72000000 | fid | (1<<31)` | 1 | Fast SMC |

**结论**：`TSP_STD_FID = TSP_YIELD_FID`（都是yielding SMC）

## 执行流程

### 完整流程

```
1. Non-Secure调用: TSP_STD_FID(TSP_ADD)
   ↓
2. CPU自动切换到EL3（硬件）
   ↓
3. BL31路由SMC到TSPD
   ↓
4. TSPD的tspd_smc_handler在EL3执行  ← 先到TSPD
   ↓
5. TSPD识别这是TSP_YIELD_FID(TSP_ADD)（569行）
   ↓
6. TSPD检查：yield_smc_active_flag未设置（593行）
   ↓
7. TSPD保存Non-Secure上下文（596行）
   ↓
8. TSPD保存参数（599行）
   ↓
9. TSPD设置TSP的入口点（624行）
   cm_set_elr_el3(SECURE, &tsp_vectors->yield_smc_entry);
   ↓
10. TSPD设置yield_smc_active_flag（623行）
   ↓
11. TSPD恢复TSP上下文（649行）
   ↓
12. TSPD设置ERET返回到SECURE（650行）
   cm_set_next_eret_context(SECURE);
   ↓
13. TSPD执行ERET到TSP（651行）
   SMC_RET3(&tsp_ctx->cpu_ctx, smc_fid, x1, x2);
   ↓
14. CPU切换到S-EL1，TSP开始执行  ← 到TSP了！
   ↓
15. TSP执行tsp_yield_smc_entry（477行）
   ↓
16. TSP调用tsp_smc_handler执行ADD计算（479行）
   ↓
17. TSP完成计算，返回结果（297-300行）
   ↓
18. TSP执行SMC返回到EL3
   ↓
19. CPU自动切换到EL3（硬件）
   ↓
20. BL31路由SMC到TSPD
   ↓
21. TSPD的tspd_smc_handler在EL3执行  ← 回到TSPD
   ↓
22. TSPD识别这是TSP的返回值（652行else分支）
   ↓
23. TSPD保存SECURE上下文（660行）
   ↓
24. TSPD恢复Non-Secure上下文（667行）
   ↓
25. TSPD清除yield_smc_active_flag（670行）
   ↓
26. TSPD设置ERET返回到NON_SECURE（668行）
   ↓
27. TSPD返回结果到Non-Secure（681行）
   SMC_RET3(ns_cpu_context, x1, x2, x3);
   ↓
28. CPU切换到Non-Secure，调用者收到结果
```

## 关键代码位置

### TSPD处理yielding SMC（569-651行）

```c
case TSP_YIELD_FID(TSP_ADD):  // TSP_STD_FID = TSP_YIELD_FID
    if (ns) {
        // 检查是否已经被抢占
        if (get_yield_smc_active_flag(tsp_ctx->state))
            SMC_RET1(handle, SMC_UNK);
        
        // 保存Non-Secure上下文
        cm_el1_sysregs_context_save(NON_SECURE);
        
        // 保存参数
        store_tsp_args(tsp_ctx, x1, x2);
        
        // 设置TSP入口点
        if (GET_SMC_TYPE(smc_fid) == SMC_TYPE_FAST) {
            cm_set_elr_el3(SECURE, &tsp_vectors->fast_smc_entry);
        } else {
            // TSP_STD_FID是yielding，走这个分支
            set_yield_smc_active_flag(tsp_ctx->state);
            cm_set_elr_el3(SECURE, &tsp_vectors->yield_smc_entry);  // 设置TSP入口
        }
        
        // 恢复TSP上下文
        cm_el1_sysregs_context_restore(SECURE);
        cm_set_next_eret_context(SECURE);  // 返回到SECURE
        
        // ERET到TSP
        SMC_RET3(&tsp_ctx->cpu_ctx, smc_fid, x1, x2);  // 切换到TSP！
    }
```

### TSPD处理TSP返回值（652-681行）

```c
} else {
    // 这是TSP返回的结果
    assert(handle == cm_get_context(SECURE));
    cm_el1_sysregs_context_save(SECURE);  // 保存TSP上下文
    
    // 恢复Non-Secure上下文
    ns_cpu_context = cm_get_context(NON_SECURE);
    cm_el1_sysregs_context_restore(NON_SECURE);
    cm_set_next_eret_context(NON_SECURE);  // 返回到Non-Secure
    
    // 清除flag
    if (GET_SMC_TYPE(smc_fid) == SMC_TYPE_YIELD) {
        clr_yield_smc_active_flag(tsp_ctx->state);
    }
    
    // 返回结果到Non-Secure
    SMC_RET3(ns_cpu_context, x1, x2, x3);
}
```

## 总结

### 执行路径

```
Non-Secure
    ↓ SMC调用
TSPD (EL3)  ← 先到这里
    ↓ 设置上下文，ERET
TSP (S-EL1)  ← 然后到这里执行计算
    ↓ SMC返回
TSPD (EL3)  ← 再回到这里
    ↓ 返回结果，ERET
Non-Secure  ← 最后回到这里
```

### 关键点

1. **TSP_STD_FID = TSP_YIELD_FID**（都是yielding SMC）
2. **会到TSP**：TSPD设置上下文后，通过ERET切换到TSP执行
3. **TSPD的作用**：
   - 接收SMC调用
   - 设置上下文
   - 切换到TSP
   - 接收TSP结果
   - 返回结果到Non-Secure

### 在测试代码中的情况（311-316行）

```c
tsp_svc_params.fid = TSP_STD_FID(TSP_ADD);  // Yielding SMC
tsp_result = tftf_smc(&tsp_svc_params);
```

这个调用会：
1. 先到TSPD（EL3）
2. TSPD切换到TSP（S-EL1）
3. TSP执行ADD计算
4. TSP返回结果到TSPD（EL3）
5. TSPD返回结果到Non-Secure

**所以答案是：会到TSP！**

