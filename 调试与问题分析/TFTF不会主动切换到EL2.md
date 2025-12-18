# TFTF不会主动切换到EL2

## 关键发现

### TFTF入口点代码分析

在`tftf/framework/aarch64/entrypoint.S`的`arch_init()`函数中：

```assembly
func arch_init
	mrs	x0, CurrentEL
	cmp	x0, #(MODE_EL1 << MODE_EL_SHIFT)
	b.eq	el1_setup

el2_setup:
	/* 如果当前在EL2，设置EL2的异常向量和SCTLR */
	adr	x0, tftf_vector
	msr	vbar_el2, x0
	mov_imm	x0, (SCTLR_EL2_RES1 | SCTLR_I_BIT | SCTLR_A_BIT | SCTLR_SA_BIT)
	msr	sctlr_el2, x0
	isb
	ret

el1_setup:
	/* 如果当前在EL1，设置EL1的异常向量和SCTLR */
	adr	x0, tftf_vector
	msr	vbar_el1, x0
	mov_imm	x0, (SCTLR_EL1_RES1 | SCTLR_I_BIT | SCTLR_A_BIT | SCTLR_SA_BIT)
	msr	sctlr_el1, x0
	isb
	ret
endfunc arch_init
```

## 关键理解

### TFTF不会主动切换EL

**TFTF只是检测当前EL，然后做相应的初始化**：

1. **检测当前EL**：读取`CurrentEL`寄存器
2. **如果在EL1**：初始化EL1的寄存器（VBAR_EL1, SCTLR_EL1）
3. **如果在EL2**：初始化EL2的寄存器（VBAR_EL2, SCTLR_EL2）

**TFTF不会主动切换到EL2**，它只是：
- 检测当前在哪个EL
- 根据当前EL做相应的初始化
- 在当前的EL继续运行

## 为什么SPSR_EL3显示EL2h？

### 不是TFTF切换的，而是系统配置的

**可能的原因**：

1. **BL31退出时配置了EL2**
   - 如果`SCR_EL3.HCE = 1`，EL2被启用
   - BL31退出到非安全世界时，系统运行在EL2
   - TFTF在EL2启动，检测到EL2，就初始化EL2的寄存器

2. **平台配置启用了EL2**
   - 某些平台可能默认启用EL2（虚拟化）
   - TFTF启动时已经在EL2

3. **SError路由到EL2**
   - 当SError发生时，如果HCR_EL2.AMO=1，路由到EL2
   - 硬件自动切换到EL2处理异常
   - SPSR_EL3保存EL2的状态

## 验证方法

### 1. 检查TFTF启动时的EL

在TFTF入口点添加打印：

```c
// 在tftf_entrypoint()或arch_init()中
unsigned int get_current_el(void)
{
    unsigned int el;
    asm volatile("mrs %0, CurrentEL" : "=r" (el));
    return (el >> 2) & 0x3;
}

void check_tftf_startup_el(void)
{
    unsigned int el = get_current_el();
    tftf_testcase_printf("TFTF启动时的异常等级: EL%d\n", el);
    
    if (el == 2) {
        tftf_testcase_printf("-> TFTF在EL2启动（系统配置了EL2）\n");
    } else if (el == 1) {
        tftf_testcase_printf("-> TFTF在EL1启动（正常情况）\n");
    }
}
```

### 2. 检查SCR_EL3.HCE

在BL31退出前检查：

```c
// 在cm_prepare_el3_exit()中
uint64_t scr_el3 = read_scr_el3();
if (scr_el3 & SCR_HCE_BIT) {
    INFO("SCR_EL3.HCE = 1，EL2被启用\n");
    INFO("退出后系统将运行在EL2\n");
} else {
    INFO("SCR_EL3.HCE = 0，EL2未启用\n");
    INFO("退出后系统将运行在EL1\n");
}
```

## 总结

**TFTF不会主动切换到EL2**：

1. ✅ **TFTF只是检测当前EL**，然后做相应的初始化
2. ✅ **如果系统已经在EL2**，TFTF就在EL2运行
3. ✅ **如果系统在EL1**，TFTF就在EL1运行
4. ✅ **TFTF没有切换EL的代码**

**SPSR_EL3显示EL2h的原因**：
- 可能是系统配置启用了EL2（SCR_EL3.HCE=1）
- 或者SError路由到EL2时，硬件自动切换到EL2
- 不是TFTF代码主动切换的

**需要检查**：
- TFTF启动时的CurrentEL值
- SCR_EL3.HCE的值
- BL31退出时的配置

