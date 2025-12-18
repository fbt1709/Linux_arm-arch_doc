# `kmem_cache_cpu` 各成员详解

## 一、结构体定义

**位置**: `mm/slub.c:384-400`

```c
struct kmem_cache_cpu {
    union {
        struct {
            void **freelist;        /* Pointer to next available object */
            unsigned long tid;      /* Globally unique transaction id */
        };
        freelist_aba_t freelist_tid;  /* For cmpxchg_double atomic update */
    };
    struct slab *slab;             /* The slab from which we are allocating */
#ifdef CONFIG_SLUB_CPU_PARTIAL
    struct slab *partial;          /* Partially allocated slabs */
#endif
    local_lock_t lock;              /* Protects the fields above */
#ifdef CONFIG_SLUB_STATS
    unsigned int stat[NR_SLUB_STAT_ITEMS];
#endif
};
```

---

## 二、各成员详细说明

### 2.1 `freelist` - 指向下一个可用对象

**类型**: `void **`

**作用**:
- 指向当前 CPU 的 slab 中**下一个可分配的对象**
- 这是 **Fast Path 的核心**，实现无锁快速分配

**使用场景**:

#### 分配时（Fast Path）

**位置**: `mm/slub.c:3975-4004`

```c
static __always_inline void *__slab_alloc_node(...)
{
    c = raw_cpu_ptr(s->cpu_slab);
    
    // 读取当前 CPU 的 freelist
    object = c->freelist;  // ← 直接获取对象指针
    
    if (likely(object && slab && node_match(slab, node))) {
        // 无锁快速分配
        void *next_object = get_freepointer_safe(s, object);
        __update_cpu_freelist_fast(s, object, next_object, tid);
        return object;  // ← 返回对象
    }
}
```

**流程**:
1. `c->freelist` 指向 Object 0
2. 返回 Object 0 给用户
3. 更新 `c->freelist = Object 0->next`（指向 Object 1）

#### 释放时（Fast Path）

**位置**: `mm/slub.c:4554-4562`

```c
static __always_inline void do_slab_free(...)
{
    c = raw_cpu_ptr(s->cpu_slab);
    freelist = READ_ONCE(c->freelist);
    
    // 将释放的对象链到 freelist 头部
    set_freepointer(s, tail, freelist);
    
    // 原子更新 c->freelist
    __update_cpu_freelist_fast(s, freelist, head, tid);
}
```

**流程**:
1. 读取 `c->freelist`（当前指向 Object 1）
2. 将释放的 Object 0 的 next 指针指向 Object 1
3. 更新 `c->freelist = Object 0`（新释放的对象成为新的头部）

**关键特性**:
- **无锁操作**：通过 `cmpxchg_double` 原子更新 `freelist` 和 `tid`
- **CPU 本地**：每个 CPU 有独立的 `freelist`，避免跨 CPU 竞争
- **快速路径**：大部分分配/释放都走这个路径，性能极高

---

### 2.2 `tid` - 全局唯一事务 ID

**类型**: `unsigned long`

**作用**:
- **ABA 问题检测**：防止对象被释放后重新分配导致的 ABA 问题
- **并发控制**：与 `freelist` 一起通过 `cmpxchg_double` 原子更新

**初始化**:

**位置**: `mm/slub.c:3000-3003`

```c
static inline unsigned int init_tid(int cpu)
{
    return cpu;  // ← 初始值为 CPU ID
}
```

**更新规则**:

**位置**: `mm/slub.c:2983-2986`

```c
static inline unsigned long next_tid(unsigned long tid)
{
    return tid + TID_STEP;  // ← 每次操作递增 TID_STEP
}
```

**TID_STEP 定义**:

**位置**: `mm/slub.c:2974-2981`

```c
#ifdef CONFIG_PREEMPTION
// 支持抢占：TID_STEP = roundup_pow_of_two(CONFIG_NR_CPUS)
// 确保每个 CPU 的 TID 不会重叠
#define TID_STEP  roundup_pow_of_two(CONFIG_NR_CPUS)
#else
// 不支持抢占：TID_STEP = 1
#define TID_STEP 1
#endif
```

**ABA 问题示例**:

```
时间线：
1. CPU 读取 c->freelist = Object A, tid = 100
2. 其他 CPU 释放 Object A，重新分配 Object A
3. CPU 尝试 cmpxchg: (freelist=A, tid=100) → (freelist=B, tid=101)
4. 如果 tid 没有变化，cmpxchg 会成功，但这是错误的！
5. 通过 tid 递增，确保 cmpxchg 失败，需要重新读取
```

**使用场景**:

**位置**: `mm/slub.c:3576-3586`

```c
static inline bool
__update_cpu_freelist_fast(struct kmem_cache *s,
           void *freelist_old, void *freelist_new,
           unsigned long tid)
{
    freelist_aba_t old = { .freelist = freelist_old, .counter = tid };
    freelist_aba_t new = { .freelist = freelist_new, .counter = next_tid(tid) };
    
    // 原子比较并交换：同时检查 freelist 和 tid
    return this_cpu_try_cmpxchg_freelist(s->cpu_slab->freelist_tid.full,
                     &old.full, new.full);
}
```

**关键特性**:
- **全局唯一**：每个 CPU 的 TID 不会重叠（通过 TID_STEP 保证）
- **原子更新**：与 `freelist` 一起通过 `cmpxchg_double` 更新
- **ABA 防护**：确保对象没有被其他 CPU 释放并重新分配

---

### 2.3 `freelist_tid` - 原子更新的联合体

**类型**: `freelist_aba_t`

**作用**:
- 将 `freelist` 和 `tid` **打包成一个原子单位**
- 通过 `cmpxchg_double` 实现**无锁的原子更新**

**定义**:

**位置**: `mm/slab.h:43-49`

```c
typedef union {
    struct {
        void *freelist;
        unsigned long counter;  // ← 这里 counter 就是 tid
    };
    freelist_full_t full;  // ← u128 或 u64，用于 cmpxchg_double
} freelist_aba_t;
```

**使用场景**:

```c
// 读取
freelist_aba_t current = READ_ONCE(c->freelist_tid);
void *freelist = current.freelist;
unsigned long tid = current.counter;

// 原子更新
freelist_aba_t old = { .freelist = old_freelist, .counter = old_tid };
freelist_aba_t new = { .freelist = new_freelist, .counter = next_tid(old_tid) };
this_cpu_try_cmpxchg_freelist(c->freelist_tid.full, &old.full, new.full);
```

**关键特性**:
- **双字对齐**：确保 `cmpxchg_double` 可以原子操作
- **性能优化**：一次原子操作同时更新两个字段，避免两次操作的开销

---

### 2.4 `slab` - 当前 CPU 使用的 Slab 页面

**类型**: `struct slab *`

**作用**:
- 指向当前 CPU **正在使用的 slab 页面**
- 这个 slab 是**冻结的（frozen）**，只有当前 CPU 可以操作

**使用场景**:

#### 分配时检查

**位置**: `mm/slub.c:3975-3979`

```c
object = c->freelist;
slab = c->slab;  // ← 获取当前 CPU 的 slab

if (!USE_LOCKLESS_FAST_PATH() ||
    unlikely(!object || !slab || !node_match(slab, node))) {
    // Fast Path 失败，进入 Slow Path
}
```

#### 释放时检查

**位置**: `mm/slub.c:4549-4552`

```c
if (unlikely(slab != c->slab)) {
    // 释放的对象不在当前 CPU 的 slab 中
    // 进入 Slow Path
    __slab_free(s, slab, head, tail, cnt, addr);
    return;
}
```

#### 设置新的 slab

**位置**: `mm/slub.c:3903-3905`

```c
// 从 node->partial 或新分配的 slab 设置到 CPU
c->slab = slab;
c->freelist = get_freepointer(s, freelist);
```

**Frozen Slab 的概念**:

**位置**: `mm/slub.c:3624-3645`

```c
static inline void *freeze_slab(struct kmem_cache *s, struct slab *slab)
{
    // 设置 slab->frozen = 1
    new.frozen = 1;
    
    // 只有持有该 slab 的 CPU 可以操作其 freelist
    // 其他 CPU 不能访问，避免需要 list_lock
}
```

**关键特性**:
- **Frozen 状态**：`slab->frozen = 1`，只有当前 CPU 可以操作
- **无锁快速路径**：因为只有当前 CPU 可以操作，所以不需要锁
- **NUMA 本地性**：CPU 优先使用本地 node 的 slab

---

### 2.5 `partial` - CPU Partial Slab 列表

**类型**: `struct slab *`（链表头）

**作用**:
- 存储当前 CPU 的 **partial slab 链表**
- 当 `c->slab` 用完后，可以从 `c->partial` 快速获取新的 slab

**使用场景**:

#### 从 CPU partial 获取 slab

**位置**: `mm/slub.c:3767-3789`

```c
#ifdef CONFIG_SLUB_CPU_PARTIAL
while (slub_percpu_partial(c)) {
    // 从 CPU partial 列表获取一个 slab
    slab = slub_percpu_partial(c);
    slub_set_percpu_partial(c, slab);
    
    if (likely(node_match(slab, node) &&
               pfmemalloc_match(slab, gfpflags))) {
        c->slab = slab;
        freelist = get_freelist(s, slab);
        return freelist;  // ← 快速获取对象
    }
}
#endif
```

#### 将 slab 加入 CPU partial

**位置**: `mm/slub.c:3204-3242`

```c
static void put_cpu_partial(struct kmem_cache *s, struct slab *slab, int drain)
{
    oldslab = this_cpu_read(s->cpu_slab->partial);
    
    // 将新 slab 加入 CPU partial 列表头部
    slab->slabs = slabs;
    slab->next = oldslab;
    this_cpu_write(s->cpu_slab->partial, slab);
    
    // 如果 CPU partial 满了，批量转移到 node
    if (slab_to_put) {
        __put_partials(s, slab_to_put);
    }
}
```

**链表结构**:

```
c->partial
  └─ slab1 (slabs=3)  ← 链表头
      └─ slab2 (slabs=2)
          └─ slab3 (slabs=1)
              └─ NULL
```

**`slabs` 字段的作用**:

**位置**: `mm/slub.c:62-64`

```c
struct {
    struct slab *next;
    int slabs;  /* Nr of slabs left */  ← 记录后续还有多少个 slab
};
```

- `slab->slabs` 记录从该 slab 开始，**后续还有多少个 slab**
- 用于限制 CPU partial 列表的长度（`cpu_partial_slabs`）

**关键特性**:
- **CPU 本地缓存**：减少从 node->partial 获取的频率
- **快速获取**：当 `c->slab` 用完后，优先从 `c->partial` 获取
- **长度限制**：达到 `cpu_partial_slabs` 时，批量转移到 node

---

### 2.6 `lock` - 本地锁

**类型**: `local_lock_t`

**作用**:
- **保护 Slow Path 操作**：当 Fast Path 失败时，需要加锁操作
- **本地锁**：只保护当前 CPU 的数据，不跨 CPU

**使用场景**:

#### Slow Path 分配

**位置**: `mm/slub.c:3714-3747`

```c
static void *___slab_alloc(...)
{
    // Fast Path 失败，进入 Slow Path
    local_lock_irqsave(&s->cpu_slab->lock, flags);
    
    // 在锁保护下操作
    if (unlikely(slab != c->slab)) {
        local_unlock_irqrestore(&s->cpu_slab->lock, flags);
        goto reread_slab;
    }
    
    freelist = get_freelist(s, slab);
    c->freelist = get_freepointer(s, freelist);
    c->tid = next_tid(c->tid);
    
    local_unlock_irqrestore(&s->cpu_slab->lock, flags);
    return freelist;
}
```

#### Slow Path 释放

**位置**: `mm/slub.c:4564-4579`

```c
if (USE_LOCKLESS_FAST_PATH()) {
    // Fast Path：无锁
} else {
    // Slow Path：需要锁
    local_lock(&s->cpu_slab->lock);
    c = this_cpu_ptr(s->cpu_slab);
    
    if (unlikely(slab != c->slab)) {
        local_unlock(&s->cpu_slab->lock);
        goto redo;
    }
    
    c->freelist = head;
    c->tid = next_tid(tid);
    local_unlock(&s->cpu_slab->lock);
}
```

**关键特性**:
- **本地锁**：只保护当前 CPU，不跨 CPU，性能好
- **Slow Path 专用**：Fast Path 不使用锁
- **中断安全**：`local_lock_irqsave` 会禁用中断

---

### 2.7 `stat[]` - 统计信息数组

**类型**: `unsigned int stat[NR_SLUB_STAT_ITEMS]`

**作用**:
- 记录当前 CPU 的**各种分配/释放统计信息**
- 用于性能分析和调试

**统计项定义**:

**位置**: `mm/slub.c:355-377`

```c
enum stat_item {
    ALLOC_FASTPATH,        /* Fast path 分配次数 */
    ALLOC_SLOWPATH,        /* Slow path 分配次数 */
    FREE_FASTPATH,         /* Fast path 释放次数 */
    FREE_SLOWPATH,         /* Slow path 释放次数 */
    FREE_FROZEN,           /* 释放到 frozen slab 的次数 */
    FREE_ADD_PARTIAL,      /* 释放时加入 partial 列表的次数 */
    FREE_REMOVE_PARTIAL,   /* 从 partial 列表移除的次数 */
    ALLOC_FROM_PARTIAL,    /* 从 partial 列表分配的次数 */
    ALLOC_SLAB,            /* 从伙伴系统分配新 slab 的次数 */
    ALLOC_REFILL,          /* 从 slab 重新填充 freelist 的次数 */
    ALLOC_NODE_MISMATCH,   /* NUMA 节点不匹配的次数 */
    FREE_SLAB,             /* 释放 slab 回伙伴系统的次数 */
    CPUSLAB_FLUSH,         /* CPU slab 刷新的次数 */
    DEACTIVATE_FULL,       /* 解冻 full slab 的次数 */
    DEACTIVATE_EMPTY,      /* 解冻 empty slab 的次数 */
    DEACTIVATE_TO_HEAD,    /* 解冻后加入 partial 列表头部 */
    DEACTIVATE_TO_TAIL,    /* 解冻后加入 partial 列表尾部 */
    DEACTIVATE_REMOTE_FREES, /* 远程释放的次数 */
    DEACTIVATE_BYPASS,     /* 绕过解冻的次数 */
    ORDER_FALLBACK,        /* 降级 order 分配的次数 */
    CMPXCHG_DOUBLE_CPU_FAIL, /* cmpxchg_double CPU 失败次数 */
    CMPXCHG_DOUBLE_FAIL,   /* cmpxchg_double 失败次数 */
    CPU_PARTIAL_ALLOC,     /* 从 CPU partial 分配的次数 */
    CPU_PARTIAL_FREE,      /* 释放到 CPU partial 的次数 */
    CPU_PARTIAL_NODE,      /* 从 node 转移到 CPU partial 的次数 */
    CPU_PARTIAL_DRAIN,     /* CPU partial 排空的次数 */
    NR_SLUB_STAT_ITEMS
};
```

**使用示例**:

**位置**: `mm/slub.c:403-412`

```c
static inline void stat(const struct kmem_cache *s, enum stat_item si)
{
#ifdef CONFIG_SLUB_STATS
    // 增加对应统计项的计数
    raw_cpu_inc(s->cpu_slab->stat[si]);
#endif
}
```

**关键特性**:
- **Per-CPU 统计**：每个 CPU 独立统计，避免跨 CPU 竞争
- **性能分析**：可以分析 Fast Path vs Slow Path 的比例
- **调试工具**：通过 `/sys/kernel/slab/<cache>/` 查看统计信息

---

## 三、各成员的关系和协作

### 3.1 Fast Path 分配流程

```
1. 读取 c->freelist 和 c->slab
   ├─ c->freelist → Object 0
   └─ c->slab → Slab A (frozen)

2. 检查 Fast Path 条件
   ├─ object != NULL ✓
   ├─ slab != NULL ✓
   └─ node_match(slab, node) ✓

3. 原子更新
   ├─ 读取 tid
   ├─ 获取 next_object = Object 0->next
   └─ cmpxchg_double: (freelist=Object0, tid=100) → (freelist=Object1, tid=101)

4. 返回 Object 0
```

### 3.2 Fast Path 释放流程

```
1. 检查释放的对象是否在当前 CPU 的 slab 中
   └─ slab == c->slab ✓

2. 原子更新
   ├─ 读取 c->freelist 和 c->tid
   ├─ 将释放的对象链到 freelist 头部
   └─ cmpxchg_double: (freelist=Object1, tid=101) → (freelist=Object0, tid=102)

3. 完成（无锁）
```

### 3.3 Slow Path 流程

```
1. Fast Path 失败
   └─ 加锁: local_lock_irqsave(&c->lock)

2. 从 slab 获取 freelist
   ├─ get_freelist(s, c->slab)
   └─ 更新 c->freelist 和 c->tid

3. 解锁
   └─ local_unlock_irqrestore(&c->lock)
```

---

## 四、关键设计思想

### 4.1 无锁 Fast Path

- **`freelist` 和 `tid` 打包**：通过 `cmpxchg_double` 原子更新
- **Frozen Slab**：只有当前 CPU 可以操作，避免需要锁
- **Per-CPU 数据**：每个 CPU 独立，避免跨 CPU 竞争

### 4.2 CPU Partial 缓存

- **减少 node 访问**：优先从 `c->partial` 获取，减少锁竞争
- **长度限制**：达到 `cpu_partial_slabs` 时，批量转移到 node
- **快速切换**：当 `c->slab` 用完后，快速从 `c->partial` 获取新的

### 4.3 统计和调试

- **Per-CPU 统计**：每个 CPU 独立统计，避免竞争
- **性能分析**：可以分析 Fast Path vs Slow Path 的比例
- **问题诊断**：通过统计信息诊断性能问题

---

## 五、总结

| 成员 | 类型 | 作用 | 关键特性 |
|------|------|------|----------|
| `freelist` | `void **` | 指向下一个可用对象 | Fast Path 核心，无锁操作 |
| `tid` | `unsigned long` | 全局唯一事务 ID | ABA 防护，原子更新 |
| `freelist_tid` | `freelist_aba_t` | 打包的原子单位 | cmpxchg_double 优化 |
| `slab` | `struct slab *` | 当前 CPU 的 slab | Frozen 状态，无锁快速路径 |
| `partial` | `struct slab *` | CPU partial 列表 | 本地缓存，减少 node 访问 |
| `lock` | `local_lock_t` | 本地锁 | Slow Path 保护 |
| `stat[]` | `unsigned int[]` | 统计信息 | 性能分析和调试 |

**核心设计**:
- **Fast Path**：通过 `freelist` + `tid` + `slab` 实现无锁快速分配/释放
- **Slow Path**：通过 `lock` 保护，从 `slab` 或 `partial` 获取对象
- **性能优化**：`partial` 列表减少 node 访问，`stat[]` 用于性能分析

