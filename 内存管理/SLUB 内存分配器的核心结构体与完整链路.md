# SLUB 内存分配器的核心结构体与完整链路

## 1. 核心数据结构总览

SLUB（SLab Unqueued）是 Linux 内核的 slab 分配器实现之一，其设计目标是减少缓存行使用和避免复杂的队列管理。整个 SLUB 系统由以下核心数据结构组成：

```
全局层面:
└── slab_caches (list_head) - 所有 slab cache 的全局链表

单个 Cache 层面:
├── struct kmem_cache - Cache 描述符
│   ├── cpu_slab (per-CPU) - 每个 CPU 的快速路径数据
│   │   └── struct kmem_cache_cpu
│   └── node[N] - 每个 NUMA 节点的数据
│       └── struct kmem_cache_node

Slab 层面:
└── struct slab - 一个 slab 页面的元数据
    └── freelist - 空闲对象链表
```

---

## 2. 核心结构体详解

### 2.1 `struct kmem_cache` - Cache 描述符

**位置**: `mm/slab.h:258`

```c
struct kmem_cache {
    /* === Per-CPU 快速路径数据 === */
    struct kmem_cache_cpu __percpu *cpu_slab;  // 每个 CPU 的 slab 指针
    
    /* === Cache 配置参数 === */
    slab_flags_t flags;                         // Cache 标志位
    unsigned int size;                          // 对象大小（含元数据）
    unsigned int object_size;                   // 对象实际大小（不含元数据）
    unsigned int offset;                        // freelist 指针在对象中的偏移
    unsigned int inuse;                         // 对象内实际使用的字节数
    unsigned int align;                         // 对齐要求
    const char *name;                           // Cache 名称（用于调试）
    
    /* === Slab 页面组织 === */
    struct kmem_cache_order_objects oo;         // 最优 order 和对象数
    struct kmem_cache_order_objects min;        // 最小 order 和对象数
    gfp_t allocflags;                           // 从伙伴系统分配页面的 GFP 标志
    
    /* === CPU Partial 列表配置 === */
    #ifdef CONFIG_SLUB_CPU_PARTIAL
    unsigned int cpu_partial;                   // CPU partial 列表的最大对象数
    unsigned int cpu_partial_slabs;             // CPU partial 列表的最大 slab 数
    #endif
    
    /* === Node 列表（NUMA） === */
    struct kmem_cache_node *node[MAX_NUMNODES]; // 每个 NUMA 节点的管理结构
    
    /* === 其他字段 === */
    unsigned long min_partial;                  // 每个 node 保留的最小 partial slab 数
    struct list_head list;                      // 全局 slab_caches 链表节点
    struct reciprocal_value reciprocal_size;    // 快速除法优化（size 的倒数）
    void (*ctor)(void *object);                 // 对象构造函数
    int refcount;                               // 引用计数
    // ... 调试、KASAN、freelist 随机化等字段
};
```

**关键字段说明**:
- **`cpu_slab`**: 每个 CPU 有一个独立的 `kmem_cache_cpu` 结构，用于快速分配，避免跨 CPU 竞争
- **`node[]`**: 每个 NUMA 节点有一个 `kmem_cache_node`，管理该节点上的 partial/full slab 列表
- **`oo`**: 编码了 slab 的 page order 和每个 slab 包含的对象数量（高 16 位是 order，低 16 位是对象数）

---

### 2.2 `struct kmem_cache_cpu` - Per-CPU 快速路径数据

**位置**: `mm/slub.c:384`

```c
struct kmem_cache_cpu {
    /* === Lockless Fast Path === */
    union {
        struct {
            void **freelist;        // 指向下一个可用对象
            unsigned long tid;      // 全局唯一事务 ID（用于 ABA 检测）
        };
        freelist_aba_t freelist_tid;  // cmpxchg_double 原子更新
    };
    
    /* === 当前活跃的 Slab === */
    struct slab *slab;              // 当前 CPU 正在使用的 slab（frozen）
    
    /* === CPU Partial 列表 === */
    #ifdef CONFIG_SLUB_CPU_PARTIAL
    struct slab *partial;           // CPU partial slab 链表（未 frozen）
    #endif
    
    /* === 锁保护 === */
    local_lock_t lock;              // 保护上述字段的本地锁（慢路径使用）
    
    /* === 统计信息 === */
    #ifdef CONFIG_SLUB_STATS
    unsigned int stat[NR_SLUB_STAT_ITEMS];  // 各种统计计数
    #endif
};
```

**关键设计**:
- **Lockless Fast Path**: `freelist` 和 `tid` 通过 `cmpxchg_double` 原子操作更新，实现无锁快速分配/释放
- **Frozen Slab**: `c->slab` 指向的 slab 被"冻结"，只有当前 CPU 可以操作其 freelist，避免需要 list_lock
- **Transaction ID (tid)**: 每次分配/释放都会递增 `tid`，用于检测 ABA 问题（对象被释放后重新分配）

---

### 2.3 `struct kmem_cache_node` - NUMA Node 管理结构

**位置**: `mm/slub.c:425`

```c
struct kmem_cache_node {
    /* === 锁保护 === */
    spinlock_t list_lock;           // 保护 partial/full 列表的锁
    
    /* === Partial Slab 列表 === */
    unsigned long nr_partial;       // partial slab 的数量
    struct list_head partial;       // partial slab 链表（部分对象被使用）
    
    /* === Full Slab 列表（仅调试模式） === */
    #ifdef CONFIG_SLUB_DEBUG
    atomic_long_t nr_slabs;         // 该 node 上的 slab 总数
    atomic_long_t total_objects;    // 对象总数
    struct list_head full;          // full slab 链表（所有对象都被使用）
    #endif
};
```

**Slab 状态分类**:
1. **CPU Slab (Frozen)**: `c->slab`，被当前 CPU "冻结"，不在任何列表中
2. **CPU Partial**: `c->partial`，在 CPU 本地 partial 列表，未 frozen
3. **Node Partial**: `n->partial`，在 node 的 partial 列表，未 frozen
4. **Full**: `n->full`（调试模式），所有对象都被分配

---

### 2.4 `struct slab` - Slab 页面元数据

**位置**: `mm/slab.h:52`

```c
struct slab {
    /* === 复用 struct page 的字段 === */
    unsigned long __page_flags;     // 复用 page->flags
    
    /* === Slab Cache 关联 === */
    struct kmem_cache *slab_cache;  // 所属的 cache
    
    /* === Freelist 和计数器 === */
    union {
        struct list_head slab_list; // 用于挂到 node->partial/full 列表
        #ifdef CONFIG_SLUB_CPU_PARTIAL
        struct {
            struct slab *next;      // CPU partial 列表的链表指针
            int slabs;              // CPU partial 列表的 slab 计数
        };
        #endif
    };
    
    union {
        struct {
            void *freelist;         // 第一个空闲对象的指针
            union {
                unsigned long counters;  // 压缩的计数器
                struct {
                    unsigned inuse:16;   // 已分配对象数
                    unsigned objects:15; // slab 总对象数
                    unsigned frozen:1;   // 是否被冻结
                };
            };
        };
        #ifdef system_has_freelist_aba
        freelist_aba_t freelist_counter;  // 原子更新的 freelist+counter
        #endif
    };
    
    unsigned int __page_type;       // 复用 page->__page_type
    atomic_t __page_refcount;       // 复用 page->_refcount
    // ... 其他字段
};
```

**关键字段说明**:
- **`freelist`**: 指向 slab 中第一个空闲对象的指针，空闲对象通过对象内的 `next` 指针形成链表
- **`inuse`**: 已分配的对象数量，当 `inuse == objects` 时，slab 为 full
- **`frozen`**: 标记该 slab 是否被 CPU "冻结"，冻结的 slab 只有持有它的 CPU 可以操作

**Slab vs Page**: `struct slab` 实际上复用 `struct page` 的内存空间（`static_assert(sizeof(struct slab) <= sizeof(struct page))`），这是为了节省内存。

---

## 3. 完整分配链路 (Fast Path + Slow Path)

### 3.1 快速路径 (Fast Path)

**入口**: `kmem_cache_alloc()` → `slab_alloc_node()` → `__slab_alloc_node()`

**位置**: `mm/slub.c:3934`

```c
static __always_inline void *__slab_alloc_node(...) {
    struct kmem_cache_cpu *c;
    void *object;
    unsigned long tid;
    
    redo:
    // 1. 读取当前 CPU 的 cache_cpu 指针
    c = raw_cpu_ptr(s->cpu_slab);
    tid = READ_ONCE(c->tid);
    barrier();
    
    // 2. 检查 fast path 条件
    object = c->freelist;
    slab = c->slab;
    
    if (likely(object && slab && node_match(slab, node))) {
        // 3. 无锁更新：cmpxchg_double(freelist, tid, next, tid+1)
        void *next_object = get_freepointer_safe(s, object);
        if (likely(__update_cpu_freelist_fast(s, object, next_object, tid))) {
            stat(s, ALLOC_FASTPATH);
            return object;  // ✅ 快速路径成功
        }
        note_cmpxchg_failure("slab_alloc", s, tid);
        goto redo;  // cmpxchg 失败，重试或走慢路径
    }
    
    // 4. 快速路径失败，进入慢路径
    object = __slab_alloc(s, gfpflags, node, addr, c, orig_size);
    return object;
}
```

**Fast Path 条件**:
- `c->freelist` 非空（有可用对象）
- `c->slab` 非空（有活跃的 slab）
- `node_match()` 匹配（NUMA 亲和性）

**性能优势**: 整个过程**无锁**，使用 `cmpxchg_double` 原子操作更新 `freelist` 和 `tid`。

---

### 3.2 慢路径 (Slow Path) - `___slab_alloc()`

**位置**: `mm/slub.c:3667`

慢路径处理以下情况：
1. CPU slab 为空或 freelist 为空
2. CPU slab 不满足 NUMA 要求
3. Fast path 的 cmpxchg 失败

**流程**:

```c
static void *___slab_alloc(...) {
    reread_slab:
    slab = READ_ONCE(c->slab);
    
    // === 情况 1: CPU slab 存在但 freelist 为空 ===
    if (slab && node_match(slab, node)) {
        local_lock_irqsave(&c->lock, flags);
        freelist = get_freelist(s, slab);  // 从 slab->freelist 获取
        
        if (freelist) {
            c->freelist = get_freepointer(s, freelist);
            c->tid = next_tid(c->tid);
            return freelist;  // ✅ 成功从 slab 获取 freelist
        }
        
        // slab 已耗尽，需要新 slab
        c->slab = NULL;
        local_unlock_irqrestore(&c->lock, flags);
        deactivate_slab(s, slab, NULL);  // 将旧 slab 放回 node partial
    }
    
    // === 情况 2: 从 CPU Partial 列表获取 ===
    new_slab:
    #ifdef CONFIG_SLUB_CPU_PARTIAL
    while (slub_percpu_partial(c)) {
        slab = slub_percpu_partial(c);
        slub_set_percpu_partial(c, slab->next);  // 从列表移除
        
        if (node_match(slab, node)) {
            c->slab = slab;
            freelist = get_freelist(s, slab);
            freeze_slab(s, slab);  // 冻结新 slab
            return freelist;  // ✅ 成功从 CPU partial 获取
        }
        
        __put_partials(s, slab);  // NUMA 不匹配，放回 node partial
    }
    #endif
    
    // === 情况 3: 从 Node Partial 列表获取 ===
    new_objects:
    pc.flags = gfpflags;
    slab = get_partial(s, node, &pc);  // 从 node->partial 获取
    
    if (slab) {
        freelist = freeze_slab(s, slab);  // 冻结并获取 freelist
        c->slab = slab;
        return freelist;  // ✅ 成功从 node partial 获取
    }
    
    // === 情况 4: 从伙伴系统分配新 Slab ===
    slab = new_slab(s, gfpflags, node);
    if (unlikely(!slab)) {
        slab_out_of_memory(s, gfpflags, node);
        return NULL;  // ❌ 内存不足
    }
    
    // 初始化新 slab
    freelist = slab->freelist;
    slab->freelist = NULL;
    slab->inuse = slab->objects;
    slab->frozen = 1;
    
    c->slab = slab;
    return freelist;  // ✅ 成功分配新 slab
}
```

**慢路径关键操作**:
- **`get_freelist()`**: 从 slab 的 `freelist` 中取出所有空闲对象，清空 slab->freelist，返回对象链表
- **`freeze_slab()`**: 将 slab 标记为 `frozen=1`，表示它现在是 CPU 的专属 slab
- **`get_partial()`**: 从 node 的 partial 列表中取出一个 partial slab
- **`new_slab()`**: 从伙伴系统分配新的页面并初始化为 slab

---

## 4. 完整释放链路

### 4.1 快速路径 (Fast Path Free)

**入口**: `kmem_cache_free()` → `slab_free()` → `do_slab_free()`

**位置**: `mm/slub.c:4528`

```c
static __always_inline void do_slab_free(...) {
    struct kmem_cache_cpu *c;
    unsigned long tid;
    
    redo:
    c = raw_cpu_ptr(s->cpu_slab);
    tid = READ_ONCE(c->tid);
    barrier();
    
    // 检查是否释放到当前 CPU 的 slab
    if (likely(slab == c->slab)) {
        if (USE_LOCKLESS_FAST_PATH()) {
            freelist = READ_ONCE(c->freelist);
            set_freepointer(s, tail, freelist);  // 将对象加入 freelist
            
            // 无锁更新：cmpxchg_double(freelist, tid, head, tid+1)
            if (likely(__update_cpu_freelist_fast(s, freelist, head, tid))) {
                stat_add(s, FREE_FASTPATH, cnt);
                return;  // ✅ 快速路径成功
            }
            note_cmpxchg_failure("slab_free", s, tid);
            goto redo;
        }
        // PREEMPT_RT 模式，使用锁保护
        local_lock(&c->lock);
        // ... 更新 c->freelist 和 c->tid
        local_unlock(&c->lock);
        return;
    }
    
    // 慢路径：释放到不同的 slab
    __slab_free(s, slab, head, tail, cnt, addr);
}
```

**Fast Path Free 条件**:
- `slab == c->slab`（释放到当前 CPU 的冻结 slab）

---

### 4.2 慢路径 (Slow Path Free) - `__slab_free()`

**位置**: `mm/slub.c:4400`

```c
static void __slab_free(...) {
    stat(s, FREE_SLOWPATH);
    
    do {
        prior = slab->freelist;
        counters = slab->counters;
        
        new.counters = counters;
        new.inuse -= cnt;  // 减少已分配对象数
        
        // 原子更新 slab->freelist 和 slab->counters
    } while (!slab_update_freelist(s, slab, prior, counters, head, new.counters, "__slab_free"));
    
    was_frozen = new.frozen;
    
    // === 情况 1: Slab 被冻结（CPU slab） ===
    if (likely(was_frozen)) {
        stat(s, FREE_FROZEN);
        return;  // 对象已加入 slab->freelist，等待 CPU 下次分配
    }
    
    // === 情况 2: Slab 现在变空（所有对象都释放） ===
    if (unlikely(!new.inuse && n->nr_partial >= s->min_partial)) {
        remove_partial(n, slab);
        discard_slab(s, slab);  // 将空 slab 归还给伙伴系统
        stat(s, FREE_SLAB);
        return;
    }
    
    // === 情况 3: Slab 从 full 变为 partial ===
    if (!prior) {  // prior == NULL 表示之前是 full
        remove_full(s, n, slab);  // 从 full 列表移除
        add_partial(n, slab, DEACTIVATE_TO_TAIL);  // 加入 partial 列表
        stat(s, FREE_ADD_PARTIAL);
        return;
    }
    
    // === 情况 4: 加入 CPU Partial 列表 ===
    if (kmem_cache_has_cpu_partial(s) && !prior) {
        put_cpu_partial(s, slab, 1);  // 加入当前 CPU 的 partial 列表
        stat(s, CPU_PARTIAL_FREE);
        return;
    }
    
    // === 情况 5: Slab 已经在 partial 列表，只更新状态 ===
    // 无需移动，对象已加入 freelist
}
```

**慢路径释放的关键操作**:
- **`slab_update_freelist()`**: 原子更新 `slab->freelist` 和 `slab->counters`
- **`deactivate_slab()`**: 将 CPU slab 解冻并放回 node partial 列表
- **`put_cpu_partial()`**: 将 slab 加入当前 CPU 的 partial 列表
- **`discard_slab()`**: 将空 slab 归还给伙伴系统

---

## 5. 关键辅助函数

### 5.1 `get_freelist()` - 从 Slab 获取所有空闲对象

**位置**: `mm/slub.c:3596`

```c
static inline void *get_freelist(struct kmem_cache *s, struct slab *slab) {
    struct slab new;
    unsigned long counters;
    void *freelist;
    
    do {
        freelist = slab->freelist;
        counters = slab->counters;
        
        new.counters = counters;
        new.inuse = slab->objects;  // 标记所有对象都被使用
        new.frozen = (freelist != NULL);  // 如果有 freelist，则冻结
        
    } while (!__slab_update_freelist(s, slab,
        freelist, counters,
        NULL, new.counters,  // 清空 slab->freelist
        "get_freelist"));
    
    return freelist;  // 返回所有空闲对象的链表
}
```

**作用**: 将 slab 的所有空闲对象"抽取"出来，返回给调用者。之后这些对象会进入 CPU 的 `c->freelist`。

---

### 5.2 `freeze_slab()` - 冻结 Slab

**位置**: `mm/slub.c:3624`

```c
static inline void *freeze_slab(struct kmem_cache *s, struct slab *slab) {
    struct slab new;
    unsigned long counters;
    void *freelist;
    
    do {
        freelist = slab->freelist;
        counters = slab->counters;
        
        new.counters = counters;
        VM_BUG_ON(new.frozen);
        
        new.inuse = slab->objects;  // 标记所有对象被使用（后续会从 c->freelist 分配）
        new.frozen = 1;  // 冻结标记
        
    } while (!slab_update_freelist(s, slab,
        freelist, counters,
        NULL, new.counters,  // 清空 slab->freelist
        "freeze_slab"));
    
    return freelist;  // 返回 freelist，之后赋值给 c->freelist
}
```

**作用**: 将 slab 标记为 `frozen=1`，并清空其 `freelist`。之后该 slab 只能由持有它的 CPU 操作。

---

### 5.3 `deactivate_slab()` - 解冻并放回 Node Partial

**位置**: `mm/slub.c:3047`

```c
static void deactivate_slab(struct kmem_cache *s, struct slab *slab, void *freelist) {
    struct kmem_cache_node *n = get_node(s, slab_nid(slab));
    int free_delta = 0;
    void *freelist_tail;
    
    // 1. 计算 CPU freelist 中的对象数
    while (freelist) {
        freelist_tail = freelist;
        freelist = get_freepointer(s, freelist);
        free_delta++;
    }
    
    // 2. 将 CPU freelist 合并到 slab->freelist
    do {
        old.freelist = READ_ONCE(slab->freelist);
        old.counters = READ_ONCE(slab->counters);
        VM_BUG_ON(!old.frozen);  // 必须是被冻结的
        
        new.counters = old.counters;
        new.frozen = 0;  // 解冻
        new.inuse -= free_delta;  // 减少已分配对象数
        
        if (freelist_tail) {
            set_freepointer(s, freelist_tail, old.freelist);
            new.freelist = freelist;  // 合并后的 freelist
        } else {
            new.freelist = old.freelist;
        }
    } while (!slab_update_freelist(s, slab, old.freelist, old.counters, new.freelist, new.counters, "unfreezing slab"));
    
    // 3. 根据状态决定 slab 的归宿
    if (!new.inuse && n->nr_partial >= s->min_partial) {
        discard_slab(s, slab);  // 空 slab，归还给伙伴系统
    } else if (new.freelist) {
        add_partial(n, slab, DEACTIVATE_TO_TAIL);  // 加入 node partial
    } else {
        // full slab，加入 full 列表（调试模式）
    }
}
```

**作用**: 将 CPU slab 解冻，合并 CPU freelist 和 slab->freelist，然后根据 slab 状态决定其归宿。

---

## 6. 数据结构关系图

```
全局 slab_caches 链表
│
├── kmem_cache ("task_struct")
│   ├── cpu_slab[CPU0] ──────┐
│   ├── cpu_slab[CPU1] ──────┤ Per-CPU 快速路径
│   └── cpu_slab[CPU2] ──────┘
│       │
│       ├── .freelist ───> [obj1] -> [obj2] -> [obj3] -> NULL
│       ├── .slab ───────> struct slab (frozen=1)
│       └── .partial ────> struct slab -> struct slab -> NULL
│
│   └── node[NODE0]
│       ├── .partial ───> slab -> slab -> slab (partial list)
│       └── .full ──────> slab -> slab (full list, 仅调试)
│
└── kmem_cache ("file")
    └── ...
```

**Slab 生命周期**:
```
new_slab() [伙伴系统分配页面]
    ↓
初始化: freelist 指向所有对象，inuse=0
    ↓
freeze_slab() → 成为 CPU slab (frozen=1)
    ↓
对象被分配 → inuse 增加，freelist 减少
    ↓
deactivate_slab() → 解冻 (frozen=0) → 加入 node->partial
    ↓
对象继续被释放/分配 → 在 node->partial 中
    ↓
如果 inuse == 0 → discard_slab() [归还给伙伴系统]
```

---

## 7. 性能优化要点

### 7.1 Fast Path 无锁设计
- **Lockless**: Fast path 使用 `cmpxchg_double` 原子操作，无需任何锁
- **Per-CPU**: 每个 CPU 有独立的 `cpu_slab`，避免跨 CPU 竞争
- **Transaction ID**: `tid` 用于检测 ABA 问题和并发修改

### 7.2 Frozen Slab 机制
- **独占访问**: Frozen slab 只有持有它的 CPU 可以操作，避免需要 `list_lock`
- **减少锁竞争**: 大部分分配/释放都在 fast path，不需要获取 `node->list_lock`

### 7.3 CPU Partial 列表
- **NUMA 优化**: 将 partial slab 缓存在 CPU 本地，减少跨节点访问
- **批量操作**: 多个对象可以批量释放到 CPU partial，减少 list_lock 获取次数

### 7.4 内存局部性
- **对象紧凑**: 同一 slab 中的对象在物理内存中连续，提高缓存命中率
- **预取优化**: `prefetch_freepointer()` 预取下一个对象的地址

---

## 8. 总结

SLUB 的核心设计思想是：
1. **Fast Path 优先**: 大部分分配/释放都通过无锁 fast path 完成
2. **Per-CPU 缓存**: 每个 CPU 维护独立的 slab 和 freelist
3. **分层管理**: CPU → Node → Buddy System 的分层结构
4. **最小化锁竞争**: 通过 frozen slab 和 CPU partial 减少全局锁的使用

整个系统通过 `kmem_cache` → `kmem_cache_cpu` / `kmem_cache_node` → `slab` → `object` 的层次结构，实现了高效的小对象内存分配。

