# ARMv8 RAS错误注入机制详解

## 1. 概述

ARMv8.2引入了**RAS（Reliability, Availability, Serviceability）扩展**，提供了硬件错误检测、报告和恢复机制。其中，**错误注入（Error Injection）**功能允许软件主动注入错误来测试错误处理路径。

## 2. 错误注入相关寄存器

### 2.1 错误记录选择寄存器（ERRSELR_EL1）

**功能**：选择要操作的Error Record编号（0, 1, 2...）

**访问**：EL1及以上特权级

```c
#define ERRSELR_EL1    S3_0_C5_C3_1  // 系统寄存器编码

// 使用示例
msr ERRSELR_EL1, x0  // x0 = 错误记录编号（如0, 1, 2...）
```

**说明**：
- 在访问其他错误记录寄存器（如ERXCTLR_EL1）之前，必须先选择错误记录
- 每个PE可能有多个错误记录，通过索引选择

### 2.2 错误记录控制寄存器（ERXCTLR_EL1）

**功能**：控制选中的Error Record的行为

**访问**：EL1及以上特权级

```c
#define ERXCTLR_EL1    S3_0_C5_C4_1

// 关键位域：
#define ERXCTLR_ED_SHIFT   0
#define ERXCTLR_ED_BIT     (1 << 0)   // Error Detection Enable（错误检测使能）
#define ERXCTLR_UE_BIT     (1 << 4)   // Uncorrected Error（不可纠正错误）
#define ERXCTLR_CE_BIT     (1 << 5)   // Corrected Error（可纠正错误）
```

**位域说明**：
- **ED (bit 0)**：Error Detection Enable，使能错误检测/上报
- **UE (bit 4)**：Uncorrected Error，表示注入不可纠正错误
- **CE (bit 5)**：Corrected Error，表示注入可纠正错误
- **其他位**：控制不同类型错误的报告（如UI, FI等）

### 2.3 可编程故障生成控制寄存器（ERXPFGCTL_EL1）

**功能**：控制可编程故障生成（Programmable Fault Generation）的行为

**访问**：EL1及以上特权级

```c
#define ERXPFGCTL_EL1    S3_0_C5_C4_5

// 关键位域：
#define ERXPFGCTL_UC_BIT      (1 << 1)   // Uncontainable（不可包含）
#define ERXPFGCTL_UEU_BIT     (1 << 2)   // Unrecoverable（不可恢复）
#define ERXPFGCTL_CDEN_BIT    (1 << 31)  // Countdown Enable（倒计时使能）
```

**位域说明**：
- **UC (bit 1)**：Uncontainable，错误类型为不可包含
- **UEU (bit 2)**：Unrecoverable，错误类型为不可恢复
- **CDEN (bit 31)**：Countdown Enable，使能倒计时功能
  - 当设置为1时，开始倒计时
  - 倒计时从ERXPFGCDN_EL1的值开始递减到0
  - 当倒计时到0时，生成错误

### 2.4 可编程故障生成倒计数值寄存器（ERXPFGCDN_EL1）

**功能**：设置倒计数器的初始值（countdown value）

**访问**：EL1及以上特权级

```c
#define ERXPFGCDN_EL1    S3_0_C5_C4_6

// 使用示例
mov x0, #1
msr ERXPFGCDN_EL1, x0  // 设置倒计时初值为1
```

**说明**：
- 当ERXPFGCTL_EL1的CDEN位设置为1时，倒计时开始
- 倒计时从该寄存器设置的值递减到0
- 当倒计时到0时，生成错误事件

## 3. 错误注入流程

### 3.1 基本流程

```
1. 选择错误记录（ERRSELR_EL1）
   ↓
2. 配置错误类型和控制（ERXCTLR_EL1）
   ↓
3. 设置倒计数值（ERXPFGCDN_EL1）
   ↓
4. 启动倒计时（ERXPFGCTL_EL1设置CDEN位）
   ↓
5. 倒计时到0，错误被注入
```

### 3.2 代码实现示例

```assembly
/*
 * 注入不可恢复错误的通用函数
 * 参数：
 *   x0 - 错误记录编号（如0, 1...）
 *   x1 - ERXCTLR_EL1的值（包含UE/CE等位）
 *   x2 - ERXPFGCTL_EL1的值（包含UEU/UC等位）
 */
func inject_ras_error_record
    /* 1. 选择错误记录 */
    msr ERRSELR_EL1, x0
    isb  // 确保选择完成
    
    /* 2. 使能错误检测/上报 */
    orr x1, x1, #ERXCTLR_ED_BIT  // 设置ED位（使能错误检测）
    msr ERXCTLR_EL1, x1
    isb
    
    /* 3. 设置倒计数器初值为1 */
    mov x0, #1
    msr ERXPFGCDN_EL1, x0
    isb
    
    /* 4. 启动倒计时以生成错误 */
    orr x2, x2, #ERXPFGCTL_CDEN_BIT  // 设置CDEN位（启动倒计时）
    msr ERXPFGCTL_EL1, x2
    isb  // 确保倒计时开始
    
    ret
endfunc inject_ras_error_record

/*
 * 注入不可恢复错误（通过错误记录0）
 */
func inject_unrecoverable_ras_error
    /* 选择错误记录0 */
    mov x0, #0
    
    /* 配置为不可纠正错误（UE） */
    mov x1, #ERXCTLR_UE_BIT
    
    /* 配置为不可恢复错误（UEU） */
    mov x2, #ERXPFGCTL_UEU_BIT
    
    /* 调用通用注入函数 */
    b inject_ras_error_record
endfunc inject_unrecoverable_ras_error
```

## 4. ARM-TF中的实现

在ARM Trusted Firmware中，这些寄存器已定义：

```c
// include/arch/aarch64/arch.h

/* 寄存器定义 */
#define ERRSELR_EL1      S3_0_C5_C3_1
#define ERXCTLR_EL1      S3_0_C5_C4_1
#define ERXPFGCTL_EL1    S3_0_C5_C4_5
#define ERXPFGCDN_EL1    S3_0_C5_C4_6

/* 位域定义 */
#define ERXCTLR_ED_SHIFT     0
#define ERXCTLR_ED_BIT       (U(1) << ERXCTLR_ED_SHIFT)
#define ERXCTLR_UE_BIT       (U(1) << 4)

#define ERXPFGCTL_UC_BIT     (U(1) << 1)
#define ERXPFGCTL_UEU_BIT    (U(1) << 2)
#define ERXPFGCTL_CDEN_BIT   (U(1) << 31)
```

**辅助函数**：

```c
// include/arch/aarch64/arch_helpers.h

DEFINE_RENAME_SYSREG_WRITE_FUNC(errselr_el1, ERRSELR_EL1)
DEFINE_RENAME_SYSREG_RW_FUNCS(erxctlr_el1, ERXCTLR_EL1)
// ... 其他寄存器辅助函数
```

## 5. 错误类型说明

### 5.1 不可纠正错误（Uncorrected Error, UE）

- **特点**：硬件无法自动纠正的错误
- **影响**：可能导致数据损坏或系统故障
- **类型**：
  - **UEU (Unrecoverable)**：不可恢复，可能导致系统崩溃
  - **UEO (Restartable)**：可重启，系统可能通过重启恢复
  - **UER (Recoverable)**：可恢复，系统可以自动恢复

### 5.2 可纠正错误（Corrected Error, CE）

- **特点**：硬件可以自动纠正的错误
- **影响**：通常不会导致数据丢失，但可能影响性能
- **处理**：硬件自动纠正，软件记录统计信息

## 6. 使用场景

### 6.1 错误处理路径测试

```c
// 测试场景：验证系统能否正确处理RAS错误
void test_ras_error_handling(void) {
    // 1. 注入错误
    inject_unrecoverable_ras_error();
    
    // 2. 等待错误被检测和处理
    // 3. 验证错误处理路径是否正确执行
    // 4. 检查错误是否被正确记录和报告
}
```

### 6.2 错误恢复机制验证

- 验证系统能否从可恢复错误中恢复
- 测试错误记录和统计功能
- 验证错误通知机制（如SError中断）

## 7. 注意事项

1. **特权级要求**：这些寄存器只能在EL1及以上特权级访问
2. **错误记录数量**：通过ERRIDR_EL1寄存器可以查询系统支持的错误记录数量
3. **同步要求**：在访问这些寄存器之间需要插入`isb`指令确保顺序
4. **测试环境**：错误注入主要用于开发和测试环境，生产环境应谨慎使用
5. **错误处理**：注入错误后，系统会触发相应的错误处理流程（如SError异常）

## 8. 系统寄存器编码

ARMv8系统寄存器使用以下编码格式：

```
ERRSELR_EL1 = S3_0_C5_C3_1
ERXCTLR_EL1 = S3_0_C5_C4_1
ERXPFGCTL_EL1 = S3_0_C5_C4_5
ERXPFGCDN_EL1 = S3_0_C5_C4_6
```

其中`S3_0_C5_C3_1`表示：
- Op0 = 3
- Op1 = 0
- CRn = C5
- CRm = C3
- Op2 = 1

## 9. 总结

ARMv8.2 RAS扩展提供了完整的错误注入机制，允许软件主动注入错误来测试错误处理路径。关键寄存器包括：

- **ERRSELR_EL1**：选择错误记录
- **ERXCTLR_EL1**：配置错误类型和使能检测
- **ERXPFGCTL_EL1**：控制故障生成和倒计时
- **ERXPFGCDN_EL1**：设置倒计时初值

通过这些寄存器的配合使用，可以实现可控的错误注入，用于验证系统的错误处理和恢复能力。

## 10. ATF中的RAS错误处理流程

当RAS错误被注入后，ARM Trusted Firmware（ATF）会按照以下流程处理错误：

### 10.1 错误触发机制

RAS错误可以通过两种方式触发：

1. **SError（异步外部中止）**：
   - 当错误被注入后，硬件会触发SError异常
   - SError被路由到EL3进行处理

2. **RAS中断**：
   - 某些RAS错误可能通过中断信号通知
   - 中断被路由到EL3的RAS优先级处理

### 10.2 ATF处理流程

```
RAS错误发生
    ↓
1. SError异常路由到EL3
   (SCR_EL3.EA = 1 表示SError路由到EL3)
    ↓
2. 异常向量表跳转
   (EL3异常向量表 → serror_aarch64/serror_aarch32)
    ↓
3. 同步并处理挂起的SError
   (sync_and_handle_pending_serror)
    ↓
4. 跳转到 handle_lower_el_async_ea
   (处理来自低特权级的异步EA)
    ↓
5. 调用 delegate_async_ea
   (委托异步EA处理)
    ↓
6. 调用 ea_proceed
   (EA处理流程)
    ↓
7. 调用 plat_ea_handler()
   (平台EA处理函数)
    ↓
4. 调用 ras_ea_handler()
   (RAS框架的EA处理函数)
    ↓
5. 遍历所有错误记录
   (for_each_err_record_info)
    ↓
6. 探测错误记录 (probe)
   (检查错误记录中是否有错误)
    ↓
7. 调用错误处理函数 (handler)
   (处理具体的错误)
    ↓
8. 清除错误状态
   (清除错误记录状态位)
    ↓
9. 可选：通知上层软件
   (如通过SDEI事件通知内核)
```

### 10.3 代码流程详解

#### 步骤1-2：异常向量表跳转

ARMv8异常向量表布局（EL3）：

```
VBAR_EL3基地址：
─────────────────────────────────────────────
0x000: Current EL with SP_EL0
  0x000: sync_exception_sp_el0
  0x080: irq_sp_el0
  0x100: fiq_sp_el0
  0x180: serror_sp_el0
─────────────────────────────────────────────
0x200: Current EL with SP_ELx
  0x200: sync_exception_sp_elx
  0x280: irq_sp_elx
  0x300: fiq_sp_elx
  0x380: serror_sp_elx
─────────────────────────────────────────────
0x400: Lower EL using AArch64
  0x400: sync_exception_aarch64  (SMC等)
  0x480: irq_aarch64
  0x500: fiq_aarch64
  0x580: serror_aarch64          ← RAS SError入口
─────────────────────────────────────────────
0x600: Lower EL using AArch32
  0x600: sync_exception_aarch32
  0x680: irq_aarch32
  0x700: fiq_aarch32
  0x780: serror_aarch32          ← RAS SError入口(AArch32)
─────────────────────────────────────────────
```

**SError异常入口代码**：

```assembly
// bl31/aarch64/runtime_exceptions.S

// 向量表基地址定义
vector_base runtime_exceptions

// Lower EL使用AArch64时的SError入口（偏移0x580）
vector_entry serror_aarch64
#if FFH_SUPPORT
    save_x30                      // 保存LR寄存器
    apply_at_speculative_wa       // 应用推测执行缓解
    sync_and_handle_pending_serror // 同步并处理挂起的SError
    b   handle_lower_el_async_ea   // ← 跳转到异步EA处理函数
#else
    b   report_unhandled_exception // 不支持FFH，报告未处理异常
#endif
end_vector_entry serror_aarch64

// Lower EL使用AArch32时的SError入口（偏移0x780）
vector_entry serror_aarch32
#if FFH_SUPPORT
    save_x30
    apply_at_speculative_wa
    sync_and_handle_pending_serror
    b   handle_lower_el_async_ea   // ← 同样跳转到异步EA处理
#else
    b   report_unhandled_exception
#endif
end_vector_entry serror_aarch32
```

**同步并处理挂起的SError宏**：

```assembly
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

**跳转到handle_lower_el_async_ea**：

```assembly
// bl31/aarch64/ea_delegate.S

/*
 * 处理来自低特权级的异步EA（SError）
 * 该函数假设x30已经被保存
 */
func handle_lower_el_async_ea
    // 保存通用寄存器和系统寄存器
    bl  prepare_el3_entry
    
    // 准备参数调用平台EA处理函数
    mov x0, #ERROR_EA_ASYNC       // EA原因：异步EA
    mrs x1, esr_el3               // 异常综合征（从ESR_EL3读取）
    bl  delegate_async_ea         // ← 委托异步EA处理
    
    // 退出EL3，返回到低特权级
    msr spsel, #MODE_SP_EL0
    b   el3_exit
endfunc handle_lower_el_async_ea

// delegate_async_ea函数
func delegate_async_ea
    // 检查错误类型（Uncontainable等）
    // ...
    
    // 跳转到ea_proceed继续处理
    b   ea_proceed
endfunc delegate_async_ea

// ea_proceed函数（最终调用plat_ea_handler）
func ea_proceed
    // 设置参数
    // x0: EA reason
    // x1: EA syndrome
    // x2: Cookie
    // x3: Context pointer
    // x4: Flags
    
    // 切换到运行时栈
    ldr x5, [sp, #CTX_EL3STATE_OFFSET + CTX_RUNTIME_SP]
    msr spsel, #MODE_SP_EL0
    mov sp, x5
    
    // 调用平台EA处理函数
    bl  plat_ea_handler            // ← 最终调用平台处理函数
    
    // 恢复上下文并返回
    // ...
    ret x29
endfunc ea_proceed
```

**完整跳转路径总结**：

```
1. RAS错误发生（硬件触发SError）
   ↓
2. CPU跳转到VBAR_EL3 + 0x580 (serror_aarch64) 或 0x780 (serror_aarch32)
   ↓
3. serror_aarch64/serror_aarch32入口
   → save_x30
   → apply_at_speculative_wa
   → sync_and_handle_pending_serror（ESB同步错误）
   → b handle_lower_el_async_ea
   ↓
4. handle_lower_el_async_ea
   → prepare_el3_entry（保存上下文）
   → delegate_async_ea
   ↓
5. delegate_async_ea
   → 检查错误类型
   → b ea_proceed
   ↓
6. ea_proceed
   → 设置参数
   → 切换到运行时栈
   → bl plat_ea_handler  ← 调用平台EA处理函数
   ↓
7. plat_ea_handler (plat_default_ea_handler)
   → ras_ea_handler      ← RAS框架处理
   → 错误处理和清除
   → 返回
   ↓
8. 返回到ea_proceed，然后el3_exit返回到低特权级
```

#### 步骤3：平台EA处理函数

```c
// plat/common/aarch64/plat_common.c
void plat_default_ea_handler(unsigned int ea_reason, uint64_t syndrome,
                             void *cookie, void *handle, uint64_t flags)
{
#if ENABLE_FEAT_RAS
    // 调用RAS EA处理函数
    int handled = ras_ea_handler(ea_reason, syndrome, cookie, handle, flags);
    if (handled != 0)
        return;  // 错误已被处理
#endif
    
    // 如果RAS未处理，则报告未处理的错误
    ERROR("Unhandled External Abort received...\n");
    lower_el_panic();
}
```

#### 步骤4：RAS EA处理函数

```c
// lib/extensions/ras/ras_common.c
int ras_ea_handler(unsigned int ea_reason, uint64_t syndrome, void *cookie,
                   void *handle, uint64_t flags)
{
    // 准备错误处理数据结构
    const struct err_handler_data err_data = {
        .ea_reason = ea_reason,
        .syndrome = syndrome,
        .flags = flags,
        .cookie = cookie,
        .handle = handle
    };
    
    // 遍历所有注册的错误记录
    for_each_err_record_info(i, info) {
        // 持续探测直到没有错误
        while (true) {
            // 探测错误记录（检查是否有错误）
            if (info->probe(info, &probe_data) == 0)
                break;  // 没有错误，退出循环
            
            // 有错误，调用对应的处理函数
            ret = info->handler(info, probe_data, &err_data);
            if (ret != 0)
                return ret;  // 处理失败
            
            n_handled++;
        }
    }
    
    return (n_handled != 0U) ? 1 : 0;
}
```

#### 步骤5-6：错误记录探测

错误记录可能通过两种方式访问：

**方式1：系统寄存器访问（System Register）**

```c
// 通过ERRSELR_EL1选择错误记录
ser_sys_select_record(idx);

// 读取ERXSTATUS_EL1检查错误状态
status = read_erxstatus_el1();

// 检查是否有错误（V位和AV位）
if ((status & ERR_STATUS_V_MASK) && (status & ERR_STATUS_AV_MASK)) {
    // 有错误需要处理
    return probe_data;  // 返回错误信息
}
```

**方式2：内存映射访问（Memory-Mapped）**

```c
// 直接读取内存映射的错误状态寄存器
status = mmio_read_64(base_addr + ERR_STATUS(idx));

// 检查错误状态
if ((status & ERR_STATUS_V_MASK) && (status & ERR_STATUS_AV_MASK)) {
    return probe_data;
}
```

#### 步骤7：错误处理函数

每个平台需要实现自己的错误处理函数：

```c
// 示例：CPU RAS错误处理函数
int nrd_ras_cpu_intr_handler(const struct err_record_info *err_rec,
                              int probe_data,
                              const struct err_handler_data *const data)
{
    // 1. 读取错误详细信息
    uint64_t status = read_erxstatus_el1();
    uint64_t addr = read_erxaddr_el1();
    uint64_t misc0 = read_erxmisc0_el1();
    
    INFO("[CPU RAS] Error detected:\n");
    INFO("  Status: 0x%lx\n", status);
    INFO("  Address: 0x%lx\n", addr);
    INFO("  Misc0: 0x%lx\n", misc0);
    
    // 2. 判断错误类型
    if (status & ERR_STATUS_UE_MASK) {
        // 不可纠正错误
        ERROR("Uncorrected Error detected!\n");
        // 可能需要panic或重启
    } else if (status & ERR_STATUS_CE_MASK) {
        // 可纠正错误
        INFO("Corrected Error detected\n");
        // 可以继续运行
    }
    
    // 3. 清除错误状态（写回相同值，write-one-to-clear）
    write_erxstatus_el1(status);
    
    // 4. 可选：通知上层软件（如通过SDEI）
    // sdei_dispatch_event(event_num);
    
    return 0;
}
```

### 10.4 RAS中断处理

除了通过SError异常，RAS错误也可以通过中断方式通知：

```c
// lib/extensions/ras/ras_common.c
static int ras_interrupt_handler(uint32_t intr_raw, uint32_t flags,
                                 void *handle, void *cookie)
{
    // 1. 二分查找找到对应的中断处理函数
    // (中断数组必须是排序的)
    
    // 2. 调用错误记录的处理函数
    selected->err_record->handler(selected->err_record, probe_data, &err_data);
    
    return 0;
}

// RAS初始化时注册中断处理函数
void ras_init(void)
{
    // 注册RAS优先级的中断处理函数
    ehf_register_priority_handler(PLAT_RAS_PRI, ras_interrupt_handler);
}
```

### 10.5 平台配置

平台需要配置RAS相关的数据结构：

```c
// 1. 定义错误记录信息数组
static const struct err_record_info err_records[] = {
    ERR_RECORD_SYSREG_V1(0, 1,              // 错误记录0，1个记录
                         NULL,              // probe函数（NULL表示使用默认）
                         cpu_ras_handler,   // 处理函数
                         0),                // aux_data
    ERR_RECORD_SYSREG_V1(1, 1, 
                         NULL, 
                         sram_ras_handler,
                         0),
};

// 2. 注册错误记录
REGISTER_ERR_RECORD_INFO(err_records);

// 3. 定义RAS中断映射（如果使用中断方式）
static struct ras_interrupt ras_intrs[] = {
    { .intr_number = RAS_INT_ID, 
      .err_record = &err_records[0],
      .cookie = NULL }
};

REGISTER_RAS_INTERRUPTS(ras_intrs);

// 4. 定义RAS优先级
#define PLAT_RAS_PRI  0x10  // RAS错误处理优先级
```

### 10.6 错误处理示例流程

假设注入了一个不可恢复错误（UE）：

```
1. 错误注入代码执行
   → ERXPFGCDN_EL1倒计时到0
   → 硬件生成SError

2. SError异常路由到EL3
   → 跳转到异常向量表
   → handle_lower_el_ea_async

3. 调用plat_ea_handler()
   → 调用ras_ea_handler()

4. ras_ea_handler遍历错误记录
   → 选择错误记录0
   → 调用probe函数检查错误

5. probe函数检查ERXSTATUS_EL1
   → 发现V=1, AV=1, UE=1
   → 返回probe_data（有错误）

6. 调用cpu_ras_handler()
   → 读取错误详细信息
   → 判断为不可恢复错误
   → 打印错误信息
   → 清除错误状态
   → 可选：通知上层软件（SDEI）

7. 返回处理结果
   → 如果错误可恢复，继续运行
   → 如果错误不可恢复，可能需要panic
```

### 10.7 关键数据结构

```c
// 错误处理数据
struct err_handler_data {
    unsigned int ea_reason;    // 错误原因
    uint32_t syndrome;         // ESR/DISR中的异常综合征
    unsigned int interrupt;    // 中断号（如果通过中断）
    uint64_t flags;            // 标志位
    void *cookie;              // 用户数据
    void *handle;              // 上下文句柄
};

// 错误记录信息
struct err_record_info {
    err_record_probe_t probe;      // 探测函数
    err_record_handler_t handler;  // 处理函数
    void *aux_data;                // 辅助数据
    union {
        struct {
            unsigned int idx_start;  // 系统寄存器：起始索引
            unsigned int num_idx;    // 系统寄存器：记录数量
        } sysreg;
        struct {
            uintptr_t base_addr;     // 内存映射：基地址
            unsigned int size_num_k; // 内存映射：大小（KB）
        } memmap;
    };
};
```

### 10.8 总结

ATF中的RAS错误处理流程：

1. **错误触发**：SError异常或RAS中断
2. **异常入口**：路由到EL3，调用plat_ea_handler()
3. **RAS框架**：调用ras_ea_handler()遍历错误记录
4. **错误探测**：通过probe函数检查错误记录状态
5. **错误处理**：调用handler函数处理具体错误
6. **状态清除**：清除错误记录状态位
7. **可选通知**：通过SDEI等机制通知上层软件

这个流程确保了RAS错误能够被及时检测、处理和报告，提高了系统的可靠性和可维护性。

