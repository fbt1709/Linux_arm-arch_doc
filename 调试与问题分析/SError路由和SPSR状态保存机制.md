# SError路由和SPSR状态保存机制

## 问题

**TFTF运行在EL1，但为什么异常发生时SPSR_EL3显示EL2h？**

## ARM异常处理机制

### 1. 异常路由流程

```
TFTF在EL1执行
    ↓
SError发生（在EL1）
    ↓
硬件检查路由配置：
  - HCR_EL2.AMO = 1? → 路由到EL2
  - HCR_EL2.AMO = 0 且 SCR_EL3.EA = 1? → 路由到EL3
  - 否则 → 路由到当前EL（EL1）
    ↓
假设HCR_EL2.AMO = 1，路由到EL2
    ↓
硬件自动保存状态：
  - SPSR_EL2 ← 保存EL1的状态（异常发生时的状态）
  - ELR_EL2 ← 保存异常发生时的PC
    ↓
EL2异常处理函数执行
    ↓
EL2可能：
  - 处理异常
  - 转发异常到EL3（如果配置）
    ↓
如果转发到EL3：
  - SPSR_EL3 ← 保存EL2的状态（当前EL2的状态）
  - ELR_EL3 ← 保存EL2的PC
```

### 2. SPSR保存的时机

**关键理解**：SPSR保存的是**异常发生时的状态**，不是当前运行状态。

#### 情况1：SError直接路由到EL3

```
EL1执行 → SError发生 → 路由到EL3
    ↓
硬件自动：
  - SPSR_EL3 ← 保存EL1的状态（M[3:0] = EL1h）
  - ELR_EL3 ← 保存EL1的PC
```

#### 情况2：SError先路由到EL2，再转发到EL3

```
EL1执行 → SError发生 → 路由到EL2
    ↓
硬件自动：
  - SPSR_EL2 ← 保存EL1的状态（M[3:0] = EL1h）
  - ELR_EL2 ← 保存EL1的PC
    ↓
EL2处理异常，然后转发到EL3
    ↓
硬件自动：
  - SPSR_EL3 ← 保存EL2的状态（M[3:0] = EL2h）← 这就是你看到的！
  - ELR_EL3 ← 保存EL2的PC
```

### 3. 为什么SPSR_EL3显示EL2h？

**SPSR_EL3.M[3:0] = 0x9 (EL2h)** 表示：

1. **SError先被路由到EL2**（因为HCR_EL2.AMO = 1）
2. **EL2处理了异常**（或转发到EL3）
3. **当异常到达EL3时**，硬件保存的是**EL2的状态**，不是EL1的状态

**关键点**：SPSR_EL3保存的是**异常到达EL3时的来源EL的状态**，不是最初异常发生时的EL1状态。

## 状态保存的代码位置

### 硬件自动保存（不需要软件干预）

当异常路由到某个EL时，硬件会自动：

```assembly
// 硬件自动执行（伪代码）
if (异常路由到EL2) {
    SPSR_EL2 = 当前PSTATE;  // 保存异常发生时的状态
    ELR_EL2 = PC;            // 保存异常发生时的PC
    // 切换到EL2
}

if (EL2转发异常到EL3) {
    SPSR_EL3 = 当前EL2的PSTATE;  // 保存EL2的状态
    ELR_EL3 = EL2的PC;           // 保存EL2的PC
    // 切换到EL3
}
```

### 软件可以修改SPSR（在异常处理函数中）

```c
// 在EL3异常处理函数中
void handle_serror_in_el3(void)
{
    // 可以读取SPSR_EL3查看异常发生时的状态
    uint64_t spsr_el3 = read_spsr_el3();
    
    // 可以修改SPSR_EL3（如果需要）
    // 但通常不应该修改，因为需要用它来恢复执行
}
```

## 验证方法

### 检查完整的异常路径

在EL3异常处理函数中检查：

```c
void check_serror_path(void)
{
    uint64_t spsr_el3 = read_spsr_el3();
    uint64_t spsr_el2;  // 如果可以从EL3读取
    uint64_t elr_el3 = read_elr_el3();
    
    INFO("SPSR_EL3 = 0x%016llx\n", spsr_el3);
    INFO("  M[3:0] = 0x%llx\n", spsr_el3 & 0xF);
    
    if ((spsr_el3 & 0xF) == 0x9) {
        INFO("异常路径: EL1 → EL2 → EL3\n");
        INFO("  - SError在EL1发生\n");
        INFO("  - 路由到EL2（HCR_EL2.AMO=1）\n");
        INFO("  - EL2转发到EL3\n");
        INFO("  - SPSR_EL3保存的是EL2的状态\n");
    }
}
```

## 总结

**为什么SPSR_EL3显示EL2h？**

1. ✅ **TFTF运行在EL1**（这是正确的）
2. ✅ **SError在EL1发生**（这也是正确的）
3. ✅ **SError被路由到EL2**（因为HCR_EL2.AMO = 1）
4. ✅ **EL2处理或转发异常到EL3**
5. ✅ **硬件自动保存EL2的状态到SPSR_EL3**

**关键点**：
- SPSR_EL3保存的是**异常到达EL3时的来源EL的状态**
- 如果异常经过EL2，SPSR_EL3就保存EL2的状态
- 如果异常直接从EL1到EL3，SPSR_EL3就保存EL1的状态

**这个状态是在硬件异常路由时自动保存的，不需要软件干预。**

