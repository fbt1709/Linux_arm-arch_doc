# TSPD 和 TSP 源码详解

## 目录结构

### TSPD (Test Secure Payload Dispatcher)
```
services/spd/tspd/
├── tspd_main.c      # 主逻辑：SMC 处理、初始化、中断处理
├── tspd_common.c    # 通用函数：上下文初始化、同步入口/退出
├── tspd_helpers.S   # 汇编辅助函数：上下文切换
├── tspd_pm.c        # 电源管理：CPU ON/OFF/SUSPEND/RESUME
├── tspd_private.h   # 私有头文件：数据结构、宏定义
└── tspd.mk          # Makefile
```

### TSP (Test Secure Payload)
```
bl32/tsp/
├── tsp_main.c           # 主逻辑：初始化、SMC 处理
├── tsp_common.c         # 通用函数
├── tsp_context.c        # 上下文管理
├── tsp_interrupt.c      # 中断处理
├── tsp_timer.c          # 定时器管理
├── tsp_private.h         # 私有头文件
├── aarch64/
│   └── tsp_entrypoint.S  # 入口点和向量表
└── tsp.mk                # Makefile
```

---

## 一、关键数据结构

### 1.1 TSPD 上下文结构 (`tsp_context_t`)

```c
// tspd_private.h:181
typedef struct tsp_context {
    uint64_t saved_elr_el3;        // 保存的 ELR_EL3（用于中断处理）
    uint32_t saved_spsr_el3;       // 保存的 SPSR_EL3
    uint32_t state;                // TSP 状态标志
    uint64_t mpidr;                // CPU ID
    uint64_t c_rt_ctx;             // C 运行时上下文指针
    cpu_context_t cpu_ctx;          // CPU 架构上下文（寄存器）
    uint64_t saved_tsp_args[2];    // 保存的 TSP 参数
#if TSP_NS_INTR_ASYNC_PREEMPT
    sp_ctx_regs_t sp_ctx;          // SP 调用者保存寄存器
    bool preempted_by_sel1_intr;   // 是否被 S-EL1 中断抢占
#endif
} tsp_context_t;
```

**状态标志 (`state`)：**
- `TSP_PSTATE_OFF` (0): TSP 关闭
- `TSP_PSTATE_ON` (1): TSP 运行中
- `TSP_PSTATE_SUSPEND` (2): TSP 挂起
- `YIELD_SMC_ACTIVE_FLAG`: Yielding SMC 是否活跃

### 1.2 TSP 向量表 (`tsp_vectors_t`)

```c
// bl32/tsp/tsp.h:96
typedef struct tsp_vectors {
    tsp_vector_isn_t yield_smc_entry;      // Yielding SMC 入口
    tsp_vector_isn_t fast_smc_entry;        // Fast SMC 入口
    tsp_vector_isn_t cpu_on_entry;         // CPU ON 入口
    tsp_vector_isn_t cpu_off_entry;        // CPU OFF 入口
    tsp_vector_isn_t cpu_resume_entry;     // CPU Resume 入口
    tsp_vector_isn_t cpu_suspend_entry;    // CPU Suspend 入口
    tsp_vector_isn_t sel1_intr_entry;      // S-EL1 中断入口
    tsp_vector_isn_t system_off_entry;     // 系统关闭入口
    tsp_vector_isn_t system_reset_entry;   // 系统复位入口
    tsp_vector_isn_t abort_yield_smc_entry;// 中止 Yielding SMC 入口
} tsp_vectors_t;
```

### 1.3 TSP 统计信息 (`work_statistics_t`)

```c
// tsp_private.h:27
typedef struct work_statistics {
    uint32_t sel1_intr_count;        // S-EL1 中断次数
    uint32_t preempt_intr_count;     // 抢占中断次数
    uint32_t sync_sel1_intr_count;   // 同步 S-EL1 中断次数
    uint32_t sync_sel1_intr_ret_count; // 同步 S-EL1 中断返回次数
    uint32_t smc_count;              // SMC 调用次数
    uint32_t eret_count;             // ERET 进入次数
    uint32_t cpu_on_count;           // CPU ON 请求次数
    uint32_t cpu_off_count;          // CPU OFF 请求次数
    uint32_t cpu_suspend_count;       // CPU SUSPEND 请求次数
    uint32_t cpu_resume_count;       // CPU RESUME 请求次数
} work_statistics_t;
```

---

## 二、初始化流程

### 2.1 TSPD 初始化

#### 步骤 1: 注册运行时服务 (`tspd_setup`)

```c
// tspd_main.c:247
static int32_t tspd_setup(void)
{
    // 1. 获取 TSP 入口点信息（从 BL2 传递）
    tsp_ep_info = bl31_plat_get_next_image_ep_info(SECURE);
    
    // 2. 初始化 TSP 入口点状态
    tspd_init_tsp_ep_state(tsp_ep_info,
                           TSP_AARCH64,
                           tsp_ep_info->pc,
                           &tspd_sp_context[linear_id]);
    
    // 3. 注册初始化函数到 BL31
    bl31_register_bl32_init(&tspd_init);
}
```

#### 步骤 2: TSP 入口点初始化 (`tspd_init_tsp_ep_state`)

```c
// tspd_common.c:24
void tspd_init_tsp_ep_state(...)
{
    // 1. 初始化 TSP 上下文
    tsp_ctx->mpidr = read_mpidr_el1();
    tsp_ctx->state = 0;
    set_tsp_pstate(tsp_ctx->state, TSP_PSTATE_OFF);
    
    // 2. 设置上下文为 SECURE
    cm_set_context(&tsp_ctx->cpu_ctx, SECURE);
    
    // 3. 设置入口点信息
    tsp_entry_point->pc = pc;
    tsp_entry_point->spsr = SPSR_64(MODE_EL1, MODE_SP_ELX, 
                                     DISABLE_ALL_EXCEPTIONS);
}
```

#### 步骤 3: 首次进入 TSP (`tspd_init`)

```c
// tspd_main.c:306
int32_t tspd_init(void)
{
    // 1. 获取 TSP 入口点
    tsp_entry_point = bl31_plat_get_next_image_ep_info(SECURE);
    
    // 2. 初始化当前 CPU 上下文
    cm_init_my_context(tsp_entry_point);
    
    // 3. 同步进入 TSP（第一次调用）
    rc = tspd_synchronous_sp_entry(tsp_ctx);
    // TSP 会通过 SMC 返回，携带向量表地址
}
```

### 2.2 TSP 初始化

#### 步骤 1: TSP 入口点 (`tsp_entrypoint`)

```assembly
// tsp_entrypoint.S:53
func tsp_entrypoint
    // 1. 保存 BL2 传递的参数
    mov x20, x0  // 保存 x0-x3
    mov x21, x1
    mov x22, x2
    mov x23, x3
    
    // 2. 设置异常向量表
    adr x0, tsp_exceptions
    msr vbar_el1, x0
    
    // 3. 启用指令缓存、对齐检查
    mov x1, #(SCTLR_I_BIT | SCTLR_A_BIT | SCTLR_SA_BIT)
    mrs x0, sctlr_el1
    orr x0, x0, x1
    msr sctlr_el1, x0
    
    // 4. 初始化内存（清零 BSS）
    // ...
    
    // 5. 调用 C 主函数
    bl tsp_main
    // tsp_main 返回向量表地址
    
    // 6. 通过 SMC 返回向量表地址给 TSPD
    mov x1, x0  // x1 = 向量表地址
    mov x0, #TSP_ENTRY_DONE
    smc #0
endfunc tsp_entrypoint
```

#### 步骤 2: TSP 主函数 (`tsp_main`)

```c
// tsp_main.c:30
uint64_t tsp_main(void)
{
    // 1. 初始化平台
    tsp_platform_setup();
    
    // 2. 启动通用定时器
    tsp_generic_timer_start();
    
    // 3. 更新统计信息
    tsp_stats[linear_id].smc_count++;
    tsp_stats[linear_id].eret_count++;
    tsp_stats[linear_id].cpu_on_count++;
    
    // 4. 返回向量表地址
    return (uint64_t) &tsp_vector_table;
}
```

#### 步骤 3: TSPD 接收向量表 (`TSP_ENTRY_DONE`)

```c
// tspd_main.c:438
case TSP_ENTRY_DONE:
    // x1 包含向量表地址
    tsp_vectors = (tsp_vectors_t *) x1;
    
    // 设置 TSP 状态为 ON
    set_tsp_pstate(tsp_ctx->state, TSP_PSTATE_ON);
    
    // 注册电源管理钩子
    psci_register_spd_pm_hook(&tspd_pm);
    
    // 注册 S-EL1 中断处理函数
    register_interrupt_type_handler(INTR_TYPE_S_EL1,
                                    tspd_sel1_interrupt_handler, ...);
```

---

## 三、上下文切换机制

### 3.1 进入 TSP (EL3 → S-EL1)

#### C 层：`tspd_synchronous_sp_entry`

```c
// tspd_common.c:71
uint64_t tspd_synchronous_sp_entry(tsp_context_t *tsp_ctx)
{
    // 1. 恢复 S-EL1 系统寄存器上下文
    cm_el1_sysregs_context_restore(SECURE);
    
    // 2. 设置下一个 ERET 上下文为 SECURE
    cm_set_next_eret_context(SECURE);
    
    // 3. 调用汇编函数进入 TSP
    rc = tspd_enter_sp(&tsp_ctx->c_rt_ctx);
    // 这会保存 EL3 C 运行时上下文，然后 ERET 进入 TSP
    
    return rc;
}
```

#### 汇编层：`tspd_enter_sp`

```assembly
// tspd_helpers.S:21
func tspd_enter_sp
    // 1. 保存当前栈指针
    mov x3, sp
    str x3, [x0, #0]  // 保存到 c_rt_ctx
    
    // 2. 在栈上分配空间保存 callee-saved 寄存器
    sub sp, sp, #TSPD_C_RT_CTX_SIZE
    
    // 3. 保存 callee-saved 寄存器 (x19-x30)
    stp x19, x20, [sp, #TSPD_C_RT_CTX_X19]
    stp x21, x22, [sp, #TSPD_C_RT_CTX_X21]
    stp x23, x24, [sp, #TSPD_C_RT_CTX_X23]
    stp x25, x26, [sp, #TSPD_C_RT_CTX_X25]
    stp x27, x28, [sp, #TSPD_C_RT_CTX_X27]
    stp x29, x30, [sp, #TSPD_C_RT_CTX_X29]
    
    // 4. 调用 el3_exit()，它会：
    //    - 从 tsp_ctx->cpu_ctx 恢复通用寄存器和 EL3 系统寄存器
    //    - 从 ELR_EL3 恢复程序计数器
    //    - 从 SPSR_EL3 恢复处理器状态
    //    - 执行 ERET 进入 S-EL1
    b el3_exit
endfunc tspd_enter_sp
```

### 3.2 退出 TSP (S-EL1 → EL3)

TSP 通过 SMC 返回到 TSPD：

```c
// TSP 中调用
return set_smc_args(TSP_ON_DONE, 0, 0, 0, 0, 0, 0, 0);
// 这会触发 SMC 异常，返回到 EL3
```

TSPD 处理返回：

```c
// tspd_common.c:100
void tspd_synchronous_sp_exit(tsp_context_t *tsp_ctx, uint64_t ret)
{
    // 1. 保存 S-EL1 系统寄存器上下文
    cm_el1_sysregs_context_save(SECURE);
    
    // 2. 恢复 EL3 C 运行时上下文
    tspd_exit_sp(tsp_ctx->c_rt_ctx, ret);
}
```

汇编层：`tspd_exit_sp`

```assembly
// tspd_helpers.S:57
func tspd_exit_sp
    // 1. 恢复栈指针
    mov sp, x0  // x0 是保存的栈指针
    
    // 2. 恢复 callee-saved 寄存器
    ldp x19, x20, [x0, #(TSPD_C_RT_CTX_X19 - TSPD_C_RT_CTX_SIZE)]
    ldp x21, x22, [x0, #(TSPD_C_RT_CTX_X21 - TSPD_C_RT_CTX_SIZE)]
    // ... 恢复其他寄存器
    
    // 3. 设置返回值
    mov x0, x1  // x1 是返回值的参数
    
    // 4. 返回到调用者
    ret
endfunc tspd_exit_sp
```

---

## 四、SMC 处理流程

### 4.1 Normal World → TSPD

```
Normal World (TFTF/Linux)
    │
    │ SMC #0 (TSP_FAST_FID(TSP_ADD), x1, x2, ...)
    ▼
EL3: SMC 异常处理
    │
    │ 路由到 TSPD
    ▼
TSPD: tspd_smc_handler()
```

### 4.2 TSPD SMC 处理 (`tspd_smc_handler`)

```c
// tspd_main.c:341
static uintptr_t tspd_smc_handler(uint32_t smc_fid, ...)
{
    // 1. 判断调用者（NS 或 S）
    ns = is_caller_non_secure(flags);
    
    switch (smc_fid) {
    // TSP 内部使用的 SMC（只能从 TSP 调用）
    case TSP_ENTRY_DONE:
    case TSP_ON_DONE:
    case TSP_OFF_DONE:
        if (ns)  // 非安全世界不能调用
            SMC_RET1(handle, SMC_UNK);
        // 处理 TSP 返回...
        break;
    
    // 正常世界请求的服务
    case TSP_FAST_FID(TSP_ADD):
    case TSP_YIELD_FID(TSP_ADD):
        if (!ns)  // 必须从非安全世界调用
            SMC_RET1(handle, SMC_UNK);
        
        // 1. 保存非安全上下文
        cm_el1_sysregs_context_save(NON_SECURE);
        
        // 2. 根据 SMC 类型设置 TSP 入口点
        if (GET_SMC_TYPE(smc_fid) == SMC_TYPE_FAST) {
            cm_set_elr_el3(SECURE, 
                          (uint64_t) &tsp_vectors->fast_smc_entry);
        } else {
            set_yield_smc_active_flag(tsp_ctx->state);
            cm_set_elr_el3(SECURE, 
                          (uint64_t) &tsp_vectors->yield_smc_entry);
        }
        
        // 3. 恢复安全上下文并进入 TSP
        cm_el1_sysregs_context_restore(SECURE);
        cm_set_next_eret_context(SECURE);
        SMC_RET3(&tsp_ctx->cpu_ctx, smc_fid, x1, x2);
        // 这会 ERET 进入 TSP
    }
}
```

### 4.3 TSP SMC 处理 (`tsp_smc_handler`)

```c
// tsp_main.c:204
smc_args_t *tsp_smc_handler(uint64_t func,
                            uint64_t arg1,
                            uint64_t arg2, ...)
{
    // 1. 更新统计信息
    tsp_stats[linear_id].smc_count++;
    tsp_stats[linear_id].eret_count++;
    
    // 2. 获取服务参数（从 TSPD 请求）
    service_args = tsp_get_magic();
    service_arg0 = (uint64_t)service_args;
    service_arg1 = (uint64_t)(service_args >> 64U);
    
    // 3. 处理服务请求
    switch (TSP_BARE_FID(func)) {
    case TSP_ADD:
        results[0] = arg1 + service_arg0;
        results[1] = arg2 + service_arg1;
        break;
    case TSP_SUB:
        results[0] = arg1 - service_arg0;
        results[1] = arg2 - service_arg1;
        break;
    // ... 其他操作
    }
    
    // 4. 通过 SMC 返回结果
    return set_smc_args(func, 0, results[0], results[1], 0, 0, 0, 0);
}
```

### 4.4 TSP 向量表入口

```assembly
// tsp_entrypoint.S:234
vector_base tsp_vector_table
    b   tsp_yield_smc_entry    // Yielding SMC 入口
    b   tsp_fast_smc_entry      // Fast SMC 入口
    b   tsp_cpu_on_entry        // CPU ON 入口
    // ... 其他入口

// Fast SMC 入口
func tsp_fast_smc_entry
    bl  tsp_smc_handler
    restore_args_call_smc  // 恢复参数并调用 SMC 返回
endfunc tsp_fast_smc_entry

// Yielding SMC 入口
func tsp_yield_smc_entry
    bl  tsp_smc_handler
    restore_args_call_smc
endfunc tsp_yield_smc_entry
```

---

## 五、中断处理

### 5.1 S-EL1 中断处理

#### TSPD 中断处理函数

```c
// tspd_main.c:108
static uint64_t tspd_sel1_interrupt_handler(uint32_t id,
                                            uint32_t flags,
                                            void *handle,
                                            void *cookie)
{
    // 1. 获取 TSP 上下文
    tsp_ctx = &tspd_sp_context[linear_id];
    
    // 2. 保存非安全上下文（如果 TSP 被非安全中断抢占）
    if (get_interrupt_src_ss(flags) == NON_SECURE) {
        cm_el1_sysregs_context_save(NON_SECURE);
    }
    
    // 3. 设置 TSP 中断入口点
    cm_set_elr_spsr_el3(SECURE,
                       (uint64_t) &tsp_vectors->sel1_intr_entry,
                       SPSR_64(MODE_EL1, MODE_SP_ELX, ...));
    
    // 4. 通过 SMC 通知 TSP 处理中断
    SMC_RET2(&tsp_ctx->cpu_ctx,
             TSP_HANDLE_SEL1_INTR_AND_RETURN,
             read_elr_el3());
}
```

#### TSP 中断处理

```c
// tsp_interrupt.c
int32_t tsp_common_int_handler(void)
{
    // 1. 更新统计信息
    tsp_stats[linear_id].sel1_intr_count++;
    
    // 2. 处理中断
    // ...
    
    // 3. 通过 SMC 返回
    return set_smc_args(TSP_HANDLED_S_EL1_INTR, 0, 0, 0, 0, 0, 0, 0);
}
```

---

## 六、电源管理

### 6.1 CPU ON

```c
// tspd_pm.c:109
static void tspd_cpu_on_finish_handler(u_register_t unused)
{
    // 1. 设置 TSP CPU ON 入口点
    cm_set_elr_el3(SECURE, 
                  (uint64_t) &tsp_vectors->cpu_on_entry);
    
    // 2. 进入 TSP
    rc = tspd_synchronous_sp_entry(tsp_ctx);
    
    // 3. 设置 TSP 状态为 ON
    set_tsp_pstate(tsp_ctx->state, TSP_PSTATE_ON);
}
```

### 6.2 CPU OFF

```c
// tspd_pm.c:30
static int32_t tspd_cpu_off_handler(u_register_t unused)
{
    // 1. 设置 TSP CPU OFF 入口点
    cm_set_elr_el3(SECURE,
                  (uint64_t) &tsp_vectors->cpu_off_entry);
    
    // 2. 进入 TSP
    rc = tspd_synchronous_sp_entry(tsp_ctx);
    
    // 3. 设置 TSP 状态为 OFF
    set_tsp_pstate(tsp_ctx->state, TSP_PSTATE_OFF);
}
```

### 6.3 CPU SUSPEND/RESUME

类似流程，TSPD 设置相应的入口点，进入 TSP 执行挂起/恢复操作。

---

## 七、关键函数总结

### TSPD 关键函数

| 函数 | 文件 | 功能 |
|------|------|------|
| `tspd_setup` | tspd_main.c | 初始化 TSPD，注册运行时服务 |
| `tspd_init` | tspd_main.c | 首次进入 TSP |
| `tspd_smc_handler` | tspd_main.c | 处理所有 SMC 调用 |
| `tspd_synchronous_sp_entry` | tspd_common.c | 同步进入 TSP |
| `tspd_synchronous_sp_exit` | tspd_common.c | 同步退出 TSP |
| `tspd_enter_sp` | tspd_helpers.S | 汇编：保存 EL3 上下文，进入 TSP |
| `tspd_exit_sp` | tspd_helpers.S | 汇编：恢复 EL3 上下文 |
| `tspd_sel1_interrupt_handler` | tspd_main.c | 处理 S-EL1 中断 |

### TSP 关键函数

| 函数 | 文件 | 功能 |
|------|------|------|
| `tsp_entrypoint` | tsp_entrypoint.S | TSP 入口点（汇编） |
| `tsp_main` | tsp_main.c | TSP 主函数，初始化 |
| `tsp_smc_handler` | tsp_main.c | 处理 SMC 调用 |
| `tsp_cpu_on_main` | tsp_main.c | CPU ON 处理 |
| `tsp_cpu_off_main` | tsp_main.c | CPU OFF 处理 |
| `tsp_cpu_suspend_main` | tsp_main.c | CPU SUSPEND 处理 |
| `tsp_cpu_resume_main` | tsp_main.c | CPU RESUME 处理 |
| `tsp_common_int_handler` | tsp_interrupt.c | 中断处理 |

---

## 八、数据流图

### 初始化流程
```
BL31 启动
  │
  ├─> runtime_svc_init()
  │     │
  │     └─> tspd_setup()  [注册运行时服务]
  │           │
  │           └─> bl31_register_bl32_init(&tspd_init)
  │
  └─> bl31_main()
        │
        └─> bl32_init()  [调用注册的初始化函数]
              │
              └─> tspd_init()
                    │
                    ├─> cm_init_my_context()  [初始化上下文]
                    │
                    └─> tspd_synchronous_sp_entry()
                          │
                          ├─> cm_el1_sysregs_context_restore()
                          ├─> tspd_enter_sp()  [保存 EL3 上下文]
                          └─> el3_exit() → ERET → S-EL1
                                │
                                └─> tsp_entrypoint()
                                      │
                                      ├─> 初始化 TSP
                                      ├─> tsp_main()
                                      │     │
                                      │     └─> return &tsp_vector_table
                                      │
                                      └─> SMC(TSP_ENTRY_DONE, vector_table)
                                            │
                                            └─> tspd_smc_handler()
                                                  │
                                                  └─> tsp_vectors = vector_table
```

### SMC 调用流程
```
Normal World
  │
  │ SMC(TSP_FAST_FID(TSP_ADD), x1, x2)
  ▼
EL3: SMC 异常
  │
  └─> tspd_smc_handler()
        │
        ├─> cm_el1_sysregs_context_save(NON_SECURE)
        ├─> cm_set_elr_el3(SECURE, &tsp_vectors->fast_smc_entry)
        ├─> cm_el1_sysregs_context_restore(SECURE)
        └─> SMC_RET3() → ERET → S-EL1
              │
              └─> tsp_fast_smc_entry()
                    │
                    └─> tsp_smc_handler()
                          │
                          ├─> 处理服务请求
                          └─> SMC(func, results...) → EL3
                                │
                                └─> tspd_smc_handler()
                                      │
                                      ├─> cm_el1_sysregs_context_save(SECURE)
                                      ├─> cm_el1_sysregs_context_restore(NON_SECURE)
                                      └─> SMC_RET3() → Normal World
```

---

## 九、总结

### TSPD 职责
1. **SMC 路由**：将 Normal World 的 SMC 调用路由到 TSP
2. **上下文管理**：管理 EL3 和 S-EL1 之间的上下文切换
3. **中断处理**：处理 S-EL1 中断，路由到 TSP
4. **生命周期管理**：管理 TSP 的启动、暂停、恢复、关闭
5. **状态跟踪**：跟踪 TSP 的状态（ON/OFF/SUSPEND）

### TSP 职责
1. **提供服务**：提供简单的安全服务（算术运算等）
2. **状态管理**：管理自己的执行状态
3. **中断处理**：处理 S-EL1 中断
4. **统计信息**：收集和报告统计信息

### 关键设计点
1. **向量表机制**：TSP 通过向量表提供多个入口点
2. **同步/异步调用**：支持 Fast SMC 和 Yielding SMC
3. **上下文隔离**：EL3 和 S-EL1 的上下文完全隔离
4. **状态机**：TSP 状态通过状态机管理

