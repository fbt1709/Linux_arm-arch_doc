# SMC_RET3 函数分析

## 问题

**`SMC_RET3(&tsp_ctx->cpu_ctx, smc_fid, x1, x2);` 这个指令最后会干什么呢？**

## 答案

**`SMC_RET3` 宏会：**
1. **设置返回值**：将 `smc_fid`, `x1`, `x2` 写入到 `tsp_ctx->cpu_ctx` 的寄存器上下文中
2. **返回handle**：返回 `tsp_ctx->cpu_ctx` 指针
3. **触发ERET**：最终通过 `el3_exit` 函数执行 `ERET` 指令，返回到TSP（S-EL1）

## 宏定义展开

### SMC_RET3 宏定义

```c
// include/arch/aarch64/smccc_helpers.h:44-47
#define SMC_RET3(_h, _x0, _x1, _x2)	{			\
	write_ctx_reg((get_gpregs_ctx(_h)), (CTX_GPREG_X2), (_x2));	\
	SMC_RET2(_h, (_x0), (_x1));				\
}
```

### 逐层展开

```c
// SMC_RET3(&tsp_ctx->cpu_ctx, smc_fid, x1, x2)
// 展开为：
{
    write_ctx_reg(get_gpregs_ctx(&tsp_ctx->cpu_ctx), CTX_GPREG_X2, x2);
    SMC_RET2(&tsp_ctx->cpu_ctx, smc_fid, x1);
}

// SMC_RET2 展开为：
{
    write_ctx_reg(get_gpregs_ctx(&tsp_ctx->cpu_ctx), CTX_GPREG_X1, x1);
    SMC_RET1(&tsp_ctx->cpu_ctx, smc_fid);
}

// SMC_RET1 展开为：
{
    write_ctx_reg(get_gpregs_ctx(&tsp_ctx->cpu_ctx), CTX_GPREG_X0, smc_fid);
    SMC_RET0(&tsp_ctx->cpu_ctx);
}

// SMC_RET0 展开为：
{
    return (uint64_t) (&tsp_ctx->cpu_ctx);
}
```

## 完整执行流程

### 1. SMC_RET3 设置返回值

```c
// services/spd/tspd/tspd_main.c:651
SMC_RET3(&tsp_ctx->cpu_ctx, smc_fid, x1, x2);
```

**执行的操作**：
- ✅ 将 `x2` 写入到 `tsp_ctx->cpu_ctx` 的 `CTX_GPREG_X2`
- ✅ 将 `x1` 写入到 `tsp_ctx->cpu_ctx` 的 `CTX_GPREG_X1`
- ✅ 将 `smc_fid` 写入到 `tsp_ctx->cpu_ctx` 的 `CTX_GPREG_X0`
- ✅ 返回 `&tsp_ctx->cpu_ctx` 指针

### 2. 返回到 SMC Handler

```c
// common/runtime_svc.c:54-55
return rt_svc_descs[index].handle(smc_fid, x1, x2, x3, x4, cookie,
                handle, flags);
```

**关键**：
- SMC handler（`tspd_smc_handler`）返回 `&tsp_ctx->cpu_ctx` 指针
- 这个返回值会被传递给 `el3_exit` 函数

### 3. 调用 el3_exit

```assembly
// bl31/aarch64/runtime_exceptions.S:380-390
/* ----------------------------------------------------------
 * Call the Runtime Service handler. Then it returns the
 * return value in x0
 * ----------------------------------------------------------
 */
bl	handle_runtime_svc

/* ----------------------------------------------------------
 * "handle_runtime_svc" returns the pointer to the cpu_context
 * structure of the target security state in x0. Prepare the
 * arguments for el3_exit function that will restore the
 * context pointed by x0.
 * ----------------------------------------------------------
 */
mov	x20, x0  // x0 = &tsp_ctx->cpu_ctx

// ... 准备参数 ...

b	el3_exit
```

**关键**：
- `handle_runtime_svc` 返回 `&tsp_ctx->cpu_ctx`（即 `SMC_RET3` 的返回值）
- 这个指针被保存到 `x20`，然后传递给 `el3_exit`

### 4. el3_exit 恢复上下文并ERET

```assembly
// lib/el3_runtime/aarch64/context.S:589-667
func el3_exit
    // ... 保存当前SP_EL0 ...

    // ... 恢复CPTR_EL3 ...

    /* --------------------------------------------------------------
     * Restore MDCR_EL3, SPSR_EL3, ELR_EL3 and SCR_EL3 prior to ERET
     * --------------------------------------------------------------
     */
    ldp	x16, x17, [sp, #CTX_EL3STATE_OFFSET + CTX_SPSR_EL3]
    ldr	x18, [sp, #CTX_EL3STATE_OFFSET + CTX_SCR_EL3]
    ldr	x19, [sp, #CTX_EL3STATE_OFFSET + CTX_MDCR_EL3]
    msr	spsr_el3, x16  // ← 恢复SPSR_EL3（包含PSTATE）
    msr	elr_el3, x17     // ← 恢复ELR_EL3（返回地址，即tsp_yield_smc_entry）
    msr	scr_el3, x18     // ← 恢复SCR_EL3（安全状态配置）
    msr	mdcr_el3, x19

    // ... 恢复其他寄存器 ...

    /* ----------------------------------------------------------
     * Restore general purpose (including x30), PMCR_EL0 and
     * ARMv8.3-PAuth registers.
     * Exit EL3 via ERET to a lower exception level.
     * ----------------------------------------------------------
     */
    bl	restore_gp_pmcr_pauth_regs  // ← 恢复通用寄存器（包括x0-x30）
    ldr	x30, [sp, #CTX_GPREGS_OFFSET + CTX_GPREG_LR]

    exception_return  // ← 执行 ERET 指令
endfunc el3_exit
```

**关键**：
- ✅ **恢复SPSR_EL3**：恢复PSTATE（包括DAIF位、EL等）
- ✅ **恢复ELR_EL3**：恢复返回地址（`tsp_yield_smc_entry`）
- ✅ **恢复SCR_EL3**：恢复安全状态配置
- ✅ **恢复通用寄存器**：恢复x0-x30（包括SMC_RET3设置的x0, x1, x2）
- ✅ **执行ERET**：通过 `exception_return` 宏执行 `ERET` 指令

### 5. ERET 返回到TSP

```
ERET 指令执行：
  ↓
从 ELR_EL3 恢复 PC → tsp_yield_smc_entry
  ↓
从 SPSR_EL3 恢复 PSTATE → S-EL1, DAIF等
  ↓
从 SCR_EL3 恢复安全状态 → Secure
  ↓
返回到 TSP (S-EL1)
```

**关键**：
- ✅ **PC恢复**：`ELR_EL3` 指向 `tsp_yield_smc_entry`，所以返回到TSP
- ✅ **PSTATE恢复**：`SPSR_EL3` 包含S-EL1的PSTATE
- ✅ **安全状态恢复**：`SCR_EL3` 配置为Secure状态

## 关键理解

### 1. SMC_RET3 的作用

**设置返回值到上下文**：
- 将 `smc_fid`, `x1`, `x2` 写入到 `tsp_ctx->cpu_ctx` 的寄存器上下文中
- 这些值会在 `el3_exit` 恢复上下文时，恢复到实际的寄存器中

### 2. 返回值的作用

**返回handle指针**：
- `SMC_RET3` 返回 `&tsp_ctx->cpu_ctx` 指针
- 这个指针告诉 `el3_exit` 要恢复哪个上下文
- `el3_exit` 使用这个指针来恢复TSP的上下文

### 3. ERET 的作用

**返回到TSP**：
- `ERET` 指令从 `ELR_EL3` 恢复PC，从 `SPSR_EL3` 恢复PSTATE
- 由于 `ELR_EL3` 被设置为 `tsp_yield_smc_entry`，所以返回到TSP
- 由于 `SPSR_EL3` 被设置为S-EL1的PSTATE，所以返回到S-EL1

## 代码位置总结

### 1. SMC_RET3 宏定义

**位置**：`include/arch/aarch64/smccc_helpers.h:44-47`

### 2. TSPD调用SMC_RET3

**位置**：`services/spd/tspd/tspd_main.c:651`
```c
SMC_RET3(&tsp_ctx->cpu_ctx, smc_fid, x1, x2);
```

### 3. el3_exit 函数

**位置**：`lib/el3_runtime/aarch64/context.S:589-667`

### 4. exception_return 宏

**位置**：`include/arch/aarch64/asm_macros.S:287-295`
```assembly
.macro exception_return
    eret
    // ... 其他指令 ...
.endm
```

## 总结

### SMC_RET3 的完整流程

```
1. SMC_RET3(&tsp_ctx->cpu_ctx, smc_fid, x1, x2)
   ↓
2. 将 smc_fid, x1, x2 写入到 tsp_ctx->cpu_ctx 的寄存器上下文中
   ↓
3. 返回 &tsp_ctx->cpu_ctx 指针
   ↓
4. SMC handler 返回这个指针
   ↓
5. runtime_exceptions.S 调用 el3_exit
   ↓
6. el3_exit 恢复 tsp_ctx->cpu_ctx 中的上下文
   ↓
7. 恢复 ELR_EL3（指向 tsp_yield_smc_entry）
   ↓
8. 恢复 SPSR_EL3（S-EL1的PSTATE）
   ↓
9. 恢复通用寄存器（包括x0=smc_fid, x1=x1, x2=x2）
   ↓
10. 执行 ERET 指令
    ↓
11. 返回到 TSP (S-EL1) 的 tsp_yield_smc_entry
```

### 关键点

1. **SMC_RET3 设置返回值**：将返回值写入到上下文结构中
2. **返回handle指针**：告诉 `el3_exit` 要恢复哪个上下文
3. **el3_exit 恢复上下文**：恢复所有寄存器，包括返回值
4. **ERET 返回到TSP**：从EL3返回到S-EL1，继续执行TSP代码

**所以，`SMC_RET3(&tsp_ctx->cpu_ctx, smc_fid, x1, x2);` 最终会通过 `el3_exit` 和 `ERET` 指令，返回到TSP（S-EL1）的 `tsp_yield_smc_entry`，并且 `x0=smc_fid`, `x1=x1`, `x2=x2`。**

