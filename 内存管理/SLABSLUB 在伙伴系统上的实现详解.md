# SLAB/SLUB 在伙伴系统上的实现详解

本文聚焦 “伙伴系统之上的 slab 内存管理”，以 SLUB 为主要参考，梳理对象缓存如何建立、如何向伙伴系统申请页、如何回收等。

---

## 1. 两层结构概览

| 层级 | 主要职责 | 分配粒度 | 核心数据结构 |
| --- | --- | --- | --- |
| 伙伴系统 (`alloc_pages/__free_pages`) | 按 zone/order 管理物理页 | 2^order × PAGE_SIZE | `struct zone` + `free_area[]` |
| SLAB/SLUB (`kmem_cache_alloc/kmalloc`) | 管理小对象缓存 | 对象（几十字节~几 KB） | `struct kmem_cache` + `struct page` (作为 slab) |

伙伴系统解决页级资源；SLAB/SLUB 在其上切分对象、缓存重用。

---

## 2. SLUB 核心概念

1. **`kmem_cache`**：每种对象类型对应一个缓存，描述对象大小、对齐、构造/析构函数等。常见的有 `kmalloc-64`、`inode_cache` 等。

2. **Slab 与 Page**：在 SLUB 中，“slab = 承载多个对象的一块连续页（通常 order=0）”。slab 的生命周期由 `struct page` 描述：
   - `page->slab_cache` 指向所属 `kmem_cache`。
   - `page->freelist` 存放当前 slab 的空闲对象链表。
   - `page->inuse` / `objects` 记录已分配/总对象数。

3. **per-CPU 缓存 (`kmem_cache_cpu`)**：每 CPU 上有当前 active slab，以及其 freelist 指针，降低全局锁竞争。

4. **部分 slab 链表**：当 per-CPU slab 用完或过满时，slab 会进入 `kmem_cache_node->partial` 链表等，由全局管理。

---

## 3. 初始化流程

### 3.1 `kmem_cache_create()`
创建专用缓存（或 kmalloc 系列缓存）时：
- 计算对象大小、对齐，确定每 slab 需多少页、每 slab 容纳多少对象；
- 设置 flags（如 SLAB_POISON、KASAN、NUMA 等）。

### 3.2 `kmem_cache_alloc()` / `kmalloc()`
- 选择目标缓存（`kmalloc_slab(size)` 映射到 `kmalloc-128` 等）。
- 调 `__slab_alloc()`，优先从 per-CPU slab freelist 取对象，失败则拉取/新建 slab。

---

## 4. 向伙伴系统申请页（new slab）

当缓存需要新 slab 时：

```text
new_slab_objects()
 └─ allocate_slab()
     └─ alloc_slab_page()
         └─ alloc_pages_node(nid, gfp, order)
             → 伙伴系统分配 2^order 张页，返回 struct page *
```

随后：
1. `page->slab_cache` = `kmem_cache`；`page->flags` 标记 SLAB 狀态。
2. `setup_object()` 按对象大小在页内切分对象，建立 freelist（对象头部存储“next”指针）。
3. Slab 加入 per-CPU 或 node partial list，供后续分配使用。

---

## 5. 对象分配与释放

### 5.1 分配
- `__slab_alloc()` 从当前 per-CPU slab freelist 弹出一个对象；
- 若当前 slab 空，则：
  - 从 `kmem_cache_node->partial` 获取 slab；
  - 或调用 `new_slab_objects()` 从伙伴系统新申请；
- 返回对象指针（通常位于 slab 页内部）。

### 5.2 释放
- `kmem_cache_free()` / `kfree()`：通过 `virt_to_head_page(obj)` 找到 slab 的 `struct page`；
- 把对象链回 slab freelist，更新 `page->inuse`；
- 若某 slab 全空且 partial list 过多，可调用 `discard_slab()` → `__free_pages()`，把页归还给伙伴系统。

---

## 6. 数据结构关系

```
struct kmem_cache
 ├─ struct kmem_cache_cpu (per-CPU)
 │   └─ page *cpu_slab  (当前 slab)
 │       └─ void *freelist
 ├─ struct kmem_cache_node (per node)
 │   └─ partial/full/empty slab list
 └─ node 指针、object size、flags 等

struct page (作为 slab)
 ├─ struct kmem_cache *slab_cache
 ├─ void *freelist  (空闲对象链表头)
 ├─ unsigned inuse / objects
 └─ page->lru / page->list  加入 partial 链等

struct zone (伙伴系统)
 └─ free_area[MAX_ORDER]  (每阶空闲页链表)
     └─ struct page * (普通页的 page 描述符)
```

---

## 7. 与伙伴系统的互动

- **获取新 slab**：SLUB 通过 `alloc_pages()` 从 zone 获取页 → 切成对象；
- **释放空 slab**：`__free_pages()` 将整个 slab 归还伙伴系统，便于合并回更大块；
- **迁移类型**：SLUB 在申请页时会使用特定 gfp_flags（如 `__GFP_HIGHMEM`) 或 MIGRATE 类型，确保特定缓存来自合适 zone；
- **NUMA 感知**：`alloc_slab_page()` 会在对应 node 调用 `alloc_pages_node()`，实现本地性；不足时可以 fallback 到其他节点。

---

## 8. 调试 / 内省工具

- `/sys/kernel/slab/`：查看各 `kmem_cache` 的使用情况。
- `/proc/slabinfo`：传统文本接口，显示对象数、slab 数、partial 数等。
- `slab_debug` 选项：可启用 red-zone、poison、验证 freelist 等。
- `slub_debug=FZPU`（boot 参数）等：按需启用 Free/Zero/Poison/use-after-free 检测。
- `kmemleak` / `kmem_cache_shrink()`：与 slab 紧密相关。

---

## 9. 调用链示例（`kmalloc(128)`）

```
kmalloc(128)
  └─ kmalloc_slab(128) → kmalloc-128 cache
      └─ __slab_alloc(cache)
          ├─ 从当前 CPU 的 slab freelist 获取 object（若有）
          ├─ 否则从 partial list 取新的 slab
          └─ partial 也空时：
              └─ new_slab_objects()
                  └─ allocate_slab()
                      └─ alloc_slab_page()
                          └─ alloc_pages(GFP_KERNEL, order)
```

释放路径逆向：`kfree(obj)` → 找到 slab page → 放回 freelist → slab 空则或许释放回伙伴系统。

---

## 10. 小结

- 伙伴系统负责“页”的合并/拆分；SLUB 在页内切分“对象”并缓存重用。
- `struct page` 担任“slab 描述符”的角色，通过附加字段与 `kmem_cache` 关联。
- `kmem_cache` 管理 per-CPU 和 per-node 的 slab 列表，实现低锁争和 NUMA 感知。
- 新 slab 的来源、空 slab 的回收都依赖伙伴系统，从而形成层次化内存管理：  

```
memblock → 伙伴系统（页级） → SLUB（对象级） → 内核/驱动使用
```

这样既兼顾了页级内存的全局合并能力，又提供了高效的小对象分配接口。


