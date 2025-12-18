# TSPD中NS中断处理流程：从EL3直接返回到TFTF

## 关键理解

**NS中断的检测和处理完全在TSPD（EL3）中进行，不会进入TSP（S-EL1）。直接从TSPD返回到TFTF（Non-Secure世界）。**

## 完整流程

### 阶段1：TSP在S-EL1执行（yielding SMC）

```
TSP (S-EL1)
  │
  │ tsp_yield_smc_entry
  │   msr daifclr  // enable中断
  │   bl tsp_smc_handler
  │   ...
```

**状态**：
- TSP在S-EL1执行yielding SMC
- TSP已enable中断（`daifclr`）
- NS SGI处于pending状态（在GIC中）

### 阶段2：NS中断路由到EL3（硬件自动）

```
NS SGI pending
  ↓
硬件检测到pending中断
  ↓
检查中断路由配置（TEL3=1，已由enable_intr_rm_local设置）
  ↓
中断路由到EL3（不路由到S-EL1）
  ↓
中断异常触发，跳转到EL3异常向量表
```

**关键**：
- ✅ **中断直接路由到EL3**：由于`enable_intr_rm_local(INTR_TYPE_NS, SECURE)`已设置路由（`CSS=0, TEL3=1`）
- ✅ **不会进入TSP**：中断异常直接触发到EL3，不会先到S-EL1
- ✅ **TSP被抢占**：TSP的执行被中断异常打断，但异常处理在EL3

### 阶段3：EL3中断框架检测并调用TSPD handler

```
EL3异常向量表
  ↓
EL3中断框架检测到NS中断
  ↓
查找注册的handler（tspd_ns_interrupt_handler）
  ↓
调用tspd_ns_interrupt_handler
```

**关键**：
- ✅ **EL3中断框架自动检测**：不需要TSPD主动检查
- ✅ **直接调用handler**：框架自动路由到注册的handler

### 阶段4：TSPD处理NS中断（在EL3中）

```c
// services/spd/tspd/tspd_main.c:224-239
static uint64_t tspd_ns_interrupt_handler(uint32_t id,
                    uint32_t flags,
                    void *handle,
                    void *cookie)
{
    /* Check the security state when the exception was generated */
    assert(get_interrupt_src_ss(flags) == SECURE);

    /*
     * Disable the routing of NS interrupts from secure world to EL3 while
     * interrupted on this core.
     */
    disable_intr_rm_local(INTR_TYPE_NS, SECURE);

    return tspd_handle_sp_preemption(handle);  // ← 处理抢占
}
```

**关键**：
- ✅ **在EL3中执行**：`tspd_ns_interrupt_handler`在EL3中执行，不在TSP中
- ✅ **验证安全状态**：确保中断是在Secure状态生成的
- ✅ **禁用路由**：禁用NS中断路由到EL3（因为即将返回到Non-Secure）
- ✅ **调用抢占处理**：调用`tspd_handle_sp_preemption`处理抢占

### 阶段5：TSPD处理抢占并返回到TFTF

```c
// services/spd/tspd/tspd_main.c:60-89
uint64_t tspd_handle_sp_preemption(void *handle)
{
    cpu_context_t *ns_cpu_context;

    assert(handle == cm_get_context(SECURE));
    cm_el1_sysregs_context_save(SECURE);  // ← 保存TSP状态（TSP被抢占时的状态）

    /* Get a reference to the non-secure context */
    ns_cpu_context = cm_get_context(NON_SECURE);
    assert(ns_cpu_context);

    /*
     * Restore non-secure state.
     */
    cm_el1_sysregs_context_restore(NON_SECURE);  // ← 恢复Non-Secure上下文
    cm_set_next_eret_context(NON_SECURE);         // ← 设置返回到Non-Secure

    /*
     * The TSP was preempted during execution of a Yielding SMC Call.
     * Return back to the normal world with SMC_PREEMPTED as error
     * code in x0.
     */
    SMC_RET1(ns_cpu_context, SMC_PREEMPTED);  // ← 直接返回到TFTF
}
```

**关键**：
- ✅ **保存TSP状态**：保存TSP被抢占时的寄存器状态（65行）
- ✅ **恢复Non-Secure上下文**：恢复TFTF的寄存器状态（80行）
- ✅ **设置返回目标**：设置返回到Non-Secure世界（81行）
- ✅ **直接返回**：通过`SMC_RET1`直接返回到TFTF，返回值是`SMC_PREEMPTED`（88行）

## 完整时序图

```
TSP (S-EL1)                    EL3 (TSPD)                    TFTF (Non-Secure)
─────────────────              ───────────                    ──────────────────
tsp_yield_smc_entry
  │
  │ msr daifclr (enable中断)
  │
  │ bl tsp_smc_handler
  │   ...
  │
  │ ← NS SGI pending（在GIC中）
  │
  │ 中断异常触发
  │ （硬件检测到pending中断）
  │
  │ 检查路由配置（TEL3=1）
  │
  │ 中断路由到EL3 ← 关键：直接到EL3，不到S-EL1
  │
  ├────────────────────────────>│
  │                              │ EL3异常向量表
  │                              │
  │                              │ EL3中断框架检测到NS中断
  │                              │
  │                              │ 调用tspd_ns_interrupt_handler
  │                              │
  │                              │ tspd_ns_interrupt_handler()
  │                              │   - 验证安全状态
  │                              │   - disable_intr_rm_local()
  │                              │   - 调用tspd_handle_sp_preemption()
  │                              │
  │                              │ tspd_handle_sp_preemption()
  │                              │   - cm_el1_sysregs_context_save(SECURE)  // 保存TSP状态
  │                              │   - cm_el1_sysregs_context_restore(NON_SECURE)  // 恢复TFTF状态
  │                              │   - cm_set_next_eret_context(NON_SECURE)
  │                              │   - SMC_RET1(ns_cpu_context, SMC_PREEMPTED)
  │                              │
  │<─────────────────────────────┤
  │                              │
tftf_smc() 返回
  │
  │ ret0 = SMC_PREEMPTED
  │
  │ enable_irq()  // 在Non-Secure世界enable中断
  │
  │ 处理SGI中断
```

## 关键理解

### 1. 中断路由机制

**关键配置**：
```c
// services/spd/tspd/tspd_main.c:632
enable_intr_rm_local(INTR_TYPE_NS, SECURE);
```

**作用**：
- 设置NS中断路由到EL3（`CSS=0, TEL3=1`）
- 当TSP在S-EL1执行时，NS中断直接路由到EL3
- **不会路由到S-EL1（TSP）**

### 2. 中断检测和处理

**完全在EL3中进行**：
- ✅ **硬件检测**：硬件检测到pending中断
- ✅ **路由到EL3**：由于路由配置，中断直接路由到EL3
- ✅ **EL3框架处理**：EL3中断框架检测并调用handler
- ✅ **TSPD处理**：`tspd_ns_interrupt_handler`在EL3中执行
- ✅ **直接返回**：从EL3直接返回到Non-Secure世界

**不会进入TSP**：
- ❌ 中断不会路由到S-EL1
- ❌ TSP的异常向量表不会被调用
- ❌ TSP的`tsp_common_int_handler`不会被调用

### 3. TSP的状态保存

**TSP被抢占时的状态**：
- TSP在S-EL1执行时被中断异常打断
- 中断异常直接路由到EL3，TSP的执行被暂停
- TSPD在`tspd_handle_sp_preemption`中保存TSP的状态（65行）
- 保存的状态包括：通用寄存器、系统寄存器、ELR_EL3、SPSR_EL3等

### 4. 返回到TFTF

**直接返回**：
- TSPD在EL3中处理完抢占后，直接返回到Non-Secure世界
- 不需要经过TSP
- 返回值是`SMC_PREEMPTED`，表示TSP被抢占

## 代码位置总结

### 1. 中断路由设置

**位置**：`services/spd/tspd/tspd_main.c:632`
```c
enable_intr_rm_local(INTR_TYPE_NS, SECURE);
```

### 2. 中断handler注册

**位置**：`services/spd/tspd/tspd_main.c:480-482`
```c
rc = register_interrupt_type_handler(INTR_TYPE_NS,
                tspd_ns_interrupt_handler,
                flags);
```

### 3. NS中断处理函数

**位置**：`services/spd/tspd/tspd_main.c:224-239`
```c
static uint64_t tspd_ns_interrupt_handler(...)
{
    disable_intr_rm_local(INTR_TYPE_NS, SECURE);
    return tspd_handle_sp_preemption(handle);
}
```

### 4. 抢占处理函数（返回到TFTF）

**位置**：`services/spd/tspd/tspd_main.c:60-89`
```c
uint64_t tspd_handle_sp_preemption(void *handle)
{
    cm_el1_sysregs_context_save(SECURE);
    cm_el1_sysregs_context_restore(NON_SECURE);
    cm_set_next_eret_context(NON_SECURE);
    SMC_RET1(ns_cpu_context, SMC_PREEMPTED);
}
```

## 与TSP的关系

### TSP的作用

**TSP在抢占过程中的作用**：
- ✅ **执行yielding SMC**：TSP在S-EL1执行yielding SMC
- ✅ **enable中断**：TSP enable中断，允许NS中断抢占
- ✅ **被抢占**：TSP的执行被中断异常打断

**TSP不参与中断处理**：
- ❌ **不检测中断**：TSP不检测pending的NS中断
- ❌ **不处理中断**：TSP的异常向量表不会被调用
- ❌ **不返回**：TSP不负责返回到Non-Secure世界

### TSPD的作用

**TSPD在抢占过程中的作用**：
- ✅ **设置路由**：设置NS中断路由到EL3
- ✅ **注册handler**：注册NS中断处理函数
- ✅ **检测中断**：EL3中断框架检测到NS中断，调用TSPD的handler
- ✅ **处理抢占**：保存TSP状态，恢复Non-Secure状态
- ✅ **返回TFTF**：直接返回到Non-Secure世界

## 总结

### 关键点

1. **检测在TSPD（EL3）中**：NS中断的检测和处理完全在EL3中进行
2. **不会进入TSP**：中断直接路由到EL3，不会进入TSP的异常向量表
3. **直接从TSPD返回**：TSPD处理完抢占后，直接返回到TFTF
4. **TSP被抢占**：TSP的执行被中断异常打断，状态被保存

### 流程总结

```
1. TSP在S-EL1执行yielding SMC，enable中断
2. NS SGI pending，硬件检测到pending中断
3. 由于路由配置（TEL3=1），中断直接路由到EL3
4. EL3中断框架检测到NS中断，调用tspd_ns_interrupt_handler
5. tspd_ns_interrupt_handler调用tspd_handle_sp_preemption
6. tspd_handle_sp_preemption保存TSP状态，恢复Non-Secure状态
7. 通过SMC_RET1直接返回到TFTF，返回值是SMC_PREEMPTED
```

### 答案

**NS中断的检测和处理完全在TSPD（EL3）中进行，不会进入TSP。TSPD检测到NS中断后，直接处理抢占并返回到TFTF，不需要经过TSP。**

