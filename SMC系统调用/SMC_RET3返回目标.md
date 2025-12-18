# SMC_RET3 返回到哪个世界？

## 关键理解

**`SMC_RET3(ns_cpu_context, x1, x2, x3)` 返回到 NON-SECURE 世界，不是SPD世界！**

## 代码分析

### 1. SMC_RET3 宏定义

```c
// include/arch/aarch64/smccc_helpers.h:44-47
#define SMC_RET3(_h, _x0, _x1, _x2) {			\
	write_ctx_reg((get_gpregs_ctx(_h)), (CTX_GPREG_X2), (_x2));	\
	SMC_RET2(_h, (_x0), (_x1));				\
}
```

**作用**：
- 将返回值写入context的寄存器（x0, x1, x2）
- 返回context指针

### 2. 在TSPD中的使用（681行）

```c
// services/spd/tspd/tspd_main.c:652-681
} else {
    /*
     * This is the result from the secure client of an
     * earlier request. The results are in x1-x3. Copy it
     * into the non-secure context, save the secure state
     * and return to the non-secure state.
     */
    assert(handle == cm_get_context(SECURE));  // 当前handle是SECURE
    
    // 1. 保存secure上下文（TSP的上下文）
    cm_el1_sysregs_context_save(SECURE);
    
    // 2. 获取non-secure上下文
    ns_cpu_context = cm_get_context(NON_SECURE);
    
    // 3. 恢复non-secure上下文
    cm_el1_sysregs_context_restore(NON_SECURE);
    
    // 4. 设置下一个ERET返回到NON_SECURE世界
    cm_set_next_eret_context(NON_SECURE);  // 关键！
    
    // 5. 清理yield_smc_active_flag
    if (GET_SMC_TYPE(smc_fid) == SMC_TYPE_YIELD) {
        clr_yield_smc_active_flag(tsp_ctx->state);
    }
    
    // 6. 设置返回值并返回到non-secure世界
    SMC_RET3(ns_cpu_context, x1, x2, x3);  // 返回到NON_SECURE！
}
```

## 执行流程

### 完整流程

```
1. Non-Secure世界调用SMC (TSP_ADD)
   ↓
2. TSPD在EL3接收SMC
   ↓
3. TSPD切换到TSP (SECURE世界，S-EL1)
   ↓
4. TSP执行计算，返回结果
   ↓
5. TSPD在EL3接收TSP的返回值
   ↓
6. TSPD保存SECURE上下文
   ↓
7. TSPD恢复NON_SECURE上下文
   ↓
8. TSPD设置 cm_set_next_eret_context(NON_SECURE)  ← 关键！
   ↓
9. TSPD调用 SMC_RET3(ns_cpu_context, x1, x2, x3)
   ↓
10. ERET返回到NON_SECURE世界（调用者）
```

## 为什么返回到NON-SECURE？

### 1. TSPD在EL3，不是"世界"

- **TSPD运行在EL3**（Secure Monitor）
- EL3不是"世界"，而是**特权层**
- TSPD的作用是**协调**SECURE和NON-SECURE世界之间的通信

### 2. 调用链

```
Non-Secure世界 (EL1/EL2)
    ↓ SMC调用
TSPD (EL3) ← 处理SMC，协调两个世界
    ↓ 切换到
TSP (S-EL1, SECURE世界)
    ↓ 完成计算，返回
TSPD (EL3) ← 接收结果
    ↓ 返回到
Non-Secure世界 (EL1/EL2) ← 调用者收到结果
```

### 3. cm_set_next_eret_context 的作用

```c
cm_set_next_eret_context(NON_SECURE);  // 668行
```

这个函数设置**下一个ERET返回到哪个世界**。在这里设置为`NON_SECURE`，意味着：
- 下一个ERET会返回到non-secure世界
- `SMC_RET3`返回的context指针会被用于ERET
- 因此最终返回到non-secure世界的调用者

## 对比：返回到SECURE的情况

### 返回到TSP（651行）

```c
// 590-651行：新的SMC请求，切换到TSP
cm_el1_sysregs_context_restore(SECURE);
cm_set_next_eret_context(SECURE);  // 返回到SECURE！
SMC_RET3(&tsp_ctx->cpu_ctx, smc_fid, x1, x2);  // 返回到TSP
```

这里：
- `cm_set_next_eret_context(SECURE)` → 返回到SECURE世界
- `SMC_RET3(&tsp_ctx->cpu_ctx, ...)` → 使用TSP的context
- 最终ERET返回到TSP（SECURE世界）

## 总结

| 位置 | cm_set_next_eret_context | SMC_RET3的context | 返回到 |
|------|-------------------------|-------------------|--------|
| 651行 | `SECURE` | `&tsp_ctx->cpu_ctx` (SECURE) | **TSP (SECURE世界)** |
| 681行 | `NON_SECURE` | `ns_cpu_context` (NON_SECURE) | **Non-Secure世界** |

**关键点**：
- `SMC_RET3`本身只是设置返回值
- **`cm_set_next_eret_context`决定返回到哪个世界**
- 在681行，已经设置为`NON_SECURE`，所以返回到non-secure世界
- TSPD在EL3，它**协调**两个世界，但不属于任何一个"世界"

