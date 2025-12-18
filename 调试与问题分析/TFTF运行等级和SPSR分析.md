# TFTF运行等级和SPSR分析

## TFTF运行等级

**TFTF（Trusted Firmware Test Framework）通常运行在非安全EL1（NS-EL1）**，不是EL2。

### 为什么SPSR_EL3显示EL2h？

**SPSR_EL3 = 0x80000249，M[3:0] = 0x9 (EL2h)** 表示：

- **异常发生时的状态**，不是TFTF运行时的状态
- 如果系统有EL2（虚拟化层），SError可能先路由到EL2
- EL2处理异常时，SPSR_EL3会保存异常发生时的状态（EL2h）

## 关键理解

### 1. TFTF运行环境

```
正常执行流程：
TFTF (NS-EL1) 
    ↓
SError发生
    ↓
如果HCR_EL2.AMO=1 → 路由到EL2
    ↓
EL2处理异常（或转发到EL3）
    ↓
如果SCR_EL3.EA=1 → 路由到EL3
    ↓
EL3处理异常
```

### 2. SPSR_EL3的含义

**SPSR_EL3保存的是异常发生时的状态**，不是当前运行状态：

- 如果SError在EL1发生，但路由到EL2：
  - SPSR_EL3.M[3:0] = EL2h（异常在EL2处理时的状态）
- 如果SError在EL1发生，直接路由到EL3：
  - SPSR_EL3.M[3:0] = EL1h（异常发生时的状态）

### 3. 路由优先级

ARM架构中SError路由的优先级：

1. **HCR_EL2.AMO = 1** → 路由到EL2（最高优先级）
2. **HCR_EL2.AMO = 0 且 SCR_EL3.EA = 1** → 路由到EL3
3. **HCR_EL2.AMO = 0 且 SCR_EL3.EA = 0** → 路由到当前EL

## 分析SPSR_EL3 = 0x80000249

### 情况分析

**SPSR_EL3.M[3:0] = 0x9 (EL2h)** 可能表示：

1. **系统有EL2，且HCR_EL2.AMO = 1**
   - SError路由到EL2
   - EL2处理异常时，SPSR_EL3保存EL2的状态

2. **或者EL2转发异常到EL3**
   - SError先到EL2，然后转发到EL3
   - SPSR_EL3保存的是EL2的状态

## 检查方法

### 1. 检查当前运行等级

在TFTF中检查当前异常等级：

```c
unsigned int get_current_el(void)
{
    unsigned int el;
    asm volatile("mrs %0, CurrentEL" : "=r" (el));
    return (el >> 2) & 0x3;  // CurrentEL[3:2]
}
```

### 2. 检查HCR_EL2.AMO

在EL3异常处理函数中检查：

```c
void check_serror_routing_in_el3(void)
{
    uint64_t scr_el3 = read_scr_el3();
    uint64_t hcr_el2 = read_hcr_el2();  // EL3可以读取HCR_EL2
    
    INFO("SCR_EL3.EA = %d\n", (scr_el3 >> 3) & 1);
    INFO("HCR_EL2.AMO = %d\n", (hcr_el2 >> 5) & 1);
    
    if ((hcr_el2 >> 5) & 1) {
        INFO("HCR_EL2.AMO=1 → SError路由到EL2\n");
    } else if ((scr_el3 >> 3) & 1) {
        INFO("HCR_EL2.AMO=0 且 SCR_EL3.EA=1 → SError路由到EL3\n");
    }
}
```

## 总结

1. **TFTF运行在EL1**，不是EL2
2. **SPSR_EL3.M[3:0] = 0x9 (EL2h)** 表示异常在EL2处理
3. **可能原因**：HCR_EL2.AMO = 1，SError路由到EL2
4. **需要检查**：HCR_EL2.AMO和SCR_EL3.EA的值

