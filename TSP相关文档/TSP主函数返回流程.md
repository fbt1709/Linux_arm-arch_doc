# TSP_MAIN 返回流程详解

## 完整返回流程

```
tsp_main() 返回
    │
    │ 返回值：&tsp_vector_table (在 x0 中)
    │
    ▼
tsp_entrypoint.S:214
    bl tsp_main
    │
    │ 返回后，x0 = &tsp_vector_table
    │
    ▼
tsp_entrypoint.S:220-222
    mov x1, x0        # x1 = 向量表地址
    mov x0, #TSP_ENTRY_DONE
    smc #0            # ← 触发 SMC 异常，返回到 EL3
    │
    ▼
EL3: runtime_exceptions.S:sync_handler64
    │
    │ SMC 异常处理
    │
    ▼
TSPD: tspd_smc_handler() (tspd_main.c:341)
    │
    │ switch (smc_fid) {
    │ case TSP_ENTRY_DONE:  # ← 匹配这里
    │
    ▼
tspd_main.c:438-515
    case TSP_ENTRY_DONE:
        // x1 包含向量表地址
        tsp_vectors = (tsp_vectors_t *) x1;
        
        // 设置 TSP 状态
        set_tsp_pstate(tsp_ctx->state, TSP_PSTATE_ON);
        
        // 注册电源管理和中断处理
        psci_register_spd_pm_hook(&tspd_pm);
        register_interrupt_type_handler(...);
        
        // 关键：返回到调用 tspd_synchronous_sp_entry 的地方
        tspd_synchronous_sp_exit(tsp_ctx, x1);  # ← 这里
        break;
    │
    ▼
tspd_common.c:100
    void tspd_synchronous_sp_exit(tsp_context_t *tsp_ctx, uint64_t ret)
    {
        // 保存 S-EL1 系统寄存器上下文
        cm_el1_sysregs_context_save(SECURE);
        
        // 恢复 EL3 C 运行时上下文
        tspd_exit_sp(tsp_ctx->c_rt_ctx, ret);  # ← 这里
    }
    │
    ▼
tspd_helpers.S:57
    func tspd_exit_sp
        // 恢复栈指针
        mov sp, x0
        
        // 恢复 callee-saved 寄存器 (x19-x30)
        ldp x19, x20, [x0, #(TSPD_C_RT_CTX_X19 - TSPD_C_RT_CTX_SIZE)]
        // ... 恢复其他寄存器 ...
        
        // 设置返回值
        mov x0, x1  # x1 是传入的 ret 参数（向量表地址）
        
        // 关键：返回到调用 tspd_enter_sp 的地方
        ret  # ← 返回到 tspd_synchronous_sp_entry:83 之后
    │
    ▼
tspd_common.c:83-88
    rc = tspd_enter_sp(&tsp_ctx->c_rt_ctx);
    // ↑ 返回到这里，rc = x0 = 向量表地址
    │
    │ return rc;  # ← 返回到这里
    │
    ▼
tspd_main.c:326
    rc = tspd_synchronous_sp_entry(tsp_ctx);
    // ↑ 返回到这里，rc = 向量表地址
    │
    │ assert(rc != 0);  # 检查返回值
    │
    │ return rc;  # ← 返回到这里
    │
    ▼
bl31_main.c:195
    int32_t rc = (*bl32_init)();
    // ↑ 返回到这里，rc = 向量表地址（非零，表示成功）
    │
    │ if (rc == 0) {
    │     WARN("BL31: BL32 initialization failed\n");
    │ }
    │
    ▼
继续执行 bl31_main 的后续代码
```

## 关键返回位置

### 1. 第一次返回：TSP → EL3

**位置**：`bl32/tsp/aarch64/tsp_entrypoint.S:222`
```assembly
smc #0  # ← 这里触发 SMC 异常，返回到 EL3
```

**返回到**：`bl31/aarch64/runtime_exceptions.S:sync_handler64`

### 2. 第二次返回：EL3 SMC 处理 → TSPD

**位置**：`bl31/aarch64/runtime_exceptions.S:sync_handler64`
```assembly
# 路由到 TSPD 的 SMC 处理函数
```

**返回到**：`services/spd/tspd/tspd_main.c:tspd_smc_handler()`

### 3. 第三次返回：TSPD SMC 处理 → 同步退出

**位置**：`services/spd/tspd/tspd_main.c:515`
```c
tspd_synchronous_sp_exit(tsp_ctx, x1);
```

**返回到**：`services/spd/tspd/tspd_common.c:100`

### 4. 第四次返回：同步退出 → 汇编退出

**位置**：`services/spd/tspd/tspd_common.c:108`
```c
tspd_exit_sp(tsp_ctx->c_rt_ctx, ret);
```

**返回到**：`services/spd/tspd/tspd_helpers.S:57`

### 5. 第五次返回：汇编退出 → 同步入口

**位置**：`services/spd/tspd/tspd_helpers.S:78`
```assembly
ret  # ← 返回到调用 tspd_enter_sp 的地方
```

**返回到**：`services/spd/tspd/tspd_common.c:83`（`tspd_enter_sp` 调用之后）

### 6. 第六次返回：同步入口 → tspd_init

**位置**：`services/spd/tspd/tspd_common.c:88`
```c
return rc;  # rc = 向量表地址
```

**返回到**：`services/spd/tspd/tspd_main.c:326`（`tspd_synchronous_sp_entry` 调用之后）

### 7. 第七次返回：tspd_init → bl31_main

**位置**：`services/spd/tspd/tspd_main.c:329`
```c
return rc;  # rc = 向量表地址
```

**返回到**：`bl31/bl31_main.c:195`（`bl32_init()` 调用之后）

## 详细代码追踪

### 步骤 1：TSP 返回

```assembly
# bl32/tsp/aarch64/tsp_entrypoint.S:214-222
bl  tsp_main              # 调用 tsp_main
                          # 返回后，x0 = &tsp_vector_table

mov x1, x0                # x1 = 向量表地址
mov x0, #TSP_ENTRY_DONE   # x0 = TSP_ENTRY_DONE (0xf2000000)
smc #0                    # ← 触发 SMC 异常，返回到 EL3
```

### 步骤 2：EL3 处理 SMC

```assembly
# bl31/aarch64/runtime_exceptions.S:334
sync_handler64:
    # ... 保存上下文 ...
    # 调用 C 层 SMC 处理函数
    bl  handle_sync_exception
    # 这会路由到 tspd_smc_handler
```

### 步骤 3：TSPD 处理 TSP_ENTRY_DONE

```c
// services/spd/tspd/tspd_main.c:438
case TSP_ENTRY_DONE:
    // x1 包含向量表地址（从 TSP 的 x1 传递过来）
    tsp_vectors = (tsp_vectors_t *) x1;
    
    // ... 其他初始化 ...
    
    // 关键：返回到调用 tspd_synchronous_sp_entry 的地方
    tspd_synchronous_sp_exit(tsp_ctx, x1);  # ← x1 是向量表地址
    break;
```

### 步骤 4：同步退出

```c
// services/spd/tspd/tspd_common.c:100
void tspd_synchronous_sp_exit(tsp_context_t *tsp_ctx, uint64_t ret)
{
    // 保存 S-EL1 系统寄存器上下文
    cm_el1_sysregs_context_save(SECURE);
    
    // 恢复 EL3 C 运行时上下文
    // ret 参数（向量表地址）会传递给 tspd_exit_sp
    tspd_exit_sp(tsp_ctx->c_rt_ctx, ret);
}
```

### 步骤 5：汇编退出

```assembly
// services/spd/tspd/tspd_helpers.S:57
func tspd_exit_sp
    // x0 = 保存的栈指针（C 运行时上下文）
    // x1 = ret 参数（向量表地址）
    
    mov sp, x0              # 恢复栈指针
    
    // 恢复 callee-saved 寄存器
    ldp x19, x20, [x0, #(TSPD_C_RT_CTX_X19 - TSPD_C_RT_CTX_SIZE)]
    // ... 恢复其他寄存器 ...
    
    mov x0, x1              # x0 = 向量表地址（作为返回值）
    ret                     # ← 返回到 tspd_enter_sp 调用之后
endfunc tspd_exit_sp
```

**返回到**：`tspd_common.c:83`（`tspd_enter_sp` 调用之后）

### 步骤 6：同步入口返回

```c
// services/spd/tspd/tspd_common.c:71
uint64_t tspd_synchronous_sp_entry(tsp_context_t *tsp_ctx)
{
    // ... 设置上下文 ...
    
    rc = tspd_enter_sp(&tsp_ctx->c_rt_ctx);
    // ↑ 返回到这里，rc = x0 = 向量表地址
    
    return rc;  # ← 返回到 tspd_init:326
}
```

### 步骤 7：tspd_init 返回

```c
// services/spd/tspd/tspd_main.c:306
int32_t tspd_init(void)
{
    // ... 初始化 ...
    
    rc = tspd_synchronous_sp_entry(tsp_ctx);
    // ↑ 返回到这里，rc = 向量表地址
    
    assert(rc != 0);  # 检查返回值非零（成功）
    
    return rc;  # ← 返回到 bl31_main:195
}
```

### 步骤 8：bl31_main 继续执行

```c
// bl31/bl31_main.c:191
if (bl32_init != NULL) {
    INFO("BL31: Initializing BL32\n");
    
    console_flush();
    int32_t rc = (*bl32_init)();  # ← 调用 tspd_init
    // ↑ 返回到这里，rc = 向量表地址
    
    if (rc == 0) {
        WARN("BL31: BL32 initialization failed\n");
    }
}

# 继续执行后续代码（准备进入 BL33）
```

## 关键点总结

### 1. 返回值传递链

```
tsp_main() 
  → 返回 &tsp_vector_table (在 x0)
    ↓
tsp_entrypoint.S
  → mov x1, x0 (保存到 x1)
  → smc #0 (通过 SMC 传递)
    ↓
EL3 SMC 处理
  → x1 包含向量表地址
    ↓
tspd_smc_handler()
  → tspd_synchronous_sp_exit(tsp_ctx, x1)
    ↓
tspd_exit_sp(c_rt_ctx, ret)
  → mov x0, x1 (x0 = 向量表地址)
  → ret (返回到 tspd_synchronous_sp_entry)
    ↓
tspd_synchronous_sp_entry()
  → return rc (rc = 向量表地址)
    ↓
tspd_init()
  → return rc (rc = 向量表地址)
    ↓
bl31_main()
  → rc = (*bl32_init)() (rc = 向量表地址)
```

### 2. 最终返回位置

**`tsp_main` 返回后，最终返回到**：

```c
// bl31/bl31_main.c:195
int32_t rc = (*bl32_init)();
// ↑ 返回到这里
// rc = 向量表地址（非零，表示成功）
```

**然后继续执行**：
```c
if (rc == 0) {
    WARN("BL31: BL32 initialization failed\n");
}
// 继续执行 bl31_main 的后续代码
```

### 3. 上下文恢复

- **EL3 C 运行时上下文**：通过 `tspd_exit_sp` 恢复（x19-x30）
- **S-EL1 系统寄存器**：通过 `cm_el1_sysregs_context_save` 保存
- **返回值**：向量表地址通过 x0 寄存器传递

## 调试技巧

### 添加调试打印

```c
// 在 tspd_init 中
int32_t tspd_init(void)
{
    INFO("TSPD: About to enter TSP\n");
    rc = tspd_synchronous_sp_entry(tsp_ctx);
    INFO("TSPD: Returned from TSP, rc=0x%lx\n", rc);  # ← 应该看到这里
    return rc;
}
```

```c
// 在 bl31_main 中
int32_t rc = (*bl32_init)();
INFO("BL31: Returned from BL32 init, rc=0x%lx\n", rc);  # ← 应该看到这里
```

## 总结

**`tsp_main` 返回后，最终返回到 `bl31_main.c:195`**，即调用 `bl32_init()` 的地方。

**返回值**：向量表地址（非零，表示 TSP 初始化成功）

**后续**：BL31 继续执行，准备进入 BL33（Normal World）

