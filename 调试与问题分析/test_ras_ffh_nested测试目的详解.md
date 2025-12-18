# test_ras_ffh_nested测试目的详解

## 测试目的

**测试EL3中SError的嵌套异常处理机制**，特别是在SMC异常处理过程中处理挂起的异步EA（SError）。

## 核心概念

### 什么是嵌套异常？

**嵌套异常（Nested Exception）**：
- 在处理一个异常的过程中，又发生了另一个异常
- 需要先处理新异常，然后继续处理原异常

### 测试场景

**具体场景**：
```
1. TFTF在EL1执行
2. TFTF调用SMC（进入EL3）
3. 在EL3处理SMC时，同步错误（ESB指令）
4. 发现挂起的SError（异步EA）
5. EL3先处理SError（嵌套异常）
6. 然后继续处理原来的SMC请求
```

## 测试流程详解

### 步骤1：注册SDEI事件处理器

```c
ret = sdei_event_register(event_id, serror_sdei_event_handler, 0,
                SDEI_REGF_RM_PE, read_mpidr_el1());
```

**目的**：
- 注册SDEI事件处理器，用于接收SError通知
- SDEI（Software Delegated Exception Interface）是ARM定义的软件委托异常接口
- 当EL3处理SError后，可以通过SDEI通知TFTF

### 步骤2：使能SDEI事件

```c
ret = sdei_event_enable(event_id);
ret = sdei_pe_unmask();
```

**目的**：
- 使能SDEI事件，允许接收通知
- 取消屏蔽PE（Processing Element），允许处理SDEI事件

### 步骤3：第一次SMC调用（设置路由）

```c
args.fid = SMCCC_VERSION;
smc_ret = tftf_smc(&args);
expected_ver = smc_ret.ret0;
```

**目的**：
- 调用SMC获取版本号，用于后续比较
- **关键**：根据注释，这个SMC调用会**改变SCR_EL3.EA=0**，使SError路由到TFTF
- 这样SError会被挂起，而不是立即处理

### 步骤4：禁用SError（PSTATE.A = 1）

```c
disable_serror();
```

**目的**：
- 禁用SError异常（设置PSTATE.A = 1）
- 这样SError不会立即触发，而是被挂起
- 挂起的SError会在下次同步错误时被检测到

### 步骤5：注入RAS错误

```c
inject_unrecoverable_ras_error();
waitms(50);
```

**目的**：
- 注入不可恢复的RAS错误
- 等待错误触发
- 由于SError被禁用，错误会被挂起（ISR_EL1 = 0x100）

### 步骤6：第二次SMC调用（触发嵌套异常处理）

```c
smc_ret = tftf_smc(&args);
```

**关键流程**：
```
TFTF调用SMC
    ↓
进入EL3，sync_exception_vector入口
    ↓
在SMC处理过程中，执行ESB指令同步错误
    ↓
检测到挂起的SError（ISR_EL1.A = 1）
    ↓
由于FFH模式（SCR_EL3.EA = 1），调用handle_pending_async_ea
    ↓
EL3处理SError（嵌套异常）
    ↓
通过SDEI通知TFTF
    ↓
SError处理完成，返回到SMC处理
    ↓
继续处理原来的SMC请求
    ↓
返回到TFTF
```

### 步骤7：验证

```c
// 验证1：SDEI事件是否收到
if (sdei_event_received == false) {
    return TEST_RESULT_FAIL;
}

// 验证2：SMC请求是否成功
if (smc_ret.ret0 != expected_ver) {
    return TEST_RESULT_FAIL;
}
```

**验证点**：
1. ✅ SDEI事件是否收到（SError是否被处理）
2. ✅ SMC请求是否成功（嵌套异常处理是否正确）

## 关键机制

### 1. 错误同步机制（ESB指令）

**ESB（Error Synchronization Barrier）指令**：
- 同步挂起的异步错误
- 在异常处理入口处执行
- 检测是否有挂起的SError

### 2. 嵌套异常处理

**EL3的嵌套异常处理流程**：
```c
// bl31/aarch64/runtime_exceptions.S

.macro sync_and_handle_pending_serror
    synchronize_errors            // ESB指令，同步错误
    mrs x30, ISR_EL1              // 读取中断状态寄存器
    tbz x30, #ISR_A_SHIFT, 2f     // 检查是否有挂起的异步EA
#if FFH_SUPPORT
    mrs x30, scr_el3
    tst x30, #SCR_EA_BIT          // 检查SCR_EL3.EA位
    b.eq 1f
    bl  handle_pending_async_ea   // 如果EA路由到EL3，处理挂起的EA
    b   2f
#endif
1:
    // EA不路由到EL3，反射回低特权级
    bl  reflect_pending_async_ea_to_lower_el
2:
.endm
```

### 3. SDEI通知机制

**SDEI（Software Delegated Exception Interface）**：
- ARM定义的软件委托异常接口
- 允许EL3通知低异常等级（EL1/EL2）异常已处理
- 用于FFH模式下的错误通知

## 测试目的总结

### 主要目的

1. **验证嵌套异常处理**
   - 在SMC处理过程中处理挂起的SError
   - 确保嵌套异常处理正确

2. **验证FFH模式**
   - 验证Firmware First Handling模式下的错误处理
   - 验证EL3正确处理SError

3. **验证SDEI通知**
   - 验证EL3通过SDEI通知TFTF错误已处理
   - 验证SDEI事件机制正常工作

4. **验证异常恢复**
   - 验证处理嵌套异常后，能正确恢复原异常处理
   - 验证SMC请求能正常完成

### 测试的关键点

**关键点1：错误挂起机制**
- 通过禁用SError（PSTATE.A = 1），使错误挂起
- 挂起的错误在下次同步错误时被检测到

**关键点2：嵌套异常处理**
- 在SMC处理过程中处理挂起的SError
- 先处理嵌套异常，然后继续处理原异常

**关键点3：FFH模式验证**
- 验证SCR_EL3.EA = 1时的FFH模式
- 验证EL3正确处理SError

**关键点4：SDEI通知验证**
- 验证EL3通过SDEI通知TFTF
- 验证TFTF能收到SDEI事件

## 与你的问题的关系

### 对比：test_uncontainable vs test_ras_ffh_nested

| 特性 | test_uncontainable | test_ras_ffh_nested |
|------|-------------------|---------------------|
| **目的** | 测试Uncontainable错误 | 测试嵌套异常处理 |
| **路由模式** | 可能KFH或特殊行为 | FFH模式（SCR_EL3.EA = 1） |
| **处理位置** | 可能在TFTF（EL1） | EL3 |
| **错误类型** | Uncontainable | Unrecoverable |
| **通知机制** | 无 | SDEI |

### 关键区别

**test_uncontainable**：
- 可能路由到TFTF（EL1）
- 没有SDEI通知
- 可能是KFH模式或Uncontainable的特殊行为

**test_ras_ffh_nested**：
- 明确路由到EL3（FFH模式）
- 使用SDEI通知TFTF
- 测试嵌套异常处理机制

## 总结

**test_ras_ffh_nested的测试目的**：

1. ✅ **验证EL3中的嵌套异常处理机制**
2. ✅ **验证FFH模式下的SError处理**
3. ✅ **验证SDEI通知机制**
4. ✅ **验证异常恢复机制**

**关键场景**：
- 在SMC处理过程中处理挂起的SError
- 先处理嵌套异常（SError），然后继续处理原异常（SMC）
- 通过SDEI通知TFTF错误已处理

**与你的问题的关系**：
- 这个测试明确使用FFH模式（SCR_EL3.EA = 1）
- 而test_uncontainable可能使用KFH模式或特殊行为
- 这解释了为什么test_uncontainable路由到TFTF，而这个测试路由到EL3

