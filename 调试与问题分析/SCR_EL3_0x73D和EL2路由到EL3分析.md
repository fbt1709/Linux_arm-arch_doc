# SCR_EL3=0x73D和EL2路由到EL3分析

## SCR_EL3=0x73D分析

**SCR_EL3 = 0x73D = 0b011100111101**

### 关键位分析

| 位 | 名称 | 值 | 含义 |
|---|------|-----|------|
| 0 | NS | 1 | 非安全状态 |
| 3 | **EA** | **1** | **SError路由到EL3** |
| 4 | FI | 1 | FIQ路由到EL3 |
| 5 | IR | 1 | IRQ路由到EL3 |
| 8 | HCE | 0 | EL2未启用（或未设置） |
| ... | ... | ... | ... |

**关键发现**：
- ✅ **SCR_EL3.EA = 1**：SError应该路由到EL3
- ⚠️ **SCR_EL3.HCE = 0**：EL2未启用（或未设置）

## SError路由机制

### 路由优先级（从高到低）

```
1. HCR_EL2.AMO = 1 → 路由到EL2（最高优先级）
   ↓
2. HCR_EL2.AMO = 0 且 SCR_EL3.EA = 1 → 路由到EL3
   ↓
3. 否则 → 路由到当前EL（FEL）
```

### 你的情况分析

**SCR_EL3 = 0x73D，EA = 1**

**如果HCR_EL2.AMO = 0**：
```
SError发生
    ↓
硬件检查：
  - HCR_EL2.AMO = 0
  - SCR_EL3.EA = 1
    ↓
直接路由到EL3 ✅
    ↓
EL3处理SError
```

**如果HCR_EL2.AMO = 1**：
```
SError发生
    ↓
硬件检查：
  - HCR_EL2.AMO = 1（最高优先级）
    ↓
直接路由到EL2（不检查SCR_EL3.EA）
    ↓
EL2处理SError
    ↓
EL2可能转发到EL3（软件行为）
    ↓
EL3处理SError
```

## EL2路由到EL3的机制

### 硬件路由 vs 软件转发

**硬件路由**：
- 由硬件自动完成
- 根据HCR_EL2.AMO和SCR_EL3.EA决定
- 直接路由到目标EL

**软件转发**：
- EL2的异常处理函数可以转发异常到EL3
- 这是软件行为，不是硬件路由
- 需要EL2有异常处理函数

### EL2转发到EL3的情况

**如果HCR_EL2.AMO = 1**：
```
SError路由到EL2（硬件）
    ↓
EL2异常处理函数执行
    ↓
EL2可以：
  1. 处理异常
  2. 转发到EL3（通过SMC或其他机制）
  3. 或panic
```

**关键点**：
- EL2转发到EL3是**软件行为**
- 需要EL2有异常处理函数
- 不是硬件自动路由

## SCR_EL3=0x73D的影响

### 情况1：HCR_EL2.AMO = 0

**路由结果**：
- ✅ SError直接路由到EL3（硬件路由）
- ✅ SCR_EL3.EA = 1生效
- ✅ EL3处理SError

### 情况2：HCR_EL2.AMO = 1

**路由结果**：
- ⚠️ SError先路由到EL2（硬件路由，最高优先级）
- ⚠️ SCR_EL3.EA = 1被忽略（HCR_EL2.AMO优先级更高）
- ⚠️ EL2处理SError
- ⚠️ EL2可能转发到EL3（软件行为）

## 你的情况（之前观察到的）

**之前观察到**：
- SPSR_EL3.M[3:0] = 0x9 (EL2h)
- 说明SError经过了EL2

**可能的原因**：
1. **HCR_EL2.AMO = 1**：SError先路由到EL2
2. **EL2转发到EL3**：EL2的异常处理函数转发到EL3
3. **SPSR_EL3保存EL2状态**：因为异常从EL2转发到EL3

## 总结

**SCR_EL3=0x73D（EA=1）的影响**：

1. **如果HCR_EL2.AMO = 0**：
   - ✅ SError直接路由到EL3（硬件路由）
   - ✅ SCR_EL3.EA = 1生效

2. **如果HCR_EL2.AMO = 1**：
   - ⚠️ SError先路由到EL2（硬件路由，最高优先级）
   - ⚠️ SCR_EL3.EA = 1被忽略
   - ⚠️ EL2可能转发到EL3（软件行为）

**关键点**：
- SCR_EL3.EA=1**不能保证**SError直接路由到EL3
- 如果HCR_EL2.AMO=1，SError会先路由到EL2
- EL2转发到EL3是软件行为，不是硬件路由

**你的情况**：
- SCR_EL3.EA = 1 ✅
- 但HCR_EL2.AMO可能 = 1（需要确认）
- 导致SError先路由到EL2，然后转发到EL3

