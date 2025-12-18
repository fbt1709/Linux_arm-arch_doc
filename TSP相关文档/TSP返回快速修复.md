# TSP 无法返回 EL3 的快速修复指南

## 问题症状

运行到 TSP 后，执行 `smc #0` 指令无法返回到 EL3，程序卡在 TSP 中。

## 最可能的原因

### 原因 1：SCR_EL3.SMD = 1（SMC 被禁用）⭐ 最可能

**症状**：
- TSP 执行 `smc #0` 后，不会触发异常
- 程序继续执行下一条指令
- EL3 的 SMC 处理函数不会被调用

**检查代码**：
```c
// lib/el3_runtime/aarch64/context_mgmt.c:426
scr_el3 &= ~(SCR_TWE_BIT | SCR_TWI_BIT | SCR_SMD_BIT);
// ↑ 这行代码应该清除 SMD 位，启用 SMC
```

**验证方法**：
在进入 TSP 之前添加调试打印：
```c
// services/spd/tspd/tspd_common.c:71 (tspd_synchronous_sp_entry)
uint64_t scr_el3 = read_scr_el3();
INFO("TSPD: SCR_EL3 before TSP entry: 0x%lx\n", scr_el3);
INFO("TSPD: SCR_EL3.SMD = %d (0=enabled, 1=disabled)\n", 
     (scr_el3 & SCR_SMD_BIT) != 0);
```

**如果 SMD = 1**，检查：
1. `context_mgmt.c:426` 是否被执行
2. 是否有其他地方设置了 `SCR_SMD_BIT`

### 原因 2：异常向量表未正确设置

**检查**：
```c
// 在 EL3 入口点检查
uint64_t vbar_el3 = read_vbar_el3();
INFO("EL3 VBAR_EL3: 0x%lx\n", vbar_el3);
// 应该指向 runtime_exceptions 的地址
```

### 原因 3：TSP 的异常向量表捕获了 SMC

**检查**：
```assembly
# bl32/tsp/aarch64/tsp_exceptions.S:104
vector_entry sync_exception_sp_elx
    b plat_panic_handler  # ← 如果 SMC 被这里捕获，会 panic
```

**正常情况**：SMC 异常应该自动路由到 EL3，不应该被 S-EL1 捕获。

## 快速调试步骤

### 步骤 1：添加调试打印（最简单）

在 TSP 的 `restore_args_call_smc` 宏中添加：

```assembly
# bl32/tsp/aarch64/tsp_entrypoint.S:31
.macro restore_args_call_smc
    # 添加调试（需要实现 print 函数）
    # 或者使用简单的循环来验证是否执行到这里
    
    ldp x6, x7, [x0, #SMC_ARG6]
    ldp x4, x5, [x0, #SMC_ARG4]
    ldp x2, x3, [x0, #SMC_ARG2]
    ldp x0, x1, [x0, #SMC_ARG0]
    
    # 在这里添加无限循环来验证
    # 1: b 1b  # 如果卡在这里，说明 SMC 没有触发异常
    
    smc #0
    
    # 如果执行到这里，说明 SMC 没有触发异常（SMD=1）
    # 1: b 1b  # 添加无限循环验证
.endm
```

### 步骤 2：检查 SCR_EL3

在 TSPD 进入 TSP 之前：

```c
// services/spd/tspd/tspd_common.c:71
uint64_t tspd_synchronous_sp_entry(tsp_context_t *tsp_ctx)
{
    // 添加检查
    uint64_t scr_el3 = read_scr_el3();
    if ((scr_el3 & SCR_SMD_BIT) != 0) {
        ERROR("TSPD: SMC is disabled! SCR_EL3.SMD = 1\n");
        ERROR("TSPD: SCR_EL3 = 0x%lx\n", scr_el3);
        panic();
    }
    
    // ... 原有代码 ...
}
```

### 步骤 3：检查 TSP 是否真的调用了 SMC

在 TSP 的 `tsp_smc_handler` 返回后：

```c
// bl32/tsp/tsp_main.c:297
return set_smc_args(func, 0, results[0], results[1], 0, 0, 0, 0);

// 修改为：
INFO("TSP: About to return, func=0x%lx\n", func);
smc_args_t *args = set_smc_args(func, 0, results[0], results[1], 0, 0, 0, 0);
INFO("TSP: SMC args prepared, x0=0x%lx\n", read_sp_arg(args, SMC_ARG0));
// 添加无限循环验证是否执行到这里
// while(1) { asm volatile("nop"); }
return args;
```

## 常见配置错误

### 错误 1：平台特定代码设置了 SCR_SMD_BIT

**检查**：
```bash
# 搜索所有设置 SCR_EL3 的地方
grep -r "SCR_SMD_BIT\|SCR_EL3.*SMD" plat/ lib/
```

### 错误 2：老版本 ATF 的配置不同

**检查**：老版本可能需要在平台代码中显式清除 SMD 位。

### 错误 3：MMU 配置导致内存访问错误

**检查**：TSP 的页表配置，确保代码和数据区域正确映射。

## 修复方案

### 方案 1：确保 SCR_EL3.SMD = 0

在 `context_mgmt.c` 中，确保 `setup_secure_context` 函数清除 SMD 位：

```c
// lib/el3_runtime/aarch64/context_mgmt.c:426
scr_el3 &= ~(SCR_TWE_BIT | SCR_TWI_BIT | SCR_SMD_BIT);
// ↑ 确保这行代码被执行
```

### 方案 2：在平台代码中显式设置

如果平台代码覆盖了 SCR_EL3，需要确保 SMD 位为 0：

```c
// 在平台特定的代码中
uint64_t scr_el3 = read_scr_el3();
scr_el3 &= ~SCR_SMD_BIT;  // 清除 SMD 位
write_scr_el3(scr_el3);
```

### 方案 3：检查编译配置

某些编译选项可能影响 SCR_EL3 的设置：

```bash
# 检查是否有影响 SCR_EL3 的配置
grep -r "SCR_SMD\|SMC.*disable" Makefile make_helpers/
```

## 验证修复

修复后，验证步骤：

1. **编译**：
```bash
make PLAT=fvp SPD=tspd DEBUG=1 all
```

2. **运行并检查日志**：
- 应该看到 "TSP: About to return" 的日志
- 应该看到 "TSPD: Received SMC" 的日志
- 不应该卡在 TSP 中

3. **如果仍然卡住**：
- 检查是否真的执行了 `smc #0`
- 检查 EL3 是否收到了 SMC 异常
- 检查异常向量表是否正确

## 最可能的问题

根据代码分析，**最可能的原因是 `SCR_EL3.SMD` 被设置为 1**。

**快速检查**：
```c
// 在进入 TSP 之前打印
uint64_t scr_el3 = read_scr_el3();
if ((scr_el3 & SCR_SMD_BIT) != 0) {
    ERROR("SMC is disabled!\n");
}
```

**快速修复**：
确保 `context_mgmt.c:426` 中的代码被执行，或者手动清除 SMD 位。

