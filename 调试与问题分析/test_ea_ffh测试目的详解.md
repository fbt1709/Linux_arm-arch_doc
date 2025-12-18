# test_ea_ffh测试目的详解

## 测试目的

**测试FFH（Firmware First Handling）模式下，来自低异常等级的EA（External Abort）是否被EL3正确捕获和处理**。

## 核心概念

### EA（External Abort）是什么？

**EA（External Abort）**：
- 外部中止异常，由外部总线或内存系统触发的错误
- 包括：
  - **同步EA**：立即触发的错误（如访问无效地址）
  - **异步EA/SError**：延迟触发的错误（如RAS错误）

### FFH模式（Firmware First Handling）

**FFH模式**：
- SCR_EL3.EA = 1
- EA路由到EL3（Firmware）处理
- EL3统一管理所有EA错误

## 测试函数详解

### 测试1：test_inject_syncEA（同步EA）

```c
test_result_t test_inject_syncEA(void)
{
    // 1. 映射一个不存在的内存地址作为Device内存
    mmap_add_dynamic_region(TEST_ADDRESS, TEST_ADDRESS, PAGE_SIZE,
                            MT_DEVICE | MT_RO | MT_NS);
    
    // 2. 读取无效地址，触发同步EA
    rc = mmio_read_32(TEST_ADDRESS);
    
    // 3. 清理映射
    mmap_remove_dynamic_region(TEST_ADDRESS, PAGE_SIZE);
    
    return TEST_RESULT_SUCCESS;
}
```

**测试流程**：
```
TFTF在EL1执行
    ↓
映射不存在的内存地址（TEST_ADDRESS = 0x7FFFF000）
    ↓
读取该地址（mmio_read_32）
    ↓
总线错误（访问不存在的设备）
    ↓
同步EA异常触发
    ↓
硬件路由到EL3（SCR_EL3.EA = 1）
    ↓
EL3处理异常
    ↓
EL3返回到下一条指令（避免重复异常）
    ↓
测试继续执行
```

**关键点**：
- 同步EA会立即触发
- EL3处理异常后，会跳过导致异常的指令
- 测试验证EL3是否正确处理了同步EA

### 测试2：test_inject_serror（异步EA/SError）

```c
test_result_t test_inject_serror(void)
{
    // 1. 映射一个不存在的内存地址作为Device内存
    mmap_add_dynamic_region(TEST_ADDRESS, TEST_ADDRESS, PAGE_SIZE,
                            MT_DEVICE | MT_RW | MT_NS);
    
    // 2. 写入无效地址，触发异步EA/SError
    mmio_write_32(TEST_ADDRESS, 1);
    
    // 3. 清理映射
    mmap_remove_dynamic_region(TEST_ADDRESS, PAGE_SIZE);
    
    return TEST_RESULT_SUCCESS;
}
```

**测试流程**：
```
TFTF在EL1执行
    ↓
映射不存在的内存地址
    ↓
写入该地址（mmio_write_32）
    ↓
总线错误（写入不存在的设备）
    ↓
异步EA/SError异常触发
    ↓
硬件路由到EL3（SCR_EL3.EA = 1）
    ↓
EL3处理异常
    ↓
EL3返回到下一条指令
    ↓
测试继续执行
```

**关键点**：
- 异步EA/SError可能延迟触发
- EL3处理异常后，会跳过导致异常的指令
- 测试验证EL3是否正确处理了异步EA

## 测试机制

### 1. 内存映射机制

**映射不存在的内存**：
```c
mmap_add_dynamic_region(TEST_ADDRESS, TEST_ADDRESS, PAGE_SIZE,
                        MT_DEVICE | MT_RO | MT_NS);
```

**目的**：
- 创建一个虚拟的内存映射
- 映射到不存在的物理地址
- 访问时会触发总线错误

**内存属性**：
- `MT_DEVICE`：设备内存类型
- `MT_RO`：只读（test_inject_syncEA）
- `MT_RW`：读写（test_inject_serror）
- `MT_NS`：非安全内存

### 2. 错误触发机制

**读取无效地址**（同步EA）：
```c
rc = mmio_read_32(TEST_ADDRESS);
```

**写入无效地址**（异步EA/SError）：
```c
mmio_write_32(TEST_ADDRESS, 1);
```

**触发条件**：
- 访问不存在的物理地址
- 总线返回错误
- 触发EA异常

### 3. EL3处理机制

**EL3异常处理**：
```
EA异常触发
    ↓
硬件路由到EL3（SCR_EL3.EA = 1）
    ↓
EL3异常处理函数
    ↓
处理异常（记录错误、决定恢复或panic）
    ↓
调整ELR，跳过导致异常的指令
    ↓
返回到下一条指令
```

**关键点**：
- EL3会跳过导致异常的指令
- 避免重复触发异常
- 测试可以继续执行

## 与你的问题的关系

### 对比：test_ea_ffh vs test_uncontainable

| 特性 | test_ea_ffh | test_uncontainable |
|------|-------------|-------------------|
| **目的** | 测试FFH模式下的EA处理 | 测试Uncontainable错误 |
| **路由模式** | FFH模式（SCR_EL3.EA = 1） | 可能KFH或特殊行为 |
| **错误类型** | 同步EA/异步EA | Uncontainable RAS错误 |
| **错误来源** | 访问无效地址 | RAS错误注入 |
| **处理位置** | EL3 | 可能在TFTF（EL1） |

### 关键区别

**test_ea_ffh**：
- ✅ **明确测试FFH模式**（SCR_EL3.EA = 1）
- ✅ **EA明确路由到EL3**
- ✅ **验证EL3正确处理EA**

**test_uncontainable**：
- ❓ **可能不是FFH模式**
- ❓ **可能路由到TFTF（EL1）**
- ❓ **可能是KFH模式或Uncontainable的特殊行为**

## 测试验证点

### 验证1：EA是否路由到EL3

**如果测试通过**：
- ✅ EA被EL3捕获
- ✅ EL3正确处理了异常
- ✅ 测试可以继续执行（EL3跳过了异常指令）

**如果测试失败**：
- ❌ EA可能路由到TFTF（EL1）
- ❌ 或者EL3处理异常失败
- ❌ 导致测试panic或失败

### 验证2：EL3是否正确跳过异常指令

**关键机制**：
- EL3处理异常后，会调整ELR
- 跳过导致异常的指令
- 返回到下一条指令

**如果EL3正确跳过**：
- ✅ 测试可以继续执行
- ✅ 不会重复触发异常
- ✅ 测试成功完成

## 测试前提条件

### 1. FFH模式必须启用

**需要配置**：
- SCR_EL3.EA = 1
- TF-A编译时启用FFH支持
- PLATFORM_TEST_EA_FFH宏必须定义

### 2. EL3异常处理必须正确实现

**需要实现**：
- EL3的EA处理函数
- 异常恢复机制
- ELR调整机制

## 总结

**test_ea_ffh的测试目的**：

1. ✅ **验证FFH模式下的EA处理**
   - 验证SCR_EL3.EA = 1时，EA路由到EL3

2. ✅ **验证同步EA处理**
   - 验证EL3正确处理同步EA（Data Abort）

3. ✅ **验证异步EA/SError处理**
   - 验证EL3正确处理异步EA/SError

4. ✅ **验证异常恢复机制**
   - 验证EL3正确跳过异常指令
   - 验证测试可以继续执行

**关键场景**：
- 通过访问无效地址触发EA
- 验证EA被EL3捕获和处理
- 验证EL3正确恢复执行

**与你的问题的关系**：
- 这个测试明确使用FFH模式（SCR_EL3.EA = 1）
- 而test_uncontainable可能使用KFH模式或特殊行为
- 这解释了为什么test_ea_ffh路由到EL3，而test_uncontainable可能路由到TFTF

