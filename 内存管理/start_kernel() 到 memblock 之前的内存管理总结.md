# start_kernel() 到 memblock 之前的内存管理总结

## 一、概述

本文档总结从 `start_kernel()` 开始到 `arm64_memblock_init()` 之前的所有内存管理相关初始化步骤。这个阶段是**内存管理基础设施的准备阶段**，为后续的 memblock 和 buddy system 初始化奠定基础。

---

## 二、调用顺序

```
start_kernel()
  ├─ page_address_init()              // 初始化高内存页地址映射哈希表，在arm32中使用，因为虚拟地址空间不够，在arm64中基本不适用了
  └─ setup_arch()
      ├─ setup_initial_init_mm()      // 设置初始内存管理结构
      ├─ early_fixmap_init()           // 初始化 fixmap 区域页表
      ├─ early_ioremap_init()         // 初始化 early_ioremap
      └─ setup_machine_fdt()          // 映射 FDT（使用 fixmap）
          └─ fixmap_remap_fdt()       // 实际映射 FDT
```

**注意**：`arm64_memblock_init()` 在 `setup_arch()` 中调用，是 memblock 的初始化入口。

---

## 三、详细分析

### 3.1 `page_address_init()` - 高内存页地址映射初始化

**位置**: `init/main.c:923` → `mm/highmem.c:815`

**作用**：初始化高内存（HIGHMEM）页地址映射的哈希表。

**代码**：

```c
void __init page_address_init(void)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(page_address_htable); i++) {
        INIT_LIST_HEAD(&page_address_htable[i].lh);
        spin_lock_init(&page_address_htable[i].lock);
    }
}
```

**关键理解**：

- **哈希表结构**：`page_address_htable[1<<PA_HASH_ORDER]`（默认 256 个桶）
- **用途**：在启用 HIGHMEM 时，将高内存页映射到虚拟地址
- **时机**：在 `setup_arch()` 之前初始化，为后续高内存操作做准备
- **arm64 注意**：arm64 通常不使用 HIGHMEM（64 位地址空间足够大），但代码仍然初始化

**数据结构**：
```c
static struct page_address_slot {
    struct list_head lh;        // 链表头
    spinlock_t lock;             // 自旋锁
} ____cacheline_aligned_in_smp page_address_htable[1<<PA_HASH_ORDER];
```

---

### 3.2 `setup_initial_init_mm()` - 初始内存管理结构设置

**位置**: `arch/arm64/kernel/setup.c:284` → `mm/init-mm.c:50`

**作用**：设置内核初始内存管理结构 `init_mm` 的代码段边界。

**代码**：
```c
void setup_initial_init_mm(void *start_code, void *end_code,
                           void *end_data, void *brk)
{
    init_mm.start_code = (unsigned long)start_code;    // _stext
    init_mm.end_code = (unsigned long)end_code;        // _etext
    init_mm.end_data = (unsigned long)end_data;         // _edata
    init_mm.brk = (unsigned long)brk;                  // _end
}
```

**调用**：
```c
setup_initial_init_mm(_stext, _etext, _edata, _end);
```

**关键理解**：
- **`init_mm`**：内核的初始内存管理结构（`struct mm_struct`）
- **作用**：记录内核代码段、数据段的边界
- **用途**：用于内存管理、页表管理、VMA 管理等
- **时机**：在 `setup_arch()` 最开始调用，为后续页表操作提供基础

**`init_mm` 结构**：
```c
struct mm_struct init_mm = {
    .mm_mt      = MTREE_INIT_EXT(...),
    .pgd        = swapper_pg_dir,        // 内核页表根
    .mm_users   = ATOMIC_INIT(2),
    .mm_count   = ATOMIC_INIT(1),
    // ...
};
```

---

### 3.3 `early_fixmap_init()` - Fixmap 区域页表初始化

**位置**: `arch/arm64/kernel/setup.c:290` → `arch/arm64/mm/fixmap.c`

**作用**：初始化 fixmap 区域的页表结构，为固定虚拟地址映射做准备。

**关键理解**：
- **Fixmap**：固定虚拟地址映射区域，用于映射固定用途的物理地址
- **用途**：FDT、early_ioremap、earlycon、页表创建等
- **页表结构**：使用静态分配的页表（`bm_pmd`、`bm_pud`、`bm_p4d`）

**初始化过程**：
1. **初始化 P4D**：`early_fixmap_init_p4d()` → 设置 `bm_p4d`
2. **初始化 PUD**：`early_fixmap_init_pud()` → 设置 `bm_pud`
3. **初始化 PMD**：`early_fixmap_init_pmd()` → 设置 `bm_pmd`
4. **初始化 PTE**：`early_fixmap_init_pte()` → 按需分配 PTE 表

**代码位置**：
```c
static void __init early_fixmap_init_pte(pmd_t *pmdp, unsigned long addr)
{
    pmd_t pmd = READ_ONCE(*pmdp);
    pte_t *ptep;

    if (pmd_none(pmd)) {
        ptep = bm_pte[BM_PTE_TABLE_IDX(addr)];
        __pmd_populate(pmdp, __pa_symbol(ptep), PMD_TYPE_TABLE);
    }
}
```

**Fixmap 区域**：
- **虚拟地址范围**：`FIXADDR_START` ~ `FIXADDR_TOP`
- **包含内容**：
  - `FIX_FDT`：FDT 映射
  - `FIX_BTMAP_BEGIN` ~ `FIX_BTMAP_END`：early_ioremap slot
  - `FIX_EARLYCON_MEM_BASE`：earlycon 映射
  - 其他固定映射

---

### 3.4 `early_ioremap_init()` - Early Ioremap 初始化

**位置**: `arch/arm64/kernel/setup.c:291` → `arch/arm64/mm/ioremap.c:47` → `mm/early_ioremap.c:71`

**作用**：初始化 early_ioremap 的虚拟地址槽位。

**代码**：
```c
void __init early_ioremap_init(void)
{
    early_ioremap_setup();
}
```

**`early_ioremap_setup()`**：
```c
void __init early_ioremap_setup(void)
{
    int i;

    for (i = 0; i < FIX_BTMAPS_SLOTS; i++) {
        WARN_ON_ONCE(prev_map[i]);
        slot_virt[i] = __fix_to_virt(FIX_BTMAP_BEGIN - NR_FIX_BTMAPS*i);
    }
}
```

**关键理解**：
- **Slot 机制**：7 个 slot，每个 slot 可映射最多 256KB
- **虚拟地址计算**：`slot_virt[i] = __fix_to_virt(FIX_BTMAP_BEGIN - NR_FIX_BTMAPS*i)`
- **用途**：在 `ioremap()` 可用之前，临时映射 I/O 内存
- **依赖**：必须在 `early_fixmap_init()` 之后调用

**数据结构**：
```c
static void __iomem *prev_map[FIX_BTMAPS_SLOTS];      // 每个 slot 的映射地址
static unsigned long prev_size[FIX_BTMAPS_SLOTS];      // 每个 slot 的映射大小
static unsigned long slot_virt[FIX_BTMAPS_SLOTS];     // 每个 slot 的虚拟地址
```

---

### 3.5 `setup_machine_fdt()` - FDT 映射

**位置**: `arch/arm64/kernel/setup.c:293` → `arch/arm64/mm/fixmap.c:134`

**作用**：映射设备树（FDT），使其在内核地址空间中可访问。

**代码流程**：
```c
static void __init setup_machine_fdt(phys_addr_t dt_phys)
{
    int size;
    void *dt_virt = fixmap_remap_fdt(dt_phys, &size, PAGE_KERNEL);

    if (dt_virt)
        memblock_reserve(dt_phys, size);

    if (!early_init_dt_scan(dt_virt, dt_phys)) {
        // 错误处理
    }

    /* Early fixups are done, map the FDT as read-only now */
    fixmap_remap_fdt(dt_phys, &size, PAGE_KERNEL_RO);
}
```

**`fixmap_remap_fdt()` 关键步骤**：
1. **计算虚拟地址**：`dt_virt_base = __fix_to_virt(FIX_FDT)`
2. **映射第一页**：读取 FDT 头部，获取总大小
3. **验证 FDT**：检查 magic number 和大小限制（最大 2MB）
4. **映射整个 FDT**：如果超过一页，映射整个 FDT

**关键理解**：
- **使用 fixmap**：FDT 使用固定的 fixmap 条目 `FIX_FDT`（不使用 slot）
- **两阶段映射**：先映射第一页读取头部，再映射整个 FDT
- **保护变化**：初始为 `PAGE_KERNEL`（可读写），扫描后改为 `PAGE_KERNEL_RO`（只读）
- **memblock 预留**：调用 `memblock_reserve()` 预留 FDT 占用的物理内存

**映射大小限制**：
- **最大 2MB**：`MAX_FDT_SIZE = SZ_2M`
- **虚拟地址预留**：513 页（2MB + 4KB）

---

## 四、内存管理初始化顺序的意义

### 4.1 初始化顺序

```
1. page_address_init()
   └─ 初始化高内存页地址映射哈希表（为 HIGHMEM 做准备）

2. setup_initial_init_mm()
   └─ 设置 init_mm 的代码段边界（为页表管理提供基础）

3. early_fixmap_init()
   └─ 初始化 fixmap 区域页表（为固定映射提供页表结构）

4. early_ioremap_init()
   └─ 初始化 early_ioremap slot（为临时 I/O 映射做准备）

5. setup_machine_fdt()
   └─ 映射 FDT（使用 fixmap，为设备树解析提供基础）
```

### 4.2 依赖关系

```
page_address_init()
  └─ 独立初始化，不依赖其他

setup_initial_init_mm()
  └─ 独立初始化，不依赖其他

early_fixmap_init()
  └─ 依赖：setup_initial_init_mm()（需要 init_mm.pgd）

early_ioremap_init()
  └─ 依赖：early_fixmap_init()（需要使用 fixmap 区域）

setup_machine_fdt()
  └─ 依赖：early_fixmap_init()（需要使用 FIX_FDT）
```

### 4.3 设计目的

1. **基础设施准备**：在 memblock 初始化之前，建立基本的页表结构和映射机制
2. **固定映射**：fixmap 提供固定虚拟地址，用于 FDT、early_ioremap 等
3. **临时映射**：early_ioremap 提供临时 I/O 内存映射能力
4. **设备树访问**：FDT 映射使内核能够解析设备树，获取内存信息

---

## 五、关键数据结构

### 5.1 `init_mm` - 初始内存管理结构

```c
struct mm_struct init_mm = {
    .mm_mt      = MTREE_INIT_EXT(...),      // VMA 树
    .pgd        = swapper_pg_dir,            // 内核页表根
    .mm_users   = ATOMIC_INIT(2),           // 引用计数
    .mm_count   = ATOMIC_INIT(1),
    .start_code = (unsigned long)_stext,    // 代码段起始
    .end_code   = (unsigned long)_etext,    // 代码段结束
    .end_data   = (unsigned long)_edata,    // 数据段结束
    .brk        = (unsigned long)_end,      // 堆结束
    // ...
};
```

### 5.2 Fixmap 页表结构

```c
// 静态分配的页表（在链接时分配）
static pte_t bm_pte[PTRS_PER_PTE] __page_aligned_bss;
static pmd_t bm_pmd[PTRS_PER_PMD] __page_aligned_bss __maybe_unused;
static pud_t bm_pud[PTRS_PER_PUD] __page_aligned_bss __maybe_unused;
static p4d_t bm_p4d[PTRS_PER_P4D] __page_aligned_bss __maybe_unused;
```

### 5.3 Early Ioremap 数据结构

```c
static void __iomem *prev_map[FIX_BTMAPS_SLOTS];      // 7 个 slot
static unsigned long prev_size[FIX_BTMAPS_SLOTS];
static unsigned long slot_virt[FIX_BTMAPS_SLOTS];
```

---

## 六、与 memblock 的关系

### 6.1 在 memblock 之前的作用

- **页表准备**：`early_fixmap_init()` 为 fixmap 建立页表结构
- **FDT 映射**：`setup_machine_fdt()` 映射 FDT，使内核能够读取设备树
- **临时映射**：`early_ioremap_init()` 提供临时 I/O 映射能力

### 6.2 memblock 初始化后的变化

- **`arm64_memblock_init()`**：初始化 memblock，扫描设备树获取内存信息
- **`paging_init()`**：建立完整的页表映射（线性映射等）
- **`bootmem_init()`**：初始化 bootmem，为 buddy system 做准备

### 6.3 过渡阶段

这个阶段是**从汇编启动代码到 C 语言内存管理的过渡**：
- **汇编阶段**：使用 identity mapping 和简单的页表
- **C 语言阶段**：建立 fixmap、early_ioremap 等基础设施
- **memblock 阶段**：开始使用 memblock 管理物理内存

---

## 七、总结

### 7.1 核心要点

1. **`page_address_init()`**：初始化高内存页地址映射哈希表（arm64 通常不使用）
2. **`setup_initial_init_mm()`**：设置 `init_mm` 的代码段边界
3. **`early_fixmap_init()`**：初始化 fixmap 区域页表，为固定映射提供基础
4. **`early_ioremap_init()`**：初始化 early_ioremap slot，为临时 I/O 映射做准备
5. **`setup_machine_fdt()`**：映射 FDT，使内核能够解析设备树

### 7.2 设计目的

- **基础设施准备**：在 memblock 之前建立基本的页表结构和映射机制
- **设备树访问**：映射 FDT，使内核能够读取内存信息
- **临时映射能力**：提供 early_ioremap，支持早期 I/O 操作

### 7.3 关键区别

- **Fixmap**：固定虚拟地址映射，用于 FDT、early_ioremap 等
- **Early Ioremap**：临时 I/O 内存映射，使用 slot 机制
- **FDT 映射**：使用 fixmap 的固定条目，不使用 slot

---

**核心理解**：从 `start_kernel()` 到 `memblock` 初始化之前，内核主要完成**内存管理基础设施的准备**，包括页表结构初始化、fixmap 设置、early_ioremap 准备和 FDT 映射。这些步骤为后续的 memblock 和 buddy system 初始化奠定基础。

