# bootmem 之后如何过渡到伙伴系统（超详细版）

本文在已有概述基础上，补充每个阶段的调用栈、关键数据结构、涉及文件以及调试要点，帮助深入理解 ARM64 启动后从 memblock 到伙伴系统的全流程。

## 1. `bootmem_init()` 完成了什么

（参见前一份文档）此阶段只是：

- 依据 `memblock_start_of_DRAM()` / `memblock_end_of_DRAM()` 计算 PFN 范围。
- 调 `arch_numa_init()` 让 NUMA 节点和 `node_online_map` 就绪。
- 预留 HugeTLB / KVM / CMA 等特殊区域。
- 调 `sparse_init()` 为 present 的物理区间分配 `struct page`/`mem_section` 元数据。
- 调 `zone_sizes_init()`（→ `free_area_init()`）配置各 zone 的 PFN 边界。

> 注意：**此时伙伴系统还没有 “free page”**，所有页面仍由 memblock 管掌。

## 2. `mem_init()`：把物理页释放给伙伴系统

入口：`arch/arm64/mm/init.c:361`

关键调用：

```c
void __init mem_init(void)
{
	...
	memblock_free_all();   // ← 真正把非保留页交给伙伴系统
	...
}
```

`memblock_free_all()`（`mm/memblock.c`）：

1. 遍历 `memblock.memory`，跳过 `memblock.reserved` 覆盖的区间。
2. 对每个可用区间调用 `__free_pages_memory()` → `__free_pages()`。
3. `__free_pages()` 把页按照 zone/node 信息（此前 `zone_sizes_init()` 已设置）插入伙伴系统的 free list。

至此：

- `struct page` 元数据已存在（来自 `sparse_init()`）。
- 物理页被标记为 `PageBuddy`，伙伴系统接管它们。
- memblock 仍可查询/调试（尚未丢弃），但不再负责分配。

## 3. `mm_core_init()`：伙伴系统全面上线

入口：`mm/mm_init.c:2633`，紧随 `start_kernel()` 中的 `mm_init()` 之后。

主要步骤：

| 步骤 | 作用 | 文件 / 函数 |
| --- | --- | --- |
| `build_all_zonelists()` | 构建 zonelist，决定伙伴分配器扫描 zone 的顺序（NUMA + MIGRATE 类型） | `mm/page_alloc.c` |
| `page_alloc_init_cpuhp()` | 注册 CPU 热插拔通知，使伙伴系统在 CPU 上/下线时调整 per-CPU 变量 | `mm/page_alloc.c` |
| `page_ext_init_flatmem()` / `mem_debugging_and_hardening_init()` | 初始化 page extension（如 page_owner、kmsan）、KASAN/KCSAN 硬化开关 | `mm/page_ext.c`, `mm/memory_debug.c` |
| `kfence_alloc_pool_and_metadata()` | 使用伙伴分配器申请 KFENCE 池并准备元数据 | `mm/kfence/core.c` |
| `mem_init()`（mm 层） | 填写全局统计、打印 `assigned bootmem` 等信息 | `mm/mm_init.c` |
| `kmem_cache_init()`、`kmemleak_init()` 等 | slab/slub/slob、kmemleak 等都依赖伙伴分配器，必须在其可用后运行 | `mm/slab.c`, `mm/kmemleak.c` |

到此，伙伴系统已可以为 slab、vmalloc、用户态匿名页等提供服务。

## 4. memblock 的收尾

- 若 **未** 配置 `CONFIG_ARCH_KEEP_MEMBLOCK`：

  `memblock_discard()` 会在稍后释放 `memblock.memory/reserved` 数组本身所占内存（通过 `memblock_free_late()` 或 `kfree()`），并将 `memblock_memory` 置空，意味着 memblock 生命周期结束。

- 若 **启用** `ARCH_KEEP_MEMBLOCK`：

  memblock 数据结构会一直保留在内核中，可供 `/sys/kernel/debug/memblock` 或热插拔场景复用。

## 5. 调用链总结

```
bootmem_init()
    ├─ sparse_init()        -> 分配 struct page / mem_section
    └─ zone_sizes_init()    -> 配置 zone 边界

mem_init()
    └─ memblock_free_all()  -> 释放所有非 reserved 页到伙伴系统

mm_core_init()
    ├─ build_all_zonelists()
    ├─ page_alloc_init_cpuhp()
    ├─ slab/kfence/etc 初始化
    └─ memblock_discard() (可选)

伙伴系统完全接管物理内存
```

## 6. 关键接口速览

| 函数 | 文件 | 说明 |
| --- | --- | --- |
| `memblock_free_all()` | `mm/memblock.c` | 把 memblock 可用内存交给伙伴系统 |
| `__free_pages_memory()` | `mm/memblock.c` | 逐页调用 `__free_pages()` |
| `__free_pages()` | `mm/page_alloc.c` | 把页面插入伙伴系统 free list |
| `build_all_zonelists()` | `mm/page_alloc.c` | 构建 zonelist（NUMA/zone 选择策略） |
| `memblock_discard()` | `mm/memblock.c` | 在无需保留 memblock 时释放其元数据 |
| `memblock_dump_all()` | `mm/memblock.c` | 全面打印 memblock 状态，常用于 debug 启动阶段 |
| `vmemmap_populate_*()` | `mm/sparse-vmemmap.c` | 初始化 `struct page` 所在的 vmemmap 映射（若启用 SPARSEMEM_VMEMMAP） |

通过上述步骤，内核从 memblock 的 “早期物理内存账本” 无缝过渡到伙伴系统，实现常规内存管理。

## 7. 调试与验证建议

1. **确认 memblock 阶段**  
   - `bootmem_init()` 之后、`mem_init()` 之前，用 `memblock_dump_all()`（或 `echo dump_all > /sys/kernel/debug/memblock`）查看可用/保留区域，验证 memblock 正确裁剪了线性映射范围、initrd、crashkernel 等。

2. **确认伙伴系统接管**  
   - `memblock_free_all()` 之后，`/proc/zoneinfo`、`/proc/buddyinfo` 即可看到各 zone 的页统计，这表明伙伴 free list 已建立。

3. **zonelist 顺序**  
   - 开启 `CONFIG_NUMA` 后，可通过 `cat /proc/zoneinfo` 查看 `nr_zone_inactive_anon` 等字段，判断 `build_all_zonelists()` 是否按预期顺序配置。

4. **memblock 生命周期**  
   - 若 `CONFIG_ARCH_KEEP_MEMBLOCK=n`，`grep -R memblock_discard` 确认释放时机，避免在 memblock 被丢弃后还访问其数据结构。

5. **异常排查**  
   - 若看到 Early allocation 失败，可在 `memblock_alloc_range_nid()` 内部加 `memblock_debug=1`（内核命令行）来追踪所有 memblock 分配路径。

