# EA=1在哪里设置的

## 关键发现

**SCR_EL3.EA=1是在BL31初始化时设置的，不是在测试代码中设置的。**

## 设置位置

### 位置：context_mgmt.c:270-273

```c
// lib/el3_runtime/aarch64/context_mgmt.c

#if HANDLE_EA_EL3_FIRST_NS
	/* SCR_EL3.EA: Route External Abort and SError Interrupt to EL3. */
	scr_el3 |= SCR_EA_BIT;  // 设置EA=1
#endif
```

**关键代码**：
- 在`setup_el3_context()`函数中
- 当编译时定义了`HANDLE_EA_EL3_FIRST_NS`宏时
- 自动设置`SCR_EL3.EA = 1`

## 设置时机

### BL31初始化流程

```
BL31启动
    ↓
bl31_main()
    ↓
平台初始化
    ↓
setup_el3_context()  ← 在这里设置SCR_EL3.EA=1
    ↓
准备退出到非安全世界
    ↓
cm_prepare_el3_exit()
    ↓
退出到TFTF（EL1）
    ↓
TFTF运行，SCR_EL3.EA=1已设置
```

## 编译配置

### 需要启用HANDLE_EA_EL3_FIRST_NS

**在平台Makefile中**：
```makefile
# plat/arm/board/fvp/platform.mk
HANDLE_EA_EL3_FIRST_NS := 1
```

**或者通过编译选项**：
```bash
make HANDLE_EA_EL3_FIRST_NS=1
```

### 测试相关配置

**PLATFORM_TEST_EA_FFH宏**：
```makefile
# plat/arm/board/fvp/platform.mk
ifeq (${PLATFORM_TEST_EA_FFH}, 1)
    # 编译fvp_ea.c，提供测试专用的plat_ea_handler
    BL31_SOURCES += plat/arm/board/fvp/aarch64/fvp_ea.c
endif
```

## 为什么测试代码不设置EA=1？

### 原因1：系统级配置，不是测试职责

**EA=1是系统级配置**：
- 应该在BL31初始化时设置
- 测试代码不应该修改系统级配置
- 测试应该验证配置是否正确工作

### 原因2：测试前提条件

**测试假设**：
- 系统已经配置为FFH模式（EA=1）
- 这是测试的前提条件
- 如果EA=1没有设置，测试会失败（EA路由到TFTF）

### 原因3：测试验证配置，不设置配置

**测试目的**：
- 验证FFH模式是否正常工作
- 验证EA是否被路由到EL3
- **不是测试如何设置EA=1**

## 如何确保EA=1已设置？

### 方法1：检查编译配置

```bash
# 检查Makefile或编译选项
grep -r "HANDLE_EA_EL3_FIRST_NS" plat/arm/board/fvp/
```

### 方法2：检查BL31初始化日志

**在BL31初始化时添加日志**：
```c
// lib/el3_runtime/aarch64/context_mgmt.c
#if HANDLE_EA_EL3_FIRST_NS
	scr_el3 |= SCR_EA_BIT;
	INFO("SCR_EL3.EA = 1 set (FFH mode enabled)\n");  // 添加这行
#endif
```

### 方法3：在测试中添加验证

```c
test_result_t test_inject_syncEA(void)
{
    // 验证EA=1是否设置（通过SMC）
    uint64_t scr_el3 = get_scr_el3_via_smc();
    if ((scr_el3 & SCR_EA_BIT) == 0) {
        tftf_testcase_printf("错误: SCR_EL3.EA = 0，FFH模式未启用\n");
        tftf_testcase_printf("需要设置HANDLE_EA_EL3_FIRST_NS=1\n");
        return TEST_RESULT_FAIL;
    }
    
    // 继续测试...
}
```

## 完整的设置流程

### 步骤1：编译时配置

```makefile
# 在平台Makefile中
HANDLE_EA_EL3_FIRST_NS := 1
PLATFORM_TEST_EA_FFH := 1
```

### 步骤2：BL31初始化时设置

```c
// lib/el3_runtime/aarch64/context_mgmt.c
void setup_el3_context(...)
{
    uint64_t scr_el3 = ...;
    
#if HANDLE_EA_EL3_FIRST_NS
    scr_el3 |= SCR_EA_BIT;  // 设置EA=1
#endif
    
    write_ctx_reg(state, CTX_SCR_EL3, scr_el3);
}
```

### 步骤3：退出到非安全世界时应用

```c
// lib/el3_runtime/aarch64/context_mgmt.c
void cm_prepare_el3_exit(...)
{
    // 从上下文恢复SCR_EL3（包含EA=1）
    // ...
}
```

### 步骤4：测试验证

```c
// test_ea_ffh.c
test_result_t test_inject_syncEA(void)
{
    // 假设EA=1已设置
    // 触发EA异常
    // 验证是否路由到EL3
}
```

## 如果EA=1没有设置会怎样？

### 情况1：EA=0（KFH模式）

```
访问TEST_ADDRESS
    ↓
触发同步EA异常
    ↓
硬件检查SCR_EL3.EA = 0
    ↓
路由到当前EL（EL1，TFTF）
    ↓
TFTF的异常处理函数处理
    ↓
如果没有注册handler，会panic
    ↓
测试失败
```

### 情况2：EA=1（FFH模式）

```
访问TEST_ADDRESS
    ↓
触发同步EA异常
    ↓
硬件检查SCR_EL3.EA = 1
    ↓
路由到EL3
    ↓
EL3异常处理函数处理（plat_ea_handler）
    ↓
EL3跳过异常指令，返回到下一条指令
    ↓
测试继续执行
    ↓
测试成功
```

## 总结

**EA=1在哪里设置的？**

1. **在BL31初始化时设置**
   - 位置：`lib/el3_runtime/aarch64/context_mgmt.c:270-273`
   - 条件：编译时定义了`HANDLE_EA_EL3_FIRST_NS`宏
   - 代码：`scr_el3 |= SCR_EA_BIT;`

2. **不是测试代码设置的**
   - 测试代码不设置EA=1
   - 测试假设EA=1已经设置
   - 测试验证EA=1是否正确工作

3. **测试前提条件**
   - 需要编译时启用`HANDLE_EA_EL3_FIRST_NS=1`
   - 需要BL31初始化时设置EA=1
   - 如果EA=1没有设置，测试会失败

**如何确保EA=1已设置？**

1. **检查编译配置**：确认`HANDLE_EA_EL3_FIRST_NS=1`
2. **检查BL31初始化日志**：确认EA=1已设置
3. **在测试中添加验证**：通过SMC检查SCR_EL3.EA的值

