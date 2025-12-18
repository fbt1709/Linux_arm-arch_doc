# TSP 无法返回 EL3 的调试指南

## 问题描述

运行到 TSP 后，无法返回到 EL3。这通常是因为 SMC 异常没有被正确路由到 EL3。

## TSP 返回 EL3 的机制

### 1. TSP 返回流程

```
TSP (S-EL1)
    │
    │ 调用 set_smc_args() 设置返回值
    │
    │ 调用 restore_args_call_smc 宏
    │   - 从 smc_args_t 结构恢复 x0-x7
    │   - 执行 smc #0
    │
    ▼
SMC 异常（应该路由到 EL3）
    │
    ▼
EL3: runtime_exceptions.S
    │   - 检测到 SMC 异常
    │   - 调用 smc_handler64/smc_handler32
    │
    ▼
TSPD: tspd_smc_handler()
    │   - 处理 TSP 返回的 SMC
    │   - 恢复非安全上下文
    │   - 返回到 Normal World
```

### 2. 关键代码位置

#### TSP 返回代码

```assembly
# bl32/tsp/aarch64/tsp_entrypoint.S:31-36
.macro restore_args_call_smc
    ldp x6, x7, [x0, #SMC_ARG6]
    ldp x4, x5, [x0, #SMC_ARG4]
    ldp x2, x3, [x0, #SMC_ARG2]
    ldp x0, x1, [x0, #SMC_ARG0]
    smc #0  # ← 这里触发 SMC 异常
.endm
```

#### EL3 SMC 处理

```assembly
# bl31/aarch64/runtime_exceptions.S:99-104
/* Handle SMC exceptions separately from other synchronous exceptions */
cmp x30, #EC_AARCH32_SMC
b.eq smc_handler32

cmp x30, #EC_AARCH64_SMC
b.eq sync_handler64  # ← 应该跳转到这里
```

## 可能的原因和解决方案

### 原因 1：SCR_EL3.SMD 未设置（SMC 被禁用）

**症状**：SMC 指令不会触发异常，直接执行下一条指令

**检查方法**：
```c
// 在 TSP 中检查
uint64_t scr_el3 = read_scr_el3();
if ((scr_el3 & SCR_SMD_BIT) != 0) {
    ERROR("SMC is disabled! SCR_EL3.SMD = 1\n");
}
```

**解决方案**：
- 确保在进入 TSP 时，`SCR_EL3.SMD = 0`（SMC 启用）
- 检查 `context_mgmt.c` 中设置 `SCR_EL3` 的代码

### 原因 2：异常向量表未正确设置

**症状**：SMC 异常没有被 EL3 捕获

**检查方法**：
```c
// 在 TSP 中检查异常向量表
uint64_t vbar_el1 = read_vbar_el1();
INFO("TSP VBAR_EL1: 0x%lx\n", vbar_el1);

// 在 EL3 中检查
uint64_t vbar_el3 = read_vbar_el3();
INFO("EL3 VBAR_EL3: 0x%lx\n", vbar_el3);
```

**解决方案**：
- 确保 EL3 的异常向量表正确设置
- 确保 TSP 的异常向量表不会干扰 SMC 处理

### 原因 3：SMC 异常被 TSP 的异常向量表捕获

**症状**：SMC 异常被 TSP 的 `sync_exception_sp_elx` 处理，而不是 EL3

**检查方法**：
查看 `tsp_exceptions.S` 中的同步异常处理：

```assembly
# bl32/tsp/aarch64/tsp_exceptions.S:104-106
vector_entry sync_exception_sp_elx
    b plat_panic_handler  # ← 如果 SMC 被这里捕获，会 panic
end_vector_entry sync_exception_sp_elx
```

**解决方案**：
- SMC 异常应该**自动路由到 EL3**，不应该被 S-EL1 捕获
- 如果被 S-EL1 捕获，说明 `SCR_EL3.SMD = 0` 但异常路由有问题

### 原因 4：SCR_EL3 配置错误

**关键位**：
- `SCR_EL3.SMD`：必须为 0（SMC 启用）
- `SCR_EL3.NS`：在 TSP 中应该为 0（Secure 状态）
- `SCR_EL3.HCE`：如果使用 EL2，必须为 1

**检查代码**：
```c
// lib/el3_runtime/aarch64/context_mgmt.c:125
scr_el3 = read_ctx_reg(state, CTX_SCR_EL3);

// 确保 SMD 位为 0
scr_el3 &= ~SCR_SMD_BIT;  // 清除 SMD 位，启用 SMC

write_ctx_reg(state, CTX_SCR_EL3, scr_el3);
```

### 原因 5：TSP 执行环境配置错误

**检查项**：
1. **MMU 配置**：TSP 的 MMU 必须正确配置
2. **栈指针**：TSP 的栈必须正确设置
3. **异常屏蔽**：DAIF 位可能屏蔽了异常

**检查代码**：
```c
// 在 TSP 入口点检查
uint64_t sctlr_el1 = read_sctlr_el1();
uint64_t daif = read_daif();
uint64_t sp_el1 = read_sp_el1();

INFO("TSP SCTLR_EL1: 0x%lx\n", sctlr_el1);
INFO("TSP DAIF: 0x%lx\n", daif);
INFO("TSP SP_EL1: 0x%lx\n", sp_el1);
```

## 调试步骤

### 步骤 1：添加调试打印

在 TSP 的 `set_smc_args` 和 `restore_args_call_smc` 中添加打印：

```c
// bl32/tsp/tsp_common.c:34
smc_args_t *set_smc_args(...)
{
    INFO("TSP: Setting SMC args: x0=0x%lx, x1=0x%lx\n", arg0, arg1);
    // ... 原有代码 ...
}
```

```assembly
# bl32/tsp/aarch64/tsp_entrypoint.S:31
.macro restore_args_call_smc
    INFO("TSP: About to call SMC, x0=0x%lx\n", x0)  # 需要实现 INFO 宏
    ldp x6, x7, [x0, #SMC_ARG6]
    ldp x4, x5, [x0, #SMC_ARG4]
    ldp x2, x3, [x0, #SMC_ARG2]
    ldp x0, x1, [x0, #SMC_ARG0]
    INFO("TSP: Calling SMC with x0=0x%lx\n", x0)
    smc #0
    INFO("TSP: Returned from SMC\n")  # 如果执行到这里，说明 SMC 没有触发异常
.endm
```

### 步骤 2：检查 EL3 是否收到 SMC

在 EL3 的 SMC 处理函数中添加打印：

```c
// bl31/aarch64/runtime_exceptions.S:sync_handler64
// 或者在 tspd_smc_handler 中添加
INFO("EL3: Received SMC from TSP, x0=0x%lx\n", x0);
```

### 步骤 3：检查 SCR_EL3

```c
// 在进入 TSP 之前（EL3）
uint64_t scr_el3 = read_scr_el3();
INFO("EL3: SCR_EL3 before TSP entry: 0x%lx\n", scr_el3);
INFO("EL3: SCR_EL3.SMD = %d (should be 0)\n", (scr_el3 & SCR_SMD_BIT) != 0);

// 在 TSP 中（S-EL1，无法直接读取 SCR_EL3，但可以检查行为）
```

### 步骤 4：单步调试

如果可能，使用调试器：
1. 在 `smc #0` 指令处设置断点
2. 单步执行，看是否触发异常
3. 检查异常是否被 EL3 捕获

## 常见配置错误

### 错误 1：SCR_EL3.SMD = 1

**原因**：SMC 被禁用

**解决**：检查 `context_mgmt.c` 中设置 `SCR_EL3` 的代码，确保 `SMD = 0`

### 错误 2：异常向量表错误

**原因**：EL3 的异常向量表未正确设置

**解决**：检查 `bl31_entrypoint.S` 中设置 `VBAR_EL3` 的代码

### 错误 3：TSP 异常处理干扰

**原因**：TSP 的异常向量表捕获了 SMC 异常

**解决**：SMC 异常应该自动路由到 EL3，不应该被 S-EL1 处理

### 错误 4：MMU 配置错误

**原因**：TSP 的 MMU 配置导致内存访问错误

**解决**：检查 TSP 的页表配置，确保代码和数据区域正确映射

## 快速检查清单

- [ ] `SCR_EL3.SMD = 0`（SMC 启用）
- [ ] EL3 异常向量表正确设置
- [ ] TSP 异常向量表不会捕获 SMC
- [ ] TSP MMU 正确配置
- [ ] TSP 栈指针正确设置
- [ ] `set_smc_args` 正确设置返回值
- [ ] `restore_args_call_smc` 正确恢复参数
- [ ] `smc #0` 指令被执行

## 调试代码示例

### 在 TSP 中添加调试

```c
// bl32/tsp/tsp_main.c:297
return set_smc_args(func, 0, results[0], results[1], 0, 0, 0, 0);

// 修改为：
INFO("TSP: Returning from smc_handler, func=0x%lx, results[0]=0x%lx\n", 
     func, results[0]);
smc_args_t *args = set_smc_args(func, 0, results[0], results[1], 0, 0, 0, 0);
INFO("TSP: SMC args set, about to call SMC\n");
return args;
```

### 在 TSPD 中添加调试

```c
// services/spd/tspd/tspd_main.c:341
static uintptr_t tspd_smc_handler(uint32_t smc_fid, ...)
{
    INFO("TSPD: Received SMC, fid=0x%x, from %s\n", 
         smc_fid, ns ? "NS" : "S");
    // ... 原有代码 ...
}
```

## 最可能的原因

根据经验，**最可能的原因是 `SCR_EL3.SMD` 被设置为 1**，导致 SMC 被禁用。

**检查方法**：
```bash
# 在编译时添加调试
make PLAT=fvp SPD=tspd DEBUG=1 all

# 运行后查看日志，寻找 SCR_EL3 的值
```

**解决方案**：
检查 `lib/el3_runtime/aarch64/context_mgmt.c` 中设置 `SCR_EL3` 的代码，确保 `SMD` 位为 0。

