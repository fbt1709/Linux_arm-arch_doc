# ARM64 跳转指令差异详解：`b` vs `ldr =label; br`

## 问题

在 `arch/arm64/kernel/head.S:533-535` 中：

```asm
ldr	x8, =__primary_switched
adrp	x0, KERNEL_START		// __pa(KERNEL_START)
br	x8
```

为什么不直接使用 `b __primary_switched`？

---

## 两种跳转方式对比

### 方式 1: 相对跳转 `b __primary_switched`

```asm
b __primary_switched
```

**特点**:
- **PC 相对跳转**：目标地址 = PC + 偏移量
- **范围限制**：±128MB（26位有符号立即数，左移2位 = ±128MB）
- **指令编码**：偏移量直接编码在指令中
- **位置无关**：不依赖链接地址，只依赖相对位置

**指令格式**:
```
31  30  29  28  27  26  25  24  23  22  21  20  19  18  17  16  15  14  13  12  11  10  9  8  7  6  5  4  3  2  1  0
+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
| 0 | 0 | 1 | 0 | 1 | 0 | 1 | 1 |                   26位有符号立即数（偏移量）                    |
+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
```

**计算**：`target = PC + (imm26 << 2)`

---

### 方式 2: 绝对地址跳转 `ldr =label; br`

```asm
ldr	x8, =__primary_switched
br	x8
```

**特点**:
- **绝对地址跳转**：目标地址是链接时的绝对虚拟地址
- **无范围限制**：可以跳转到任意64位地址
- **需要 literal pool**：链接器会在附近创建数据池存储地址
- **位置相关**：依赖链接地址

**指令流程**:
1. `ldr x8, =__primary_switched`：
   - 链接器在 literal pool 中存储 `__primary_switched` 的绝对地址
   - 从 literal pool 加载地址到 x8
2. `br x8`：跳转到 x8 寄存器中的地址

---

## 为什么这里必须用绝对地址跳转？

### 关键问题：地址空间分离

让我们看看两个函数的链接位置：

#### 1. `__primary_switch` 的位置

```asm
.section ".idmap.text","a"    // ← 在 .idmap.text section
SYM_FUNC_START_LOCAL(__primary_switch)
    ...
    bl	__pi_early_map_kernel
    ldr	x8, =__primary_switched
    br	x8
```

- **Section**: `.idmap.text`
- **链接地址**: 可能在低地址（身份映射区域）或高地址（KIMAGE_VADDR）
- **执行地址**: 通过身份映射执行，可能是物理地址或低虚拟地址

#### 2. `__primary_switched` 的位置

```asm
// 没有 .idmap.text，在普通 .text section
SYM_FUNC_START_LOCAL(__primary_switched)
    adr_l	x4, init_task
    ...
```

- **Section**: `.text`（普通代码段）
- **链接地址**: `KIMAGE_VADDR`（高虚拟地址，如 `0xffff800008000000`）
- **执行地址**: 必须通过线性映射在高虚拟地址执行

---

### 地址差距分析

假设：
- **KIMAGE_VADDR** = `0xffff800008000000` (高虚拟地址)
- **当前执行地址**（`__primary_switch` 通过身份映射执行）= `0x40080000` (物理地址)

**地址差距**：
```
0xffff800008000000 - 0x40080000 = 0xffff7fffc7f80000
≈ 约 256TB
```

**相对跳转范围**：±128MB = ±0x8000000

**结论**：地址差距远超 128MB，**相对跳转无法到达**！

---

## 详细执行流程

### 场景：从身份映射跳转到线性映射

```
┌─────────────────────────────────────────────────────────────┐
│ 阶段 1: 在身份映射中执行                                     │
│                                                             │
│ __primary_switch (在 .idmap.text 中)                        │
│ - 当前 PC: 0x40080000 (物理地址，通过身份映射访问)          │
│ - TTBR0: 指向身份映射页表                                    │
│ - TTBR1: 指向 swapper_pg_dir（已创建内核线性映射）          │
│                                                             │
│ 执行: bl __pi_early_map_kernel()                            │
│   → 创建内核代码的线性映射到高虚拟地址                       │
│   → 将页表复制到 swapper_pg_dir                             │
│   → TTBR1 已指向 swapper_pg_dir                             │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 阶段 2: 跳转到高虚拟地址                                     │
│                                                             │
│ ldr x8, =__primary_switched                                  │
│   → x8 = 0xffff800008000000 (链接时的绝对虚拟地址)          │
│                                                             │
│ br x8                                                        │
│   → PC = 0xffff800008000000                                 │
│   → 此时通过 TTBR1 访问（高虚拟地址）                       │
│   → 线性映射已建立，可以正常执行                            │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 阶段 3: 在高虚拟地址执行                                     │
│                                                             │
│ __primary_switched (在 .text 中)                            │
│ - 当前 PC: 0xffff800008000000 (高虚拟地址)                  │
│ - 通过 TTBR1 的线性映射访问                                 │
│ - 内核代码正常运行在高虚拟地址空间                          │
└─────────────────────────────────────────────────────────────┘
```

---

## 如果使用 `b __primary_switched` 会怎样？

### 问题 1: 地址超出范围

```asm
// 假设当前执行地址是 0x40080000
b __primary_switched  // 目标地址 0xffff800008000000

// 计算偏移量
offset = 0xffff800008000000 - 0x40080000
       = 0xffff7fffc7f80000
       ≈ 256TB

// 但 b 指令只能编码 ±128MB 的偏移量！
// 结果：链接器报错或生成错误的跳转
```

### 问题 2: 相对跳转的语义问题

即使地址在范围内，相对跳转也有问题：

```asm
// 相对跳转：target = PC + offset
// 如果当前 PC 是物理地址 0x40080000
// 跳转后 PC = 0x40080000 + offset
// 但我们需要的是虚拟地址 0xffff800008000000！
```

**相对跳转无法跨越地址空间**（从物理地址/低虚拟地址 → 高虚拟地址）

---

## 代码验证

让我们看看链接脚本中的定义：

```c
// arch/arm64/kernel/vmlinux.lds.S
. = KIMAGE_VADDR;  // 高虚拟地址起始

.head.text : {
    _text = .;
    HEAD_TEXT  // __primary_switch 可能在这里
}

.text : {
    _stext = .;
    ENTRY_TEXT  // __primary_switched 在这里
}
```

**关键点**：
- `.idmap.text` 中的代码会被**同时**放在身份映射区域和正常 `.text` 区域
- `__primary_switch` 在 `.idmap.text` 中，执行时通过身份映射
- `__primary_switched` 在 `.text` 中，链接在高虚拟地址

---

## 其他使用场景

### 什么时候可以用 `b`？

**场景 1**: 同一 section 内的跳转
```asm
.section ".text"
func1:
    ...
    b func2  // ✅ 可以，同一 section，地址差距小
func2:
    ...
```

**场景 2**: 地址差距 < 128MB
```asm
// 如果两个函数链接地址差距 < 128MB
b nearby_function  // ✅ 可以
```

### 什么时候必须用 `ldr =label; br`？

**场景 1**: 跨地址空间跳转（如本例）
```asm
// 从身份映射跳转到线性映射
ldr x8, =high_address_function
br x8  // ✅ 必须用绝对地址
```

**场景 2**: 地址差距 > 128MB
```asm
// 如果目标函数很远
ldr x8, =far_away_function
br x8  // ✅ 必须用绝对地址
```

**场景 3**: 动态计算目标地址
```asm
// 目标地址需要运行时计算
adrp x8, function_table
ldr x8, [x8, #offset]
br x8  // ✅ 寄存器跳转
```

---

## 性能对比

| 方式 | 指令数 | 延迟 | 代码大小 |
|------|--------|------|----------|
| `b label` | 1 条 | 1 周期 | 4 字节 |
| `ldr x8, =label; br x8` | 2 条 + literal pool | 2-3 周期 | 12 字节 + 8 字节数据 |

**结论**：相对跳转更快更小，但受范围限制。

---

## 总结

### 为什么必须用 `ldr =label; br`？

1. **地址空间跨越**：
   - `__primary_switch` 通过身份映射执行（可能是物理地址）
   - `__primary_switched` 链接在高虚拟地址（KIMAGE_VADDR）
   - 地址差距远超 128MB

2. **相对跳转限制**：
   - `b` 指令只能跳转 ±128MB
   - 无法跨越不同的地址空间

3. **绝对地址需求**：
   - 需要跳转到链接时的绝对虚拟地址
   - `ldr =label` 从 literal pool 加载绝对地址
   - `br` 可以跳转到任意 64 位地址

### 关键代码位置

- **`__primary_switch`**: `arch/arm64/kernel/head.S:521`
  - 在 `.idmap.text` section
  - 通过身份映射执行

- **`__primary_switched`**: `arch/arm64/kernel/head.S:217`
  - 在 `.text` section
  - 链接在高虚拟地址（KIMAGE_VADDR）

- **跳转代码**: `arch/arm64/kernel/head.S:533-535`
  - 使用绝对地址跳转，跨越地址空间

---

## 参考资料

- ARM64 指令集架构文档：Branch instructions
- Linux 内核文档：`Documentation/arm64/memory.rst`
- 链接脚本：`arch/arm64/kernel/vmlinux.lds.S`

