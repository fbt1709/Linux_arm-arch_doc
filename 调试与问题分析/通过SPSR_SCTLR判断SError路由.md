# 通过SPSR和SCTLR判断SError路由

## SPSR_EL3分析

**SPSR_EL3（Saved Program Status Register）**保存异常发生时的处理器状态。

### 关键位域

| 位域 | 名称 | 说明 |
|------|------|------|
| [3:0] | M[3:0] | **异常发生时的异常等级** ← 关键！ |
| [4] | M[4] | 执行状态（0=AArch32, 1=AArch64） |
| [6:5] | F, I | FIQ和IRQ掩码 |
| [7] | A | SError掩码 |
| [8] | D | 调试异常掩码 |
| [9] | IL | 非法执行状态位 |
| [10] | SS | 软件步进 |
| [11] | PAN | Privileged Access Never |
| [12] | UAO | User Access Override |
| [13] | DIT | Data Independent Timing |
| [14] | TCO | Tag Check Override |
| [20:16] | IL | 指令长度（AArch32） |
| [23:22] | EL | **异常等级（部分实现）** |
| [24] | nRW | 执行状态（0=AArch32, 1=AArch64） |
| [25] | F | FIQ掩码 |
| [26] | I | IRQ掩码 |
| [27] | A | **SError掩码** ← 关键！ |
| [28] | D | 调试异常掩码 |
| [31:29] | RES0 | 保留 |

### 判断SError路由的关键位

#### 1. M[3:0] - 异常发生时的异常等级

```
M[3:0] = 0b0001 → EL1
M[3:0] = 0b0010 → EL2  
M[3:0] = 0b0011 → EL3  ← 如果SError路由到EL3，这里应该是0b0011
```

**判断方法**：
- 如果 `M[3:0] == 0b0011`（EL3），说明**SError路由到了EL3** ✓
- 如果 `M[3:0] == 0b0001`（EL1），说明**SError路由到了EL1** ✗

#### 2. M[4] - 执行状态

- `M[4] = 0` → AArch32
- `M[4] = 1` → AArch64

#### 3. A位（bit 27）- SError掩码

- `A = 0` → SError未被掩码（可能触发）
- `A = 1` → SError被掩码（不会触发）

**注意**：如果A=1，说明SError被掩码了，可能不会触发。

## SCTLR分析

**SCTLR（System Control Register）**是系统控制寄存器，但**不包含SError路由信息**。

SError路由由**SCR_EL3.EA**控制，不在SCTLR中。

## 判断方法

### 方法1：通过SPSR_EL3.M[3:0]判断

```c
void analyze_serror_routing_from_spsr(uint64_t spsr_el3)
{
    uint64_t m = spsr_el3 & 0xF;  // M[3:0]
    
    printf("SPSR_EL3 = 0x%016llx\n", spsr_el3);
    printf("M[3:0] = 0x%llx\n", m);
    
    if (m == 0x3) {
        printf("-> SError路由到EL3 ✓\n");
    } else if (m == 0x1) {
        printf("-> SError路由到EL1 ✗\n");
    } else if (m == 0x2) {
        printf("-> SError路由到EL2 ✗\n");
    }
    
    // 检查A位（SError掩码）
    if (spsr_el3 & (1ULL << 27)) {
        printf("警告: SError被掩码（A=1）\n");
    }
}
```

### 方法2：完整的SPSR分析

```c
void analyze_spsr_el3_detailed(uint64_t spsr_el3)
{
    printf("========================================\n");
    printf("SPSR_EL3 = 0x%016llx 详细分析:\n", spsr_el3);
    
    // M[3:0] - 异常等级
    uint64_t m = spsr_el3 & 0xF;
    printf("  M[3:0] = 0x%llx - ", m);
    if (m == 0x3) {
        printf("EL3 ✓ (SError路由到EL3)\n");
    } else if (m == 0x1) {
        printf("EL1 ✗ (SError路由到EL1)\n");
    } else if (m == 0x2) {
        printf("EL2 ✗ (SError路由到EL2)\n");
    } else {
        printf("未知\n");
    }
    
    // M[4] - 执行状态
    printf("  M[4] = %d - %s\n", 
           (spsr_el3 >> 4) & 1,
           (spsr_el3 >> 4) & 1 ? "AArch64" : "AArch32");
    
    // A位 - SError掩码
    printf("  A (bit 27) = %d - SError%s\n",
           (spsr_el3 >> 27) & 1,
           (spsr_el3 >> 27) & 1 ? "被掩码" : "未掩码");
    
    // I, F位 - 中断掩码
    printf("  I (bit 26) = %d - IRQ%s\n",
           (spsr_el3 >> 26) & 1,
           (spsr_el3 >> 26) & 1 ? "被掩码" : "未掩码");
    printf("  F (bit 25) = %d - FIQ%s\n",
           (spsr_el3 >> 25) & 1,
           (spsr_el3 >> 25) & 1 ? "被掩码" : "未掩码");
    
    printf("========================================\n");
}
```

## 总结

**通过SPSR_EL3可以判断SError路由**：

1. **SPSR_EL3.M[3:0] = 0x3 (EL3)** → SError路由到EL3 ✓
2. **SPSR_EL3.M[3:0] = 0x1 (EL1)** → SError路由到EL1 ✗
3. **SPSR_EL3.A = 1** → SError被掩码，可能不会触发

**SCTLR不包含路由信息**，但可以检查其他系统配置。

## 注意事项

1. **SPSR_EL3只有在异常到达EL3时才有意义**
   - 如果SError路由到EL1，SPSR_EL3不会更新
   - 需要检查SPSR_EL1来判断

2. **需要结合ESR_EL3一起分析**
   - ESR_EL3.EC字段可以确认异常类型
   - ESR_EL3.EC = 0x2F 表示来自低异常等级的SError

3. **检查异常发生时的上下文**
   - 如果异常在EL3处理，SPSR_EL3会保存异常发生时的状态
   - 如果异常在EL1处理，需要检查SPSR_EL1

