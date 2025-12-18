# HCR_EL2配置和EL2切换分析

## 关键发现

### 1. HCR_EL2的初始化

在`lib/el3_runtime/aarch64/context_mgmt.c`中：

```c
// 第1012行：init_nonsecure_el2_unused()
u_register_t hcr_el2 = HCR_RESET_VAL;  // HCR_RESET_VAL = 0x0

// 第1023行
write_hcr_el2(hcr_el2);
```

**HCR_RESET_VAL = 0x0**，这意味着：
- **HCR_EL2.AMO (bit 5) = 0**（默认值）
- 如果AMO=0，SError应该根据SCR_EL3.EA路由

### 2. 但SPSR_EL3显示EL2h

**问题**：如果HCR_EL2.AMO = 0，为什么SError会路由到EL2？

## 可能的原因

### 原因1：EL2被实际使用（SCR_HCE_BIT = 1）

在`cm_prepare_el3_exit()`中（第1164行）：

```c
if ((scr_el3 & SCR_HCE_BIT) != 0U) {
    // EL2被使用，初始化EL2相关寄存器
    // ...
}
```

**如果SCR_EL3.HCE = 1**：
- EL2被启用
- 系统运行在EL2，而不是EL1
- TFTF可能实际运行在EL2，而不是EL1

### 原因2：EL2上下文被恢复

在`cm_prepare_el3_exit()`中，如果EL2被使用：
- EL2的系统寄存器会被恢复
- HCR_EL2可能被设置为非零值
- 可能包含AMO=1的配置

### 原因3：平台特定配置

某些平台可能在初始化时设置了HCR_EL2.AMO = 1。

## 验证方法

### 1. 检查SCR_EL3.HCE

```c
// 在EL3退出前检查
uint64_t scr_el3 = read_scr_el3();
if (scr_el3 & SCR_HCE_BIT) {
    INFO("EL2被启用（HCE=1），系统可能运行在EL2\n");
} else {
    INFO("EL2未启用（HCE=0），系统运行在EL1\n");
}
```

### 2. 检查当前异常等级

在TFTF中检查：

```c
unsigned int get_current_el(void)
{
    unsigned int el;
    asm volatile("mrs %0, CurrentEL" : "=r" (el));
    return (el >> 2) & 0x3;
}

// 如果返回2，说明TFTF运行在EL2
// 如果返回1，说明TFTF运行在EL1
```

### 3. 检查HCR_EL2.AMO

在EL3异常处理函数中：

```c
uint64_t hcr_el2 = read_hcr_el2();
INFO("HCR_EL2 = 0x%016llx\n", hcr_el2);
INFO("AMO (bit 5) = %d\n", (hcr_el2 >> 5) & 1);
INFO("HCE (bit 31) = %d\n", (hcr_el2 >> 31) & 1);
```

## 关键代码位置

### 1. HCR_EL2初始化

**文件**：`lib/el3_runtime/aarch64/context_mgmt.c`
- **第1012行**：`init_nonsecure_el2_unused()` - 初始化未使用的EL2
- **第1023行**：`write_hcr_el2(hcr_el2)` - 写入HCR_EL2

### 2. EL3退出准备

**文件**：`lib/el3_runtime/aarch64/context_mgmt.c`
- **第1122行**：`cm_prepare_el3_exit()` - 准备退出到低异常等级
- **第1164行**：检查`SCR_HCE_BIT`，如果设置则初始化EL2

### 3. EL2上下文恢复

如果EL2被使用，在退出到非安全世界时：
- EL2的系统寄存器会被恢复
- 包括HCR_EL2、SCTLR_EL2等

## 总结

**TFTF并没有主动切换到EL2**，而是：

1. **如果SCR_EL3.HCE = 1**：
   - EL2被启用
   - 系统实际运行在EL2
   - TFTF运行在EL2，不是EL1

2. **如果SCR_EL3.HCE = 0但HCR_EL2.AMO = 1**：
   - EL2未启用，但HCR_EL2.AMO被设置
   - SError路由到EL2（即使EL2未启用，异常也会路由到EL2处理）

3. **硬件自动切换**：
   - 当SError路由到EL2时，硬件自动切换到EL2
   - 保存EL2的状态到SPSR_EL3
   - 这不是软件主动切换的

**需要检查**：
- SCR_EL3.HCE的值（EL2是否启用）
- TFTF实际运行在哪个异常等级
- HCR_EL2.AMO的值

