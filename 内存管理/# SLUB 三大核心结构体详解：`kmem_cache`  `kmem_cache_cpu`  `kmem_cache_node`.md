# SLUB 三大核心结构体详解：`kmem_cache` / `kmem_cache_cpu` / `kmem_cache_node`

面向 SLUB（即 `mm/slub.c`）的调用者，理解这三个结构体能把“对象规格”“CPU 快速路径”“NUMA 节点共享池”串成一个故事。下面按层次展开每个成员的职责。

---

## 1. `struct kmem_cache` —— Cache 级别的“总控面板”

路径：`mm/slab.h`

| 成员 | 作用 |
| --- | --- |
| `struct kmem_cache_cpu __percpu *cpu_slab` | 指向所有 CPU 的 per-CPU 状态入口。`slab_alloc_node()`/`do_slab_free()` 都要先拿对应 CPU 的 `kmem_cache_cpu`。 |
| `slab_flags_t flags` | Cache 标志，决定调试/安全/对齐策略（如 `SLAB_HWCACHE_ALIGN`、`SLAB_TYPESAFE_BY_RCU`）。 |
| `unsigned long min_partial` | 每个 node 期望保留的最小 partial slab 数，避免频繁向伙伴系统要 slab。 |
| `unsigned int size` | 单个分配对象在 slab 中的大小（含必要的内部 metadata）。 |
| `unsigned int object_size` | 提供给调用者的“真实”对象大小（不含内部 metadata）。 |
| `struct reciprocal_value reciprocal_size` | `size` 的倒数，用于快速求模/除法（如计算对象在 slab 中的索引）。 |
| `unsigned int offset` | freelist 指针在对象中的偏移，配合硬化/随机化 freelist 时调整指针位置。 |
| `unsigned int align` | 对齐约束。 |
| `unsigned int red_left_pad` | 调试红区需要的左侧 padding（`CONFIG_SLUB_DEBUG` 等）。 |
| `unsigned int inuse` | 对象头部到 metadata 的偏移，便于构造函数/调试访问。 |
| `const char *name`/`struct list_head list` | Cache 名称和在全局 `slab_caches` 链表中的挂载点，供 `/proc/slabinfo` 等使用。 |
| `struct kmem_cache_order_objects oo` | 默认 slab 配置，编码“页阶 (order)”和“该 slab 可切出的对象数”。决定向伙伴系统申请的页大小。 |
| `struct kmem_cache_order_objects min` | fallback 配置：默认 order 分配失败时退化到更小 order。 |
| `gfp_t allocflags` | 每次向伙伴系统要 slab 时附带的 GFP 标志（叠加调用者 flag）。 |
| `int refcount` | Cache 引用计数，控制销毁流程。 |
| `void (*ctor)(void *obj)` | 可选构造函数，新分配的对象会调用它做初始化。 |
| `#ifdef CONFIG_SLUB_CPU_PARTIAL`<br>`unsigned int cpu_partial` | 以“对象数”配置每 CPU partial 链表里最多缓存多少对象。 |
| `unsigned int cpu_partial_slabs` | 由 `cpu_partial` 派生而来（假设 partial slab 半满），真正限制“链表里最多允许多少个 slab”。 |
| `#ifdef CONFIG_NUMA`<br>`unsigned int remote_node_defrag_ratio` | 当本地 node 内存紧张时，可以从远程 node 借 slab 的比例。 |
| 其他按配置扩展的字段 | `kobj`（sysfs）、`random/random_seq`（freelist hardening）、`kasan_info`、`useroffset/usersize`（HARDENED_USERCOPY）等。 |
| `struct kmem_cache_node *node[MAX_NUMNODES]`（结构体末尾） | 指向每个 NUMA node 的 per-node 管理结构，后者保存 partial/full 链表、统计、锁等。 |

**总结**：`kmem_cache` 把“对象规格”“分配策略”“per-CPU/per-node 入口”集中在一个描述符里，任何 SLUB 操作都得先锁定它。

---

## 2. `struct kmem_cache_cpu` —— 每个 CPU 的极速缓存

路径：`mm/slub.c`

| 成员 | 作用 |
| --- | --- |
| `void **freelist` | Fast path 的核心：指向当前 CPU slab 中下一个可用对象。分配时从这里弹出对象；释放时把对象串到这个链表头部。 |
| `unsigned long tid` | 事务 ID，配合 `freelist` 组成 `cmpxchg_double` 的 ABA 防护。每次更新 freelist 都会递增，确保 lockless fast path 正确。 |
| `struct slab *slab` | 该 CPU 当前冻结 (frozen) 的 slab。只有这个 CPU 能从它的 freelist 取对象，实现无锁分配。 |
| `struct slab *partial`（可选） | CPU 的 partial slab 链表头，用 `slab->next` 串多块 slab。当前 slab 用完时优先从这里提取，减少访问 node->partial 的频率。 |
| `local_lock_t lock` | Slow path 使用的本地锁，保护 `c->slab`/`c->freelist`/`c->partial` 更新。 |
| `unsigned int stat[NR_SLUB_STAT_ITEMS]`（可选） | Per-CPU 统计，例如 fast/slow path 次数、partial 推入/取出次数等。 |

**总结**：`kmem_cache_cpu` 让每个 CPU 有自己的“专属 slab + freelist + partial 缓存”，Fast path 在无锁情况下完成绝大部分分配/释放。

---

## 3. `struct kmem_cache_node` —— 每个 NUMA 节点的共享池

路径：`mm/slub.c`

| 成员 | 作用 |
| --- | --- |
| `spinlock_t list_lock` | 保护 node 的 partial/full 链表。所有跨 CPU 共享的 slab 操作都要持锁。 |
| `unsigned long nr_partial` | 该 node partial 链表里当前的 slab 数。搭配 `kmem_cache::min_partial` 决定是否需要释放/补充。 |
| `struct list_head partial` | node 级别的 partial slab 链表，CPU partial 满了就批量刷到这里；allocator 找不到可用 slab 时也从这里取。 |
| `struct list_head full`（仅 CONFIG_SLUB_DEBUG） | 调试用途，记录 full slab，方便遍历所有对象做一致性检查。 |
| `atomic_long_t nr_slabs / total_objects`（仅调试） | 统计该 node 上的 slab 数 / 对象数。 |

**总结**：`kmem_cache_node` 是“node 共享池”——CPU partial 和 frozen slab 之外的所有 slab 都归它管理。它负责 partial 链表、统计数据，并在必要时把 slab 释放回伙伴系统。

---

## 4. `struct slab` —— 复用 `struct page` 的单 slab 描述

路径：`mm/slab.h`

| 成员 | 作用 |
| --- | --- |
| `unsigned long __page_flags` / `unsigned int __page_type` / `atomic_t __page_refcount` | 直接复用 `struct page` 的字段，记录页标志、页类型（如 `PGTY_slab`/`PGTY_buddy`）、引用计数。 |
| `struct kmem_cache *slab_cache` | 反向指向所属 cache，释放或统计时可定位回 `kmem_cache`。 |
| `struct list_head slab_list` | slab 挂到 `kmem_cache_node->partial/full` 链表时使用（复用 `page->lru`）。 |
| `struct slab *next` / `int slabs`（仅 `CONFIG_SLUB_CPU_PARTIAL`） | CPU partial 链表指针：`next` 指向下一个 slab，`slabs` 记录从当前节点开始链上还剩多少 slab，便于判断是否超限。 |
| `void *freelist` | slab 内空闲对象链表头；每个空闲对象头部嵌入指向下一对象的指针。 |
| `union { unsigned long counters; struct { unsigned inuse:16; unsigned objects:15; unsigned frozen:1; }; }` | 压缩状态：`objects` 表示 slab 可容纳的总对象数，`inuse` 为已分配数量，`frozen`=1 表示 slab 被某 CPU 冻结，只允许该 CPU 分配。 |
| `struct rcu_head rcu_head` | 与上面 union 共址，调试/RCU 延迟释放路径中使用。 |
| `unsigned long obj_exts`（可选） | `CONFIG_SLAB_OBJ_EXT` 时指向对象扩展（如 KASAN shadow）的数组。 |

**要点**：`frozen=1` 代表 slab 处于 CPU 专用的 frozen 状态；`inuse==0` 时根据 `kmem_cache::min_partial` 决定挂回 node partial 还是 `discard_slab()`；`slab_list` 与 `next` 互斥使用，分别服务于 per-node 和 per-CPU partial 链表。

---

## 5. 四者如何配合？

1. **分配**：`slab_alloc_node()` → `kmem_cache_cpu`（fast path）→ 如果失败，去 `kmem_cache_node` 的 partial → 还不行就按 `kmem_cache::oo` 向伙伴系统申请。
2. **释放**：优先尝试 `kmem_cache_cpu`（同一个 CPU、同一个 frozen slab）；不匹配则在 `kmem_cache_node` 的 partial/full 列表上操作，必要时丢回伙伴系统。
3. **CPU 迁移/NUMA 切换**：`kmem_cache_cpu` 的 slab 不匹配当前 node 时，调用 `deactivate_slab()` 把 slab 交回 `kmem_cache_node`，再从目标 node 取 slab。

理解这四层结构，就能完整地把 SLUB 的对象生命周期（per-CPU 快速缓存 → per-node 共享池 → slab 元数据 → 伙伴系统）串起来。这个组织方式既保证了快速路径的极致性能，又能在内存紧张时退化到共享/伙伴层面继续服务。***

