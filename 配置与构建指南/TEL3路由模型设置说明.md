# TEL3路由模型设置位置分析

## 问题

**检查路由模型：TEL3=0（路由到FEL）这个TEL没看到那里有设置阿**

## 答案

**TEL3=0是默认路由模型，不需要显式设置。** 当`TSP_NS_INTR_ASYNC_PREEMPT=0`时，使用默认路由模型（`INTR_DEFAULT_RM = 0x0`），它对应`TEL3=0`（路由到FEL）。

## 关键理解

### 1. 默认路由模型

**定义**：
```c
// include/bl31/interrupt_mgmt.h:54
#define INTR_DEFAULT_RM			U(0x0)
```

**文档说明**：
```
The default routing model for an interrupt type is to route it to the FEL in
either security state.
```

**关键**：
- ✅ **默认路由模型是`INTR_DEFAULT_RM = 0x0`**
- ✅ **对应`TEL3=0`（路由到FEL）**
- ✅ **不需要显式设置**：如果没有调用`enable_intr_rm_local`，就使用默认路由模型

### 2. 路由模型的计算

**路由模型标志位**：
```c
// include/bl31/interrupt_mgmt.h:63-67
#define INTR_RM_FROM_SEC_SHIFT		SECURE		/* BIT[0] */
#define INTR_RM_FROM_NS_SHIFT		NON_SECURE	/* BIT[1] */
#define get_interrupt_rm_flag(flag, ss) \
	((((flag) >> INTR_RM_FLAGS_SHIFT) >> (ss)) & INTR_RM_FROM_FLAG_MASK)
```

**对于`INTR_DEFAULT_RM = 0x0`**：
- `get_interrupt_rm_flag(0x0, SECURE)` = `((0x0 >> 0) >> 0) & 1` = `0`
- `get_interrupt_rm_flag(0x0, NON_SECURE)` = `((0x0 >> 0) >> 1) & 1` = `0`

**结果**：
- ✅ **从Secure状态**：`TEL3=0`（路由到FEL）
- ✅ **从Non-Secure状态**：`TEL3=0`（路由到FEL）

### 3. 什么时候使用默认路由模型？

**当`TSP_NS_INTR_ASYNC_PREEMPT=0`时**：
- ❌ **不注册NS中断handler**：不会调用`register_interrupt_type_handler(INTR_TYPE_NS, ...)`
- ❌ **不调用`enable_intr_rm_local`**：不会显式设置路由模型
- ✅ **使用默认路由模型**：`INTR_DEFAULT_RM = 0x0`，对应`TEL3=0`（路由到FEL）

**当`TSP_NS_INTR_ASYNC_PREEMPT=1`时**：
- ✅ **注册NS中断handler**：调用`register_interrupt_type_handler(INTR_TYPE_NS, ...)`
- ✅ **设置路由模型**：`set_interrupt_rm_flag(flags, SECURE)`设置`flags = 0x1`
- ✅ **调用`disable_intr_rm_local`**：使用`INTR_DEFAULT_RM`，但之后可以调用`enable_intr_rm_local`来启用

## 代码分析

### 1. 默认路由模型的定义

**位置**：`include/bl31/interrupt_mgmt.h:54`
```c
#define INTR_DEFAULT_RM			U(0x0)
```

**含义**：
- `0x0`表示两个位都是0
- Bit[0]（Secure状态）：`TEL3=0`（路由到FEL）
- Bit[1]（Non-Secure状态）：`TEL3=0`（路由到FEL）

### 2. disable_intr_rm_local使用默认路由模型

**位置**：`bl31/interrupt_mgmt.c:155-167`
```c
int disable_intr_rm_local(uint32_t type, uint32_t security_state)
{
    uint32_t bit_pos, flag;

    assert(intr_type_descs[type].handler != NULL);

    flag = get_interrupt_rm_flag(INTR_DEFAULT_RM, security_state);  // ← 使用默认路由模型

    bit_pos = plat_interrupt_type_to_line(type, security_state);
    cm_write_scr_el3_bit(security_state, bit_pos, flag);  // ← 设置SCR_EL3的IRQ/FIQ位

    return 0;
}
```

**关键**：
- ✅ **使用`INTR_DEFAULT_RM`**：`get_interrupt_rm_flag(INTR_DEFAULT_RM, security_state)`返回`0`
- ✅ **设置SCR_EL3位**：`cm_write_scr_el3_bit`设置SCR_EL3的IRQ或FIQ位为`0`（TEL3=0）

### 3. 当TSP_NS_INTR_ASYNC_PREEMPT=0时

**代码位置**：`services/spd/tspd/tspd_main.c:471-490`
```c
#if TSP_NS_INTR_ASYNC_PREEMPT
    // 注册NS中断handler
    flags = 0;
    set_interrupt_rm_flag(flags, SECURE);  // flags = 0x1
    rc = register_interrupt_type_handler(INTR_TYPE_NS, ...);
    disable_intr_rm_local(INTR_TYPE_NS, SECURE);  // 使用默认路由模型
#else
    // 不注册NS中断handler
    // 使用默认路由模型（隐式）
#endif
```

**关键**：
- ❌ **不注册NS中断handler**：如果`TSP_NS_INTR_ASYNC_PREEMPT=0`，不会注册NS中断handler
- ✅ **使用默认路由模型**：即使不注册handler，默认路由模型仍然有效（`TEL3=0`）

### 4. SCR_EL3的IRQ/FIQ位

**SCR_EL3位**：
- **SCR_EL3.IRQ**：控制IRQ中断的路由（0=路由到FEL, 1=路由到EL3）
- **SCR_EL3.FIQ**：控制FIQ中断的路由（0=路由到FEL, 1=路由到EL3）

**默认值**：
- ✅ **默认是0**：如果没有显式设置，SCR_EL3.IRQ和SCR_EL3.FIQ默认是0
- ✅ **对应TEL3=0**：路由到FEL

## 完整流程

### 当TSP_NS_INTR_ASYNC_PREEMPT=0时

```
1. TSPD初始化
   ↓
2. 不注册NS中断handler（因为TSP_NS_INTR_ASYNC_PREEMPT=0）
   ↓
3. 使用默认路由模型（隐式）
   - INTR_DEFAULT_RM = 0x0
   - TEL3=0（路由到FEL）
   ↓
4. SCR_EL3.IRQ/FIQ = 0（默认值，对应TEL3=0）
   ↓
5. 当NS中断触发时，根据SCR_EL3.IRQ/FIQ=0，路由到FEL（S-EL1/TSP）
```

### 当TSP_NS_INTR_ASYNC_PREEMPT=1时

```
1. TSPD初始化
   ↓
2. 注册NS中断handler
   - set_interrupt_rm_flag(flags, SECURE)  // flags = 0x1
   - register_interrupt_type_handler(INTR_TYPE_NS, ...)
   ↓
3. disable_intr_rm_local(INTR_TYPE_NS, SECURE)
   - 使用INTR_DEFAULT_RM = 0x0
   - 设置SCR_EL3.IRQ/FIQ = 0（TEL3=0）
   ↓
4. 在yielding SMC时，enable_intr_rm_local(INTR_TYPE_NS, SECURE)
   - 使用注册时的flags = 0x1
   - 设置SCR_EL3.IRQ/FIQ = 1（TEL3=1，路由到EL3）
```

## 关键理解

### 1. 默认路由模型是隐式的

**不需要显式设置**：
- ✅ **默认路由模型是`INTR_DEFAULT_RM = 0x0`**
- ✅ **对应`TEL3=0`（路由到FEL）**
- ✅ **SCR_EL3.IRQ/FIQ默认是0**：如果没有显式设置，就是0

### 2. CSS是CPU的状态

**CSS（Current Security State）**：
- ✅ **由SCR_EL3.NS位决定**：当CPU在S-EL1执行时，`SCR_EL3.NS=0`，CSS=0
- ✅ **不是设置的**：CSS是CPU的状态，由执行环境决定

### 3. TEL3是路由模型

**TEL3（Target Exception Level 3）**：
- ✅ **由SCR_EL3.IRQ/FIQ位决定**：0=路由到FEL, 1=路由到EL3
- ✅ **默认是0**：如果没有显式设置，SCR_EL3.IRQ/FIQ=0，TEL3=0

## 代码位置总结

### 1. 默认路由模型定义

**位置**：`include/bl31/interrupt_mgmt.h:54`
```c
#define INTR_DEFAULT_RM			U(0x0)
```

### 2. 默认路由模型的使用

**位置**：`bl31/interrupt_mgmt.c:161`
```c
flag = get_interrupt_rm_flag(INTR_DEFAULT_RM, security_state);  // 返回0
```

### 3. SCR_EL3位的设置

**位置**：`bl31/interrupt_mgmt.c:164`
```c
cm_write_scr_el3_bit(security_state, bit_pos, flag);  // flag=0，设置TEL3=0
```

### 4. 文档说明

**位置**：`docs/design/interrupt-framework-design.rst:75-76`
```
The default routing model for an interrupt type is to route it to the FEL in
either security state.
```

## 总结

### 关键点

1. **TEL3=0是默认路由模型**：`INTR_DEFAULT_RM = 0x0`对应`TEL3=0`（路由到FEL）
2. **不需要显式设置**：如果没有调用`enable_intr_rm_local`，就使用默认路由模型
3. **SCR_EL3.IRQ/FIQ默认是0**：对应`TEL3=0`（路由到FEL）
4. **CSS是CPU的状态**：由SCR_EL3.NS位决定，不是设置的

### 流程总结

```
1. 当TSP_NS_INTR_ASYNC_PREEMPT=0时，不注册NS中断handler
2. 使用默认路由模型（隐式）：INTR_DEFAULT_RM = 0x0
3. 默认路由模型对应TEL3=0（路由到FEL）
4. SCR_EL3.IRQ/FIQ默认是0（对应TEL3=0）
5. 当NS中断触发时，根据SCR_EL3.IRQ/FIQ=0，路由到FEL（S-EL1/TSP）
```

### 答案

**TEL3=0是默认路由模型，不需要显式设置。**

**原因**：
- ✅ **默认路由模型是`INTR_DEFAULT_RM = 0x0`**：对应`TEL3=0`（路由到FEL）
- ✅ **SCR_EL3.IRQ/FIQ默认是0**：如果没有显式设置，就是0，对应`TEL3=0`
- ✅ **文档说明**："The default routing model for an interrupt type is to route it to the FEL in either security state."
- ✅ **当`TSP_NS_INTR_ASYNC_PREEMPT=0`时**：不注册NS中断handler，使用默认路由模型（隐式）

