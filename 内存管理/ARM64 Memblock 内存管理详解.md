# ARM64 Memblock 内存管理详解

## 目录

1. [Memblock 概述](#memblock-概述)
2. [Memblock 核心数据结构](#memblock-核心数据结构)
3. [Memblock 工作机制](#memblock-工作机制)
4. [arm64_memblock_init() 详细分析](#arm64_memblock_init-详细分析)
5. [Memblock 与后续内存管理的关系](#memblock-与后续内存管理的关系)
6. [完整流程图](#完整流程图)

---

## Memblock 概述

### 什么是 Memblock？

**Memblock** 是 Linux 内核在启动早期使用的**物理内存管理器**，它在伙伴系统（Buddy System）和页分配器初始化之前，负责：

- **记录物理内存布局**：维护系统中所有可用物理内存区域
- **早期内存分配**：为内核启动阶段提供内存分配服务
- **内存预留管理**：标记哪些物理内存已被使用或保留

### 为什么需要 Memblock？

在系统启动早期，以下组件都还没有初始化：
- ❌ 伙伴系统（Buddy System）
- ❌ Slab 分配器
- ❌ 页表完整映射
- ❌ 虚拟内存管理（VMA）

但内核启动过程中需要分配内存用于：
- ✅ 页表结构
- ✅ 设备树解析
- ✅ 早期数据结构
- ✅ Initrd 处理

**Memblock 就是在这个"真空期"提供内存管理服务**。

### 关键特性

1. **只管理物理地址**：所有操作基于 `phys_addr_t`，不涉及虚拟地址
2. **简单的数组结构**：使用静态数组存储内存区域信息
3. **自动合并相邻区域**：相同类型的相邻区域会自动合并
4. **支持 NUMA**：可以为不同 NUMA 节点分配内存

---

## Memblock 核心数据结构

### 1. memblock_region - 内存区域

```c
struct memblock_region {
    phys_addr_t base;      // 物理地址起始
    phys_addr_t size;      // 区域大小
    enum memblock_flags flags;  // 区域属性标志
#ifdef CONFIG_NUMA
    int nid;              // NUMA 节点 ID
#endif
};
```

**示例**：
```c
// 表示一个从 0x40000000 开始，大小为 1GB 的内存区域
{
    .base = 0x40000000,
    .size = 0x40000000,  // 1GB
    .flags = MEMBLOCK_NONE,
    .nid = 0
}
```

### 2. memblock_type - 内存类型集合

```c
struct memblock_type {
    unsigned long cnt;           // 当前区域数量
    unsigned long max;           // 数组最大容量
    phys_addr_t total_size;      // 所有区域总大小
    struct memblock_region *regions;  // 区域数组
    char *name;                  // 类型名称（用于调试）
};
```

**两种主要类型**：
- **`memory`**：可用的物理内存区域
- **`reserved`**：已被预留/占用的物理内存区域

### 3. memblock - 全局管理器

```c
struct memblock {
    bool bottom_up;              // 分配方向（从低地址还是高地址）
    phys_addr_t current_limit;   // 当前分配限制
    struct memblock_type memory; // 可用内存列表
    struct memblock_type reserved; // 预留内存列表
};
```

**全局实例**：
```c
struct memblock memblock __initdata_memblock = {
    .memory.regions = memblock_memory_init_regions,  // 初始数组
    .memory.max = INIT_MEMBLOCK_MEMORY_REGIONS,      // 默认 128
    .memory.name = "memory",
    
    .reserved.regions = memblock_reserved_init_regions,
    .reserved.max = INIT_MEMBLOCK_RESERVED_REGIONS,
    .reserved.name = "reserved",
    
    .bottom_up = false,         // 默认从高地址分配
    .current_limit = MEMBLOCK_ALLOC_ANYWHERE,
};
```

---

## Memblock 工作机制

### 1. 添加内存区域 - memblock_add()

**功能**：将一段物理内存添加到 `memblock.memory` 列表

```c
int memblock_add(phys_addr_t base, phys_addr_t size)
{
    return memblock_add_range(&memblock.memory, base, size, 
                              MAX_NUMNODES, 0);
}
```

**工作流程**：

```
memblock_add(0x40000000, 0x10000000)  // 添加 256MB 内存
    ↓
memblock_add_range()
    ↓
1. 检查是否与现有区域重叠
2. 如果重叠，分割并插入新区域
3. 自动合并相邻的相同类型区域
4. 更新 total_size
```

**示例**：
```c
// 初始状态：memblock.memory 为空

// 步骤 1：添加第一块内存
memblock_add(0x40000000, 0x10000000);  // 256MB
// memory.regions[0] = {base: 0x40000000, size: 0x10000000}
// memory.cnt = 1
// memory.total_size = 0x10000000

// 步骤 2：添加相邻内存（会自动合并）
memblock_add(0x50000000, 0x10000000);  // 256MB
// memory.regions[0] = {base: 0x40000000, size: 0x20000000}  // 合并！
// memory.cnt = 1
// memory.total_size = 0x20000000

// 步骤 3：添加不连续内存
memblock_add(0x80000000, 0x10000000);  // 256MB
// memory.regions[0] = {base: 0x40000000, size: 0x20000000}
// memory.regions[1] = {base: 0x80000000, size: 0x10000000}
// memory.cnt = 2
// memory.total_size = 0x30000000
```

### 2. 删除内存区域 - memblock_remove()

**功能**：从 `memblock.memory` 中移除一段物理内存

```c
int memblock_remove(phys_addr_t base, phys_addr_t size)
{
    return memblock_remove_range(&memblock.memory, base, size);
}
```

**工作流程**：
```
memblock_remove(0x45000000, 0x01000000)  // 移除中间 16MB
    ↓
1. 找到包含该地址的区域
2. 如果完全包含，删除整个区域
3. 如果部分重叠，分割区域
4. 更新 total_size
```

**示例**：
```c
// 初始：memory.regions[0] = {base: 0x40000000, size: 0x20000000}

// 移除中间部分
memblock_remove(0x45000000, 0x01000000);
// memory.regions[0] = {base: 0x40000000, size: 0x05000000}  // 前半部分
// memory.regions[1] = {base: 0x46000000, size: 0x1A000000}  // 后半部分
// memory.cnt = 2
```

### 3. 预留内存 - memblock_reserve()

**功能**：将一段物理内存标记为"已预留"，添加到 `memblock.reserved` 列表

```c
int memblock_reserve(phys_addr_t base, phys_addr_t size)
{
    return memblock_add_range(&memblock.reserved, base, size, 
                              MAX_NUMNODES, 0);
}
```

**关键点**：
- `reserved` 列表与 `memory` 列表**独立管理**
- 预留的内存仍然在 `memory` 列表中，但分配器会跳过它
- 用于标记内核代码、initrd、页表等已使用的内存

**示例**：
```c
// 预留内核代码区域
memblock_reserve(__pa_symbol(_stext), _end - _stext);
// reserved.regions[0] = {base: 0x40080000, size: 0x00200000}
// reserved.cnt = 1
// reserved.total_size = 0x00200000

// memory 列表不变，但分配时会跳过这段内存
```

### 4. 内存分配 - memblock_alloc()

Memblock 提供两种分配 API：

#### A. 返回物理地址 - memblock_phys_alloc()

```c
phys_addr_t memblock_phys_alloc(phys_addr_t size, phys_addr_t align)
{
    return memblock_alloc_range_nid(size, align, 0,
                                    MEMBLOCK_ALLOC_ACCESSIBLE,
                                    NUMA_NO_NODE, false);
}
```

**工作流程**：
```
memblock_phys_alloc(0x1000, 0x1000)  // 分配 4KB，4KB 对齐
    ↓
memblock_find_in_range_node()
    ↓
1. 遍历 memory 列表，找到空闲区域
2. 检查是否与 reserved 列表重叠
3. 找到满足大小和对齐要求的地址
4. 调用 memblock_reserve() 标记为预留
5. 返回物理地址
```

#### B. 返回虚拟地址 - memblock_alloc()

```c
void *memblock_alloc(phys_addr_t size, phys_addr_t align)
{
    return memblock_alloc_internal(size, align, 0,
                                    MEMBLOCK_ALLOC_ACCESSIBLE,
                                    NUMA_NO_NODE, false);
}
```

**工作流程**：
```
memblock_alloc(0x1000, 0x1000)
    ↓
memblock_alloc_internal()
    ↓
1. 调用 memblock_phys_alloc() 获取物理地址
2. 使用 phys_to_virt() 转换为虚拟地址
3. 返回虚拟地址指针
```

**示例**：
```c
// 分配页表内存
phys_addr_t pgd_pa = memblock_phys_alloc(PAGE_SIZE, PAGE_SIZE);
// pgd_pa = 0x40100000

// 或者直接获取虚拟地址
pgd_t *pgd = memblock_alloc(PAGE_SIZE, PAGE_SIZE);
// pgd = (void *)0xffff8000080100000  // 通过线性映射访问
```

### 5. 内存查找 - for_each_mem_range()

**功能**：遍历所有可用内存区域（排除预留区域）

```c
#define for_each_mem_range(i, type_a, type_b, nid, flags, \
                          p_start, p_end, p_nid) \
    for (i = 0, __next_mem_range(&i, nid, flags, type_a, type_b, \
                                  p_start, p_end, p_nid); \
         i != (u64)ULLONG_MAX; \
         __next_mem_range(&i, nid, flags, type_a, type_b, \
                          p_start, p_end, p_nid))
```

**使用示例**：
```c
phys_addr_t start, end;
u64 i;

// 遍历所有可用内存（memory - reserved）
for_each_mem_range(i, &memblock.memory, &memblock.reserved,
                   NUMA_NO_NODE, MEMBLOCK_NONE,
                   &start, &end, NULL) {
    pr_info("Available memory: [%pa-%pa]\n", &start, &end);
}
```

---

## arm64_memblock_init() 详细分析

`arm64_memblock_init()` 是 ARM64 架构特定的 memblock 初始化函数，在 `setup_arch()` 中调用。它的主要任务是**根据 ARM64 的地址空间限制，裁剪和调整物理内存布局**。

### 函数位置

```c
// arch/arm64/mm/init.c:186
void __init arm64_memblock_init(void)
```

### 完整代码流程

#### 步骤 1: 计算线性映射区域大小

```c
s64 linear_region_size = PAGE_END - _PAGE_OFFSET(vabits_actual);
```

**作用**：
- 计算线性映射（`PAGE_OFFSET` → `PAGE_END`）可以覆盖的虚拟地址空间大小
- `vabits_actual` 是实际可用的虚拟地址位数（可能是 48 或 52）

**示例**：
```c
// 48-bit VA: PAGE_OFFSET = 0xffff000000000000, PAGE_END = 0xffff800000000000
// linear_region_size = 0x800000000000 = 128TB

// 52-bit VA: PAGE_OFFSET = 0xfff0000000000000, PAGE_END = 0xfff8000000000000
// linear_region_size = 0x800000000000 = 128TB
```

#### 步骤 2: KVM nVHE 模式特殊处理

```c
if (IS_ENABLED(CONFIG_KVM) && vabits_actual == 52 &&
    is_hyp_mode_available() && !is_kernel_in_hyp_mode()) {
    pr_info("Capping linear region to 51 bits for KVM in nVHE mode...\n");
    linear_region_size = min_t(u64, linear_region_size, BIT(51));
}
```

**作用**：
- 在 KVM nVHE（非虚拟化主机扩展）模式下，ID map 的放置可能限制线性映射
- 将线性映射区域限制为 51-bit（2TB）以确保兼容性

**原因**：
- nVHE 模式下，Hypervisor 需要自己的页表
- ID map 的位置可能随机化，占用部分地址空间
- 限制线性映射可以避免冲突

#### 步骤 3: 移除超出硬件支持的物理内存

```c
memblock_remove(1ULL << PHYS_MASK_SHIFT, ULLONG_MAX);
```

**作用**：
- `PHYS_MASK_SHIFT` 是硬件支持的物理地址位数（从 CPU ID 寄存器读取）
- 移除所有超出硬件寻址能力的物理内存

**示例**：
```c
// 如果硬件只支持 40-bit 物理地址（1TB）
// PHYS_MASK_SHIFT = 40
// 移除 [1TB, ULLONG_MAX) 的所有内存
memblock_remove(0x10000000000, ULLONG_MAX);
```

#### 步骤 4: 确定 memstart_addr（物理内存基址）

```c
memstart_addr = round_down(memblock_start_of_DRAM(),
                           ARM64_MEMSTART_ALIGN);
```

**作用**：
- `memstart_addr` 是线性映射的物理地址起点
- 对应虚拟地址 `PAGE_OFFSET`
- 对齐到 `ARM64_MEMSTART_ALIGN`（通常是 1GB）

**关系**：
```
虚拟地址 = 物理地址 - memstart_addr + PAGE_OFFSET
         = 物理地址 + (PAGE_OFFSET - memstart_addr)
```

**示例**：
```c
// DRAM 从 0x40000000 开始
// memstart_addr = round_down(0x40000000, 1GB) = 0x40000000
// PAGE_OFFSET = 0xffff000000000000
// 
// 物理地址 0x40000000 映射到虚拟地址：
// VA = 0x40000000 + (0xffff000000000000 - 0x40000000)
//    = 0xffff000040000000
```

#### 步骤 5: 检查内存是否超出线性映射范围

```c
if ((memblock_end_of_DRAM() - memstart_addr) > linear_region_size)
    pr_warn("Memory doesn't fit in the linear mapping, VA_BITS too small\n");
```

**作用**：
- 检查所有物理内存是否都能被线性映射覆盖
- 如果不能，发出警告（但不会阻止启动）

**问题场景**：
```c
// 假设：
// - linear_region_size = 128TB
// - memstart_addr = 0x40000000
// - memblock_end_of_DRAM() = 0x40000000 + 200TB
// 
// 200TB > 128TB → 警告！
// 超出部分无法通过线性映射访问
```

#### 步骤 6: 移除无法映射的物理内存

```c
memblock_remove(max_t(u64, memstart_addr + linear_region_size,
                      __pa_symbol(_end)), ULLONG_MAX);
```

**作用**：
- 移除超出线性映射范围的物理内存
- 但确保内核代码（`_end`）不被移除

**逻辑**：
```c
// 移除起点 = max(线性映射终点, 内核代码终点)
// 确保内核代码始终可访问
remove_start = max(memstart_addr + linear_region_size, __pa_symbol(_end));
memblock_remove(remove_start, ULLONG_MAX);
```

**示例**：
```c
// memstart_addr = 0x40000000
// linear_region_size = 128TB
// __pa_symbol(_end) = 0x40200000
// 
// remove_start = max(0x40000000 + 128TB, 0x40200000)
//              = 0x40000000 + 128TB
// 
// 移除 [0x40000000 + 128TB, ULLONG_MAX)
```

#### 步骤 7: 处理低端内存不足的情况

```c
if (memstart_addr + linear_region_size < memblock_end_of_DRAM()) {
    memstart_addr = round_up(memblock_end_of_DRAM() - linear_region_size,
                             ARM64_MEMSTART_ALIGN);
    memblock_remove(0, memstart_addr);
}
```

**作用**：
- 如果物理内存的**高端**超出线性映射，调整 `memstart_addr` 向上移动
- 这样可以让线性映射覆盖物理内存的**高端部分**
- 移除低端无法映射的内存

**场景**：
```c
// 初始状态：
// - DRAM: [0x0, 0x2000000000]  // 128GB
// - memstart_addr = 0x0
// - linear_region_size = 64GB
// 
// 问题：0x0 + 64GB = 64GB < 128GB（高端超出）
// 
// 调整：
// - memstart_addr = round_up(128GB - 64GB, 1GB) = 64GB
// - 移除 [0x0, 64GB)
// - 现在线性映射覆盖 [64GB, 128GB]
```

#### 步骤 8: 处理 52-bit VA 配置但硬件不支持的情况

```c
if (IS_ENABLED(CONFIG_ARM64_VA_BITS_52) && (vabits_actual != 52)) {
    memstart_addr -= _PAGE_OFFSET(vabits_actual) - _PAGE_OFFSET(52);
}
```

**作用**：
- 如果内核配置为 52-bit VA，但硬件只支持 48-bit VA
- 需要将物理内存"向上移动"到 48-bit 可寻址的范围内
- 通过减小 `memstart_addr` 实现（因为 `memstart_addr` 代表 `PAGE_OFFSET` 对应的物理地址）

**计算**：
```c
// 52-bit: PAGE_OFFSET = 0xfff0000000000000
// 48-bit: PAGE_OFFSET = 0xffff000000000000
// 
// 差值 = 0xffff000000000000 - 0xfff0000000000000
//      = 0x0001000000000000  // 256TB
// 
// memstart_addr -= 256TB
// 这样物理内存被"向上移动"到 48-bit 可寻址范围
```

#### 步骤 9: 应用 mem= 命令行参数限制

```c
if (memory_limit != PHYS_ADDR_MAX) {
    memblock_mem_limit_remove_map(memory_limit);
    memblock_add(__pa_symbol(_text), (u64)(_end - _text));
}
```

**作用**：
- 如果用户通过 `mem=4G` 等参数限制内存
- 移除超出限制的内存
- 但确保内核代码区域被重新添加（因为内核必须可访问）

**示例**：
```c
// 用户指定 mem=4G
// memory_limit = 4GB
// 
// 1. 移除 [4GB, ULLONG_MAX)
// 2. 如果内核代码在 4GB 以上，重新添加内核区域
memblock_add(__pa_symbol(_text), _end - _text);
```

#### 步骤 10: 处理 Initrd

```c
if (IS_ENABLED(CONFIG_BLK_DEV_INITRD) && phys_initrd_size) {
    u64 base = phys_initrd_start & PAGE_MASK;
    u64 size = PAGE_ALIGN(phys_initrd_start + phys_initrd_size) - base;
    
    if (WARN(base < memblock_start_of_DRAM() ||
             base + size > memblock_start_of_DRAM() + linear_region_size,
             "initrd not fully accessible...")) {
        phys_initrd_size = 0;  // 禁用 initrd
    } else {
        memblock_add(base, size);        // 确保在 memory 列表中
        memblock_clear_nomap(base, size); // 清除 NOMAP 标记
        memblock_reserve(base, size);     // 标记为预留
    }
}
```

**作用**：
- 检查 initrd 是否在线性映射范围内
- 如果不在，禁用 initrd（发出警告）
- 如果在，确保 initrd 区域：
  1. 在 `memory` 列表中（可访问）
  2. 清除 `NOMAP` 标记（需要映射）
  3. 在 `reserved` 列表中（不被分配）

#### 步骤 11: KASLR 随机化 memstart_addr

```c
if (IS_ENABLED(CONFIG_RANDOMIZE_BASE)) {
    extern u16 memstart_offset_seed;
    u64 mmfr0 = read_cpuid(ID_AA64MMFR0_EL1);
    int parange = cpuid_feature_extract_unsigned_field(
                    mmfr0, ID_AA64MMFR0_EL1_PARANGE_SHIFT);
    s64 range = linear_region_size -
                BIT(id_aa64mmfr0_parange_to_phys_shift(parange));
    
    if (memstart_offset_seed > 0 && range >= (s64)ARM64_MEMSTART_ALIGN) {
        range /= ARM64_MEMSTART_ALIGN;
        memstart_addr -= ARM64_MEMSTART_ALIGN *
                         ((range * memstart_offset_seed) >> 16);
    }
}
```

**作用**：
- 如果启用 KASLR，随机化 `memstart_addr`
- 增加地址空间布局随机性，提高安全性
- 随机化范围 = 线性映射大小 - 物理内存可寻址范围

**计算**：
```c
// 可用随机化范围
range = linear_region_size - 物理内存可寻址范围
// 
// 如果 range >= 1GB，进行随机化
// memstart_addr -= 1GB * (随机偏移)
```

#### 步骤 12: 预留内核代码和页表

```c
memblock_reserve(__pa_symbol(_stext), _end - _stext);
```

**作用**：
- 将内核代码区域标记为预留
- 防止被 memblock 分配器分配出去

#### 步骤 13: 设置 initrd 虚拟地址

```c
if (IS_ENABLED(CONFIG_BLK_DEV_INITRD) && phys_initrd_size) {
    initrd_start = __phys_to_virt(phys_initrd_start);
    initrd_end = initrd_start + phys_initrd_size;
}
```

**作用**：
- 将 initrd 的物理地址转换为虚拟地址
- 后续代码使用虚拟地址访问 initrd

#### 步骤 14: 解析设备树保留内存

```c
early_init_fdt_scan_reserved_mem();
```

**作用**：
- 从设备树（FDT）中解析 `reserved-memory` 节点
- 将这些区域添加到 `memblock.reserved` 列表

**设备树示例**：
```dts
reserved-memory {
    #address-cells = <2>;
    #size-cells = <2>;
    ranges;
    
    secure_memory: secure@50000000 {
        compatible = "shared-dma-pool";
        reg = <0x0 0x50000000 0x0 0x10000000>;  // 256MB
        no-map;
    };
};
```

#### 步骤 15: 设置 high_memory

```c
high_memory = __va(memblock_end_of_DRAM() - 1) + 1;
```

**作用**：
- `high_memory` 是线性映射中可访问的最高虚拟地址
- 用于后续内存管理子系统

---

## Memblock 与后续内存管理的关系

### 1. 为 paging_init() 提供输入

```c
// arch/arm64/mm/mmu.c:808
void __init paging_init(void)
{
    map_mem(swapper_pg_dir);  // 使用 memblock.memory 创建线性映射
    ...
}
```

**`map_mem()` 的工作**：
```c
// 遍历 memblock.memory 列表
for_each_mem_range(i, &start, &end) {
    // 为每个区域创建线性映射
    __map_memblock(pgdp, start, end, PAGE_KERNEL, flags);
}
```

### 2. 为 bootmem_init() 提供输入

```c
// arch/arm64/mm/init.c:315
void __init bootmem_init(void)
{
    min = PFN_UP(memblock_start_of_DRAM());
    max = PFN_DOWN(memblock_end_of_DRAM());
    // 使用 memblock 信息初始化 PFN 范围
    ...
}
```

### 3. 内存移交到伙伴系统

在 `mem_init()` 中，memblock 管理的所有内存会被释放到伙伴系统：

```c
void __init mem_init(void)
{
    // 将所有 memblock.memory 区域释放到伙伴系统
    memblock_free_all();
    
    // 如果未启用 CONFIG_ARCH_KEEP_MEMBLOCK，销毁 memblock 数据结构
    memblock_discard();
}
```

---

## 完整流程图

```
┌─────────────────────────────────────────────────────────────┐
│ Bootloader / FDT                                             │
│ 提供物理内存信息                                              │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 早期初始化（setup_arch 之前）                                 │
│                                                             │
│ - memblock_add() 添加 DRAM 区域                             │
│ - memblock_reserve() 预留内核代码、initrd                    │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ arm64_memblock_init()                                        │
│                                                             │
│ 1. 计算线性映射大小                                          │
│ 2. 移除超出硬件支持的物理内存                                │
│ 3. 确定 memstart_addr                                        │
│ 4. 移除无法映射的物理内存                                    │
│ 5. 处理 mem= 参数                                            │
│ 6. 处理 initrd                                               │
│ 7. KASLR 随机化                                              │
│ 8. 预留内核代码                                              │
│ 9. 解析设备树保留内存                                        │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ paging_init()                                                │
│                                                             │
│ - 使用 memblock.memory 创建线性映射                          │
│ - 所有物理内存映射到 PAGE_OFFSET 以上                        │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ bootmem_init()                                               │
│                                                             │
│ - 使用 memblock 信息初始化 PFN 范围                          │
│ - 初始化 NUMA、zone 等                                       │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ mem_init()                                                   │
│                                                             │
│ - memblock_free_all() 释放所有内存到伙伴系统                 │
│ - memblock_discard() 销毁 memblock 数据结构                  │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 伙伴系统接管内存管理                                          │
│                                                             │
│ - 所有内存由伙伴系统管理                                      │
│ - memblock 不再使用（除非 CONFIG_ARCH_KEEP_MEMBLOCK）       │
└─────────────────────────────────────────────────────────────┘
```

---

## 总结

### Memblock 的核心价值

1. **早期内存管理**：在伙伴系统初始化前提供内存分配
2. **物理内存记账**：维护系统中所有物理内存的"账本"
3. **内存布局调整**：根据架构限制裁剪和调整内存布局
4. **为后续系统铺路**：为 `paging_init()` 和 `bootmem_init()` 提供输入

### arm64_memblock_init() 的核心任务

1. **适配地址空间限制**：确保所有物理内存都能被线性映射覆盖
2. **处理硬件约束**：移除超出硬件寻址能力的物理内存
3. **支持用户配置**：处理 `mem=` 参数、KASLR 等
4. **保证关键区域可访问**：确保内核代码、initrd 等始终可访问

### 关键设计决策

- **只管理物理地址**：因为虚拟地址布局依赖物理内存信息
- **简单的数组结构**：启动早期不需要复杂的数据结构
- **自动合并区域**：减少内存碎片，提高效率
- **独立管理 reserved**：清晰区分可用和已用内存



