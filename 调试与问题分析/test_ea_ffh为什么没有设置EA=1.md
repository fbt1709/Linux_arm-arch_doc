# test_ea_ffh为什么没有设置EA=1

## 问题

**test_ea_ffh.c中没有显式设置SCR_EL3.EA=1，怎么保证SError会路由到EL3？**

## 答案

### EA=1是在BL31初始化时设置的

**SCR_EL3.EA=1不是在测试代码中设置的，而是在BL31（EL3 Firmware）初始化时设置的。**

### 设置位置

#### 位置1：BL31平台初始化代码

```c
// plat/arm/common/arm_bl31_setup.c 或类似文件
void bl31_plat_arch_setup(void)
{
    // ...
    uint64_t scr_el3 = read_scr_el3();
    scr_el3 |= SCR_EA_BIT;  // 设置EA=1
    write_scr_el3(scr_el3);
    // ...
}
```

#### 位置2：平台配置宏

```c
// 如果定义了HANDLE_EA_EL3_FIRST_NS或PLATFORM_TEST_EA_FFH
#if HANDLE_EA_EL3_FIRST_NS
    // BL31初始化时设置SCR_EL3.EA=1
#endif
```

#### 位置3：测试前提条件

**测试注释说明**：
```c
/*
 * Works in conjunction with PLATFORM_TEST_EA_FFH macro in TF-A.
 */
```

**这意味着**：
- 测试需要TF-A编译时启用`PLATFORM_TEST_EA_FFH`宏
- 这个宏会确保BL31初始化时设置SCR_EL3.EA=1
- 测试代码本身不需要设置EA=1

## 测试前提条件

### 前提1：TF-A编译配置

**需要启用FFH支持**：
```makefile
# 在TF-A编译时启用
HANDLE_EA_EL3_FIRST_NS := 1
# 或
PLATFORM_TEST_EA_FFH := 1
```

### 前提2：BL31初始化设置

**BL31在初始化时会检查配置并设置EA=1**：
```c
// 在BL31初始化代码中
#if HANDLE_EA_EL3_FIRST_NS
    uint64_t scr_el3 = read_scr_el3();
    scr_el3 |= SCR_EA_BIT;  // 设置EA=1
    write_scr_el3(scr_el3);
#endif
```

### 前提3：测试运行时验证

**测试假设EA=1已经设置**：
- 测试代码不设置EA=1
- 测试假设BL31已经设置了EA=1
- 如果EA=1没有设置，测试会失败（EA路由到TFTF而不是EL3）

## 为什么测试代码不设置EA=1？

### 原因1：测试的是系统配置，不是设置配置

**测试目的**：
- 验证FFH模式是否正常工作
- 验证EA是否被路由到EL3
- **不是测试如何设置EA=1**

### 原因2：EA=1应该在系统初始化时设置

**设计原则**：
- EA=1是系统级配置，应该在BL31初始化时设置
- 测试代码不应该修改系统级配置
- 测试应该验证配置是否正确工作

### 原因3：测试前提条件

**测试假设**：
- 系统已经配置为FFH模式（EA=1）
- 测试验证这个配置是否正常工作
- 如果配置不正确，测试会失败

## 如何验证EA=1是否设置？

### 方法1：在测试中添加验证代码

```c
test_result_t test_inject_syncEA(void)
{
    int rc;
    
    // 验证EA=1是否设置（通过SMC）
    uint64_t scr_el3 = get_scr_el3_via_smc();
    if ((scr_el3 & SCR_EA_BIT) == 0) {
        tftf_testcase_printf("错误: SCR_EL3.EA = 0，FFH模式未启用\n");
        tftf_testcase_printf("测试需要FFH模式（EA=1）\n");
        return TEST_RESULT_FAIL;
    }
    
    // 继续测试...
    rc = mmap_add_dynamic_region(TEST_ADDRESS, TEST_ADDRESS, PAGE_SIZE,
                                  MT_DEVICE | MT_RO | MT_NS);
    // ...
}
```

### 方法2：检查BL31初始化日志

**在BL31初始化时添加日志**：
```c
// 在BL31初始化代码中
#if HANDLE_EA_EL3_FIRST_NS
    uint64_t scr_el3 = read_scr_el3();
    scr_el3 |= SCR_EA_BIT;
    write_scr_el3(scr_el3);
    INFO("SCR_EL3.EA = 1 set (FFH mode enabled)\n");
#endif
```

### 方法3：检查编译配置

**确认TF-A编译时启用了FFH支持**：
```bash
# 检查编译配置
grep -r "HANDLE_EA_EL3_FIRST_NS\|PLATFORM_TEST_EA_FFH" build/
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
EL3异常处理函数处理
    ↓
EL3跳过异常指令，返回到下一条指令
    ↓
测试继续执行
    ↓
测试成功
```

## 总结

**为什么测试代码不设置EA=1？**

1. **EA=1在BL31初始化时设置**
   - 不是测试代码的职责
   - 应该在系统初始化时配置

2. **测试验证配置，不设置配置**
   - 测试目的是验证FFH模式是否工作
   - 不是测试如何设置FFH模式

3. **测试前提条件**
   - 需要TF-A编译时启用FFH支持
   - 需要BL31初始化时设置EA=1
   - 测试假设这些前提条件已满足

**如何确保EA=1已设置？**

1. **检查TF-A编译配置**：确认启用了FFH支持
2. **检查BL31初始化日志**：确认EA=1已设置
3. **在测试中添加验证**：通过SMC检查SCR_EL3.EA的值

**如果EA=1没有设置**：
- 测试会失败（EA路由到TFTF而不是EL3）
- 可以通过测试失败来发现配置问题

