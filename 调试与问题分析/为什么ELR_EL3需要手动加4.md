# 为什么ELR_EL3需要手动加4？

## 问题

**用户疑问**：ELR_EL3是返回地址，硬件在跳转到异常处理程序时，不是应该自动保存"下一条指令"的地址吗？为什么这里还要手动加4？

## ARM架构规范

### ELR_EL3的保存规则

**关键点**：ELR_EL3保存的地址**取决于异常类型**：

| 异常类型 | ELR保存的地址 | 说明 |
|---------|-------------|------|
| **同步异常**（Data Abort, Instruction Abort） | **导致异常的指令的地址** | 不是下一条指令！ |
| **异步异常**（IRQ, FIQ, SError） | **下一条指令的地址** | 硬件自动+4 |

### 同步异常的特殊行为

**对于同步异常（如Data Abort）**：
```
指令序列：
    ...
    ldr x0, [x1]      ← 这条指令导致Data Abort
    add x2, x2, #1    ← 下一条指令
    ...

异常发生时：
    ELR_EL3 = ldr指令的地址  ← 保存的是导致异常的指令地址！
    （不是add指令的地址）
```

**为什么这样设计？**
- 允许异常处理程序**重试**导致异常的指令
- 如果问题已解决，可以重新执行这条指令
- 如果决定跳过，需要手动调整ELR

### 异步异常的行为

**对于异步异常（如IRQ）**：
```
指令序列：
    ...
    add x0, x0, #1    ← 正常执行
    add x1, x1, #1    ← 正常执行
    ldr x2, [x3]      ← 正常执行
    add x4, x4, #1    ← 下一条指令（IRQ可能在这里发生）
    ...

IRQ发生时：
    ELR_EL3 = add x4指令的地址  ← 保存的是下一条指令地址！
    （硬件自动+4）
```

**为什么异步异常自动+4？**
- 异步异常可以在指令之间发生
- 硬件不知道具体在哪条指令发生
- 保存下一条指令地址，返回后继续执行

## 代码分析

### fvp_ea.c中的处理

```c
if (ea_reason == ERROR_EA_SYNC) {
    INFO("Handled sync EA from lower EL at address 0x%lx\n", fault_address);
    /* To avoid continuous faults, forward return address */
    elr_el3 = read_ctx_reg(el3_ctx, CTX_ELR_EL3);
    elr_el3 += 4;  // ← 为什么需要手动加4？
    write_ctx_reg(el3_ctx, CTX_ELR_EL3, elr_el3);
    return;
}
```

### 为什么需要手动加4？

**原因**：同步EA（Data Abort）的ELR保存的是**导致异常的指令地址**，不是下一条指令地址。

**如果不加4**：
```
TFTF执行：
    mmio_read_32(TEST_ADDRESS)  ← 导致Data Abort
    mmap_remove_dynamic_region()  ← 下一条指令

异常发生时：
    ELR_EL3 = mmio_read_32的地址  ← 保存的是导致异常的指令地址

EL3处理异常后：
    eret返回到mmio_read_32  ← 重新执行导致异常的指令
    ↓
    再次触发Data Abort
    ↓
    死循环 ❌
```

**如果加4**：
```
TFTF执行：
    mmio_read_32(TEST_ADDRESS)  ← 导致Data Abort
    mmap_remove_dynamic_region()  ← 下一条指令

异常发生时：
    ELR_EL3 = mmio_read_32的地址  ← 保存的是导致异常的指令地址

EL3处理异常：
    ELR_EL3 += 4  ← 手动调整到下一条指令

EL3处理异常后：
    eret返回到mmap_remove_dynamic_region  ← 跳过导致异常的指令
    ↓
    继续执行 ✅
```

## ARM架构文档参考

### ARM Architecture Reference Manual

**对于同步异常（Synchronous Exceptions）**：
- ELR保存的是**导致异常的指令的地址**
- 这是为了允许异常处理程序重试指令

**对于异步异常（Asynchronous Exceptions）**：
- ELR保存的是**下一条指令的地址**
- 硬件自动计算并保存

### 具体到Data Abort

**Data Abort是同步异常**：
```
指令：ldr x0, [x1]  ← 访问非法地址，触发Data Abort

硬件行为：
    1. 检测到Data Abort
    2. 保存当前PC到ELR_EL3（ldr指令的地址）
    3. 跳转到异常处理程序

注意：ELR保存的是ldr指令的地址，不是下一条指令的地址！
```

## 对比：同步异常 vs 异步异常

### 同步异常（Data Abort）

```
代码：
    ldr x0, [x1]      ← 导致异常
    add x2, x2, #1    ← 下一条指令

异常发生时：
    ELR_EL3 = ldr指令的地址  ← 硬件保存的是导致异常的指令地址
    （需要手动+4才能跳过）
```

### 异步异常（IRQ）

```
代码：
    add x0, x0, #1    ← 正常执行
    add x1, x1, #1    ← 正常执行
    ldr x2, [x3]      ← 正常执行
    add x4, x4, #1    ← 下一条指令

IRQ发生时：
    ELR_EL3 = add x4指令的地址  ← 硬件自动保存下一条指令地址
    （不需要手动调整）
```

## 为什么同步异常不自动+4？

### 设计原因

**允许重试机制**：
```
场景：访问一个可能暂时不可用的设备

代码：
    ldr x0, [DEVICE_ADDR]  ← 设备暂时不可用，触发Data Abort

异常处理程序：
    1. 检查设备状态
    2. 如果设备现在可用，重试指令
    3. 如果设备不可用，跳过指令

如果硬件自动+4：
    - 无法重试指令
    - 失去了重试机制

如果硬件保存当前指令地址：
    - 可以重试指令（直接返回）
    - 可以跳过指令（手动+4）
    - 灵活性更高 ✅
```

## 总结

### 关键点

1. **同步异常**（Data Abort）：
   - ELR保存的是**导致异常的指令地址**
   - **不是**下一条指令地址
   - 需要手动+4才能跳过指令

2. **异步异常**（IRQ, SError）：
   - ELR保存的是**下一条指令地址**
   - 硬件自动+4
   - 不需要手动调整

3. **为什么这样设计**：
   - 同步异常允许重试机制
   - 异步异常不需要重试（已经执行完成）

### 代码中的处理

```c
if (ea_reason == ERROR_EA_SYNC) {
    // 同步EA：ELR保存的是导致异常的指令地址
    // 需要手动+4跳过指令
    elr_el3 += 4;
} else if (ea_reason == ERROR_EA_ASYNC) {
    // 异步EA：ELR已经保存了下一条指令地址
    // 不需要手动调整
    return;
}
```

**结论**：硬件**不会**自动为同步异常+4，所以需要手动调整ELR才能跳过导致异常的指令。

