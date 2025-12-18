# SGI 0-7 和 8-15 的安全/非安全划分说明

## 关键结论

**SGI 0-15 的划分是软件约定，不是硬件限制！**

- **硬件层面**：GIC规范中，所有SGI（0-15）都是相同的，没有硬件强制区分安全/非安全
- **软件约定**：ARM平台代码中约定：
  - **0-7**：通常用于非安全（NS SGI）
  - **8-15**：通常用于安全（Secure SGI）
- **实际配置**：任何SGI都可以通过配置寄存器设置为安全或非安全

## 为什么会有这个约定？

### 1. 历史原因和兼容性

```c
// include/drivers/arm/gic_common.h
#define MIN_SGI_ID          U(0)    // SGI 0-7: 传统上用于非安全
#define MIN_SEC_SGI_ID      U(8)    // SGI 8-15: 传统上用于安全
```

这个约定来自于：
- **GICv2时代**：在GICv2中，SGI的配置更简单，0-7通常留给非安全世界使用
- **向后兼容**：保持与旧代码和文档的兼容性
- **代码可读性**：明确的命名约定使代码更易理解

### 2. 默认配置行为

```c
// drivers/arm/gic/v3/gicv3_helpers.c:286-332
void gicv3_ppi_sgi_config_defaults(uintptr_t gicr_base)
{
    // ...
    /* 32 interrupt IDs per GICR_IGROUPR register */
    for (i = 0U; i < ppi_regs_num; ++i) {
        /* Treat all SGIs/(E)PPIs as G1NS by default */
        gicr_write_igroupr(gicr_base, i, ~0U);  // 所有SGI默认都是G1NS！
    }
}
```

**重要发现**：在ATF初始化时，**所有SGI（包括8-15）默认都被配置为G1NS（非安全组1）**！

### 3. 平台配置覆盖默认值

```c
// include/plat/arm/common/arm_def.h
#define ARM_IRQ_SEC_SGI_0   8   // 平台定义：SGI 8为安全
#define ARM_IRQ_SEC_SGI_1   9   // 平台定义：SGI 9为安全
// ...

// 在平台初始化时，通过 interrupt_props 配置
#define ARM_G0_IRQ_PROPS(grp) \
    INTR_PROP_DESC(ARM_IRQ_SEC_SGI_0, PLAT_SDEI_NORMAL_PRI, (grp), \
            GIC_INTR_CFG_EDGE), \
    // ...
```

平台代码会通过 `interrupt_props` 将特定的SGI（如8-15）重新配置为安全。

## 如何判断SGI是安全还是非安全？

### 寄存器配置

SGI的安全/非安全属性由以下寄存器决定：

1. **GICR_IGROUPR0**：组寄存器
   - `Bit[i] = 0`：Secure（需要结合IGRPMODR判断是G0还是G1S）
   - `Bit[i] = 1`：Non-Secure（G1NS）

2. **GICR_IGRPMODR0**：组模式寄存器（仅用于Secure）
   - `Bit[i] = 0`：Group 0（EL3）
   - `Bit[i] = 1`：Group 1 Secure（S-EL1）

### 判断逻辑

```c
// drivers/arm/gic/v3/gicv3_main.c:432-484
unsigned int gicv3_get_interrupt_group(unsigned int id, unsigned int proc_num)
{
    // ...
    if (igroup != 0U) {
        return INTR_GROUP1NS;  // 非安全
    }
    
    if (grpmodr != 0U) {
        return INTR_GROUP1S;    // 安全组1（S-EL1）
    }
    
    return INTR_GROUP0;         // 安全组0（EL3）
}
```

## 为什么中断号8可以路由到非安全世界？

### 原因1：默认配置

初始化时，所有SGI（包括8）默认都是G1NS：

```c
gicv3_ppi_sgi_config_defaults(gicr_base);
// 所有SGI被配置为 G1NS（非安全）
```

### 原因2：软件可以重新配置

任何SGI都可以通过软件重新配置：

```c
// 将SGI 8配置为非安全
gicv3_set_interrupt_group(8, proc_num, INTR_GROUP1NS);

// 将SGI 0配置为安全
gicv3_set_interrupt_group(0, proc_num, INTR_GROUP0);
```

### 原因3：没有硬件限制

GIC硬件**不区分**SGI 0-7和8-15，它们只是不同的中断ID。

## 实际使用建议

### 1. 遵循平台约定（推荐）

```c
// 使用平台定义的宏
#define ARM_IRQ_SEC_SGI_0   8   // 用于安全
#define ARM_IRQ_NS_SGI_0     0   // 用于非安全（如果平台定义了）
```

### 2. 明确配置

不要依赖默认值，显式配置：

```c
// 配置SGI 8为安全EL3
plat_ic_set_interrupt_type(ARM_IRQ_SEC_SGI_0, INTR_TYPE_EL3);

// 配置SGI 0为非安全
plat_ic_set_interrupt_type(0, INTR_TYPE_NS);
```

### 3. 检查当前配置

```c
// 检查SGI 8的当前类型
unsigned int type = plat_ic_get_interrupt_type(8);
if (type == INTR_TYPE_NS) {
    // SGI 8当前配置为非安全
}
```

## 总结

| 方面 | 说明 |
|------|------|
| **硬件限制** | ❌ 无限制，所有SGI（0-15）都可以配置为安全或非安全 |
| **软件约定** | ✅ 0-7通常用于NS，8-15通常用于Secure |
| **默认配置** | ⚠️ 所有SGI默认都是G1NS（非安全） |
| **实际配置** | ✅ 可以通过软件重新配置任何SGI |
| **你的观察** | ✅ 正确！中断号8可以路由到非安全世界 |

**关键点**：SGI 0-7和8-15的划分是**软件约定**，不是硬件限制。任何SGI都可以通过配置寄存器设置为安全或非安全。

