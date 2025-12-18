# 非 reserved 区域如何映射到各个 zone（ARM64 启动流程详解）

本文聚焦 “memblock 中的非 reserved 区域如何被划分并归属到各个 zone” 的细节。结合 ARM64 的启动流程，逐步分析这一组织方式。

---

## 1. 核心背景：zone 的作用

Linux 伙伴系统按 **zone**（ZONE_DMA / ZONE_DMA32 / ZONE_NORMAL / ZONE_MOVABLE 等）将物理内存切分成多个逻辑区。每个 zone 有独立的 free list、防止某些设备/子系统抢占全部内存、满足 DMA/NUMA 约束等。

要让伙伴系统正确管理内存，需要解决两个问题：

1. **zone 范围**：每个 zone 覆盖哪些 PFN。
2. **struct page → zone 的映射**：任意 PFN 被初始化成 `struct page` 后，要能快速定位其所在 zone。

这两点都在 `bootmem_init()` 之后的阶段完成。

---

## 2. 描述阶段（memblock 自身并不区分 zone）

在 memblock 阶段：

```text
memblock.memory:    记录所有可用物理内存（多个 [base, size) 区间）
memblock.reserved:  记录已预留的区间（内核映像、initrd、crashkernel 等）
```

此时 memblock 只知道哪些物理地址可用/保留，并不关心 zone。它的主要职责是：

- 保障早期分配成功；
- 允许 “按物理地址” 对内存做裁剪或预留。

---

## 3. 确定 zone 范围：`zone_sizes_init()` → `free_area_init()`

文件：`arch/arm64/mm/init.c`

```c
static void __init zone_sizes_init(void)
{
    unsigned long max_zone_pfns[MAX_NR_ZONES] = {0};

#ifdef CONFIG_ZONE_DMA
    max_zone_pfns[ZONE_DMA]   = PFN_DOWN(arm64_dma_phys_limit);
#endif
#ifdef CONFIG_ZONE_DMA32
    max_zone_pfns[ZONE_DMA32] = PFN_DOWN(dma32_phys_limit);
#endif
    max_zone_pfns[ZONE_NORMAL] = max_pfn;

    free_area_init(max_zone_pfns);
}
```

要点：

- 根据平台约束（如 `zone_dma_limit`、`DMA_BIT_MASK(32)`）算出每个 zone 的顶部 PFN。
- `max_pfn` 来源于 `memblock_end_of_DRAM()`（可用物理内存的最高 PFN）。
- `max_zone_pfns[]` 被传给 `free_area_init()`。

### `free_area_init()` 内部（`mm/mm_init.c`）

1. 对每个 zone 设置 `arch_zone_lowest_possible_pfn[zone]` / `arch_zone_highest_possible_pfn[zone]`。
2. 为每个 NUMA 节点创建 `pg_data_t`、`struct zone`。
3. zone 的起始/结束 PFN、`zone->spanned_pages`/`zone_start_pfn` 在此时敲定。

到此为止，虽然还没给伙伴系统任何实际物理页，但**每个 zone 的 PFN 范围已经被记载下来**。

---

## 4. 初始化 `struct page`：`sparse_init()` / `vmemmap_populate()`

`sparse_init()`（`mm/sparse.c`）会：

1. 遍历所有 “present” 内存区间（来自 memblock），为每个 section 分配 `struct page` 数组。
2. 在 `struct page` 中填充所属 node 信息，并依据 zone 范围设置 `page->flags` 的 zone bits。
   - 对于 SPARSEMEM_VMEMMAP 模型，`vmemmap_populate()` 建立虚拟映射，使得 `struct page` 可以通过 `PFN → vmemmap` 公式计算。
3. 此阶段不管页是否 reserved，只是单纯初始化 describe 页面状态。

结果：任何 PFN 对应的 `struct page` 都可以通过 `page_zone(page)` 得到所属 zone。

---

## 5. 释放非 reserved 区域：`memblock_free_all()`

函数：`mm/memblock.c`

流程：

1. 遍历 `memblock.memory` 的所有区间。
2. 调 `memblock_is_region_reserved()`，排除 reserved 占用部分。
3. 对剩余区间调用 `__free_pages_memory(start, end)`，逐步拆分为不超过 `MAX_ORDER` 的块。
4. 每块调用 `__free_pages_boot_core(page, order)` → `__free_pages(page, order)`。

在 `__free_pages()` 内部：

```c
struct zone *zone = page_zone(page);
spin_lock(&zone->lock);
// 将该块加入 zone->free_area[order] 链表
...
```

由于 `struct page` 早已知道自己的 zone，伙伴系统自然把该块放进对应 zone 的 free list。

---

## 6. 整体链路概述

```
memblock（记录物理区间，不区分 zone）
    ↓
zone_sizes_init/free_area_init（确定各 zone PFN 范围）
    ↓
sparse_init/vmemmap_populate（初始化 struct page，记住 zone/nid）
    ↓
memblock_free_all（把非 reserved 页释放到伙伴系统）
    ↓
__free_pages → page_zone → zone->free_area[...] （按 zone 归类，建立 free list）
```

因此，“把整段非 reserved 内存放到 zone 中” 的本质是：
- zone 的 PFN 边界早已写入全局结构；
- `struct page` 在被创建时就绑定了它属于哪个 zone；
- `memblock_free_all()` 释放时，通过 `page_zone()` 自动落到正确 zone 的 free list。

---

## 7. 调试 / 验证建议

1. **查看 zone 范围**：`cat /proc/zoneinfo` 中的 `zone_start_pfn`、`spanned` 字段。
2. **确认 reserved 未进入伙伴系统**：`/proc/buddyinfo` 中看不到 reserved 区域的页；memblock debug (`memblock=debug`) 可打印被保留的区间。
3. **验证 struct page 的 zone**：在内核里 `pr_info("%p belongs to zone %s\n", page, page_zone(page)->name);` 可以直观验证。
4. **边界问题排查**：若 zone 切分错误，可能导致某些 PFN 未被任何 zone 覆盖，可借助 `pfn_valid()`、`memblock_dump_all()` 定位。

---

## 8. 小结

- memblock 只负责记录物理地址区间，并不区分 zone；
- `zone_sizes_init()` / `free_area_init()` 定义各 zone 的 PFN 范围；
- `sparse_init()` / `vmemmap` 将每个 PFN 映射到一个 `struct page`，并设置其 zone；
- `memblock_free_all()` 释放非 reserved 区域时，伙伴系统通过 `page_zone()` 自动把页归入正确 zone 的 free list。

如此，非 reserved 区域即使是一整个连续内存块，也会在 “释放到伙伴系统” 的过程中被精确地分散到各个 zone 中管理。


