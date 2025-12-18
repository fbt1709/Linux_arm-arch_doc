# PSCI `power_state` 参数编码详解

## 一、核心问题

**问题**：传递给 PSCI 的 `power_state` 参数，这个 32 位值代表三种不同意思？

**答案**：是的。PSCI v0.2 的 `power_state` 参数用一个 32 位值编码了**三种信息**：
1. **Power State ID**（16 位）：具体的电源状态标识符
2. **Power State Type**（1 位）：电源状态类型（STANDBY 或 POWER_DOWN）
3. **Affinity Level**（2 位）：亲和性级别

---

## 二、PSCI v0.2 的 `power_state` 编码

### 2.1 位域定义

**位置**: `include/uapi/linux/psci.h:72-80`

```c
/* PSCI v0.2 power state encoding for CPU_SUSPEND function */
#define PSCI_0_2_POWER_STATE_ID_MASK		0xffff
#define PSCI_0_2_POWER_STATE_ID_SHIFT		0
#define PSCI_0_2_POWER_STATE_TYPE_SHIFT		16
#define PSCI_0_2_POWER_STATE_TYPE_MASK		\
				(0x1 << PSCI_0_2_POWER_STATE_TYPE_SHIFT)
#define PSCI_0_2_POWER_STATE_AFFL_SHIFT		24
#define PSCI_0_2_POWER_STATE_AFFL_MASK		\
				(0x3 << PSCI_0_2_POWER_STATE_AFFL_SHIFT)
```

### 2.2 32 位编码结构

```
31  30  29  28  27  26  25  24  23  22  21  20  19  18  17  16  15  14  13  12  11  10   9   8   7   6   5   4   3   2   1   0
└─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
│                                                                                                                              │
│  [31:26] 未使用                                                                                                              │
│  [25:24] Affinity Level (2 bits)                                                                                             │
│  [23:17] 未使用                                                                                                              │
│  [16]    Power State Type (1 bit)                                                                                           │
│  [15:0]  Power State ID (16 bits)                                                                                            │
└─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

### 2.3 三种信息的详细说明

#### 1. Power State ID（位 0-15，16 位）

**作用**：
- 标识**具体的电源状态**
- 由平台/固件定义，不同平台可能有不同的电源状态 ID

**提取方式**：
```c
u32 state_id = (power_state & PSCI_0_2_POWER_STATE_ID_MASK) >> PSCI_0_2_POWER_STATE_ID_SHIFT;
// 或简化为：
u32 state_id = power_state & 0xffff;
```

**示例**：
- `0x0000`：可能是最浅的睡眠状态
- `0x0001`：可能是中等深度的睡眠状态
- `0x0002`：可能是深度睡眠状态

#### 2. Power State Type（位 16，1 位）

**作用**：
- 指示电源状态的**类型**：是 **STANDBY**（待机）还是 **POWER_DOWN**（掉电）

**定义**：

**位置**: `include/linux/psci.h:14-15`

```c
#define PSCI_POWER_STATE_TYPE_STANDBY		0
#define PSCI_POWER_STATE_TYPE_POWER_DOWN	1
```

**提取方式**：
```c
u32 state_type = (power_state & PSCI_0_2_POWER_STATE_TYPE_MASK) >> PSCI_0_2_POWER_STATE_TYPE_SHIFT;
// 或简化为：
u32 state_type = (power_state >> 16) & 0x1;
```

**含义**：
- **`0` (STANDBY)**：待机状态，CPU 上下文可能保留，唤醒速度快
- **`1` (POWER_DOWN)**：掉电状态，CPU 上下文丢失，需要从入口地址重新启动

**使用场景**：

**位置**: `drivers/firmware/psci/psci.c:93-100`

```c
static inline bool psci_power_state_loses_context(u32 state)
{
    const u32 mask = psci_has_ext_power_state() ?
                    PSCI_1_0_EXT_POWER_STATE_TYPE_MASK :
                    PSCI_0_2_POWER_STATE_TYPE_MASK;

    return state & mask;  // ← 如果 bit 16 = 1，表示会丢失上下文
}
```

**关键理解**：
- **如果 `state_type == 1`**：CPU 会丢失上下文，需要保存/恢复寄存器状态
- **如果 `state_type == 0`**：CPU 可能保留上下文，唤醒更快

#### 3. Affinity Level（位 24-25，2 位）

**作用**：
- 指定**亲和性级别**，表示电源状态影响的 CPU 层次

**提取方式**：
```c
u32 affl = (power_state & PSCI_0_2_POWER_STATE_AFFL_MASK) >> PSCI_0_2_POWER_STATE_AFFL_SHIFT;
// 或简化为：
u32 affl = (power_state >> 24) & 0x3;
```

**含义**：
- **`0`**：CPU 级别（单个 CPU）
- **`1`**：Cluster 级别（CPU 集群）
- **`2`**：系统级别（整个系统）
- **`3`**：保留/未使用

**使用场景**：
- 当需要让整个 CPU 集群进入睡眠时，设置 `affl = 1`
- 当只需要单个 CPU 睡眠时，设置 `affl = 0`

---

## 三、完整示例

### 3.1 构建 `power_state` 值

```c
// 示例：构建一个 power_state 值
u32 power_state = 0;

// 1. 设置 Power State ID = 0x0001（中等深度睡眠）
power_state |= (0x0001 << PSCI_0_2_POWER_STATE_ID_SHIFT);

// 2. 设置 Power State Type = POWER_DOWN（会丢失上下文）
power_state |= (PSCI_POWER_STATE_TYPE_POWER_DOWN << PSCI_0_2_POWER_STATE_TYPE_SHIFT);

// 3. 设置 Affinity Level = 0（CPU 级别）
power_state |= (0 << PSCI_0_2_POWER_STATE_AFFL_SHIFT);

// 结果：power_state = 0x00010001
//       位 0-15:  0x0001 (State ID)
//       位 16:    1 (POWER_DOWN)
//       位 24-25: 0 (CPU 级别)
```

### 3.2 解析 `power_state` 值

```c
// 示例：解析 power_state = 0x00010001
u32 power_state = 0x00010001;

// 1. 提取 Power State ID
u32 state_id = power_state & PSCI_0_2_POWER_STATE_ID_MASK;
// state_id = 0x0001

// 2. 提取 Power State Type
u32 state_type = (power_state & PSCI_0_2_POWER_STATE_TYPE_MASK) >> PSCI_0_2_POWER_STATE_TYPE_SHIFT;
// state_type = 1 (POWER_DOWN)

// 3. 提取 Affinity Level
u32 affl = (power_state & PSCI_0_2_POWER_STATE_AFFL_MASK) >> PSCI_0_2_POWER_STATE_AFFL_SHIFT;
// affl = 0 (CPU 级别)

// 4. 判断是否会丢失上下文
bool loses_context = psci_power_state_loses_context(power_state);
// loses_context = true（因为 state_type = 1）
```

---

## 四、PSCI v1.0 扩展编码（Extended Power State）

### 4.1 扩展编码定义

**位置**: `include/uapi/linux/psci.h:82-87`

```c
/* PSCI extended power state encoding for CPU_SUSPEND function */
#define PSCI_1_0_EXT_POWER_STATE_ID_MASK	0xfffffff
#define PSCI_1_0_EXT_POWER_STATE_ID_SHIFT	0
#define PSCI_1_0_EXT_POWER_STATE_TYPE_SHIFT	30
#define PSCI_1_0_EXT_POWER_STATE_TYPE_MASK	\
				(0x1 << PSCI_1_0_EXT_POWER_STATE_TYPE_SHIFT)
```

### 4.2 扩展编码结构

```
31  30  29  28  27  26  25  24  23  22  21  20  19  18  17  16  15  14  13  12  11  10   9   8   7   6   5   4   3   2   1   0
└─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
│                                                                                                                              │
│  [31]    Power State Type (1 bit)                                                                                            │
│  [29:0]  Power State ID (28 bits)                                                                                            │
└─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

**关键变化**：
- **Power State ID** 扩展到 **28 位**（位 0-29）
- **Power State Type** 移到 **位 30**
- **移除了 Affinity Level**（不再需要单独编码）

---

## 五、使用场景

### 5.1 CPU 挂起

**位置**: `drivers/firmware/psci/psci.c`

```c
int psci_cpu_suspend_enter(u32 state)
{
    // state 参数就是编码了三种信息的 power_state
    // PSCI 固件会根据这三个信息决定如何挂起 CPU
    return invoke_psci_fn(PSCI_0_2_FN_CPU_SUSPEND, state, ...);
}
```

### 5.2 验证 `power_state` 有效性

**位置**: `drivers/firmware/psci/psci.c:102-109`

```c
bool psci_power_state_is_valid(u32 state)
{
    const u32 valid_mask = psci_has_ext_power_state() ?
                   PSCI_1_0_EXT_POWER_STATE_MASK :
                   PSCI_0_2_POWER_STATE_MASK;

    return !(state & ~valid_mask);  // ← 检查是否只有有效位被设置
}
```

---

## 六、总结

### 6.1 PSCI v0.2 编码

| 字段 | 位域 | 位数 | 含义 |
|------|------|------|------|
| **Power State ID** | [15:0] | 16 | 具体的电源状态标识符 |
| **Power State Type** | [16] | 1 | 0=STANDBY, 1=POWER_DOWN |
| **Affinity Level** | [25:24] | 2 | 0=CPU, 1=Cluster, 2=System |
| **未使用** | [31:26] | 6 | 保留位 |

### 6.2 PSCI v1.0 扩展编码

| 字段 | 位域 | 位数 | 含义 |
|------|------|------|------|
| **Power State ID** | [29:0] | 28 | 扩展的电源状态标识符 |
| **Power State Type** | [30] | 1 | 0=STANDBY, 1=POWER_DOWN |
| **未使用** | [31] | 1 | 保留位 |

### 6.3 关键理解

1. **Power State ID**：平台/固件定义的电源状态编号
2. **Power State Type**：决定是否会丢失 CPU 上下文
3. **Affinity Level**：决定影响范围（CPU/Cluster/System）

**核心设计**：用一个 32 位值同时编码三种信息，减少参数传递，提高效率。

---

**核心理解**：PSCI 的 `power_state` 参数用一个 32 位值编码了**三种信息**：**Power State ID**（16 位）、**Power State Type**（1 位）、**Affinity Level**（2 位）。这种编码方式既节省参数传递，又提供了足够的灵活性来支持不同的电源管理策略。

