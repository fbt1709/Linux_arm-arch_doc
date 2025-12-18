# Slab 在什么时候会被 Frozen？

## 一、核心问题

**问题**：Slab 在什么时候会被 frozen？好像没看到。

**答案**：Slab 会在**两个场景**下被 frozen：
1. **从 node->partial 获取 slab 时**：通过 `freeze_slab()` 函数
2. **从伙伴系统分配新 slab 时**：在 `new_slab()` 函数中直接设置

---

## 二、场景 1：从 node->partial 获取 slab 时

### 2.1 调用链

**位置**: `mm/slub.c:3817-3833`

```c
static void *___slab_alloc(...)
{
    // 尝试从 node->partial 获取 slab
    slab = get_partial(s, node, &pc);
    if (slab) {
        if (kmem_cache_debug(s)) {
            // 调试模式：特殊处理
            return freelist;
        }
        
        // ✅ 冻结 slab
        freelist = freeze_slab(s, slab);
        goto retry_load_slab;
    }
    
    // 如果 get_partial() 失败，分配新 slab
    slab = new_slab(s, pc.flags, node);
}
```

### 2.2 `freeze_slab()` 函数详解

**位置**: `mm/slub.c:3624-3646`

```c
static inline void *freeze_slab(struct kmem_cache *s, struct slab *slab)
{
    struct slab new;
    unsigned long counters;
    void *freelist;
    
    do {
        // 1. 读取当前 slab 的状态
        freelist = slab->freelist;
        counters = slab->counters;
        
        // 2. 准备新的状态
        new.counters = counters;
        VM_BUG_ON(new.frozen);  // ← 确保 slab 之前不是 frozen 的
        
        // 3. 设置新的状态
        new.inuse = slab->objects;  // ← 所有对象都在使用（从 slab->freelist 取走）
        new.frozen = 1;              // ← ✅ 设置为 frozen
        
    } while (!slab_update_freelist(s, slab,
        freelist, counters,
        NULL, new.counters,  // ← slab->freelist 被清空
        "freeze_slab"));
    
    return freelist;  // ← 返回从 slab->freelist 取走的所有对象
}
```

**关键理解**：
- **`new.frozen = 1`**：将 slab 设置为 frozen 状态
- **`new.inuse = slab->objects`**：所有对象都在使用（因为从 `slab->freelist` 取走了）
- **`slab->freelist = NULL`**：清空 slab 的 freelist（对象被移到 CPU 的 `c->freelist`）

### 2.3 完整流程

```
1. CPU 需要新的 slab
   └─ ___slab_alloc() → get_partial() → 从 node->partial 获取 slab
   
2. 获取到 slab（未 frozen）
   └─ slab->frozen = 0
   └─ slab->freelist → Object 0 → Object 1 → ... → NULL
   
3. 调用 freeze_slab()
   └─ 读取 slab->freelist（所有对象）
   └─ 设置 new.frozen = 1
   └─ 设置 slab->freelist = NULL
   └─ 返回所有对象（给 CPU 的 c->freelist）
   
4. 设置到 CPU
   └─ c->slab = slab
   └─ c->freelist = freeze_slab() 返回的对象
   └─ slab->frozen = 1  ← ✅ 现在 slab 是 frozen 的
```

---

## 三、场景 2：从伙伴系统分配新 slab 时

### 3.1 调用链

**位置**: `mm/slub.c:3836-3871`

```c
static void *___slab_alloc(...)
{
    // 如果 get_partial() 失败，分配新 slab
    slub_put_cpu_ptr(s->cpu_slab);
    slab = new_slab(s, pc.flags, node);  // ← 从伙伴系统分配新 slab
    c = slub_get_cpu_ptr(s->cpu_slab);
    
    if (unlikely(!slab)) {
        // 分配失败
        return NULL;
    }
    
    stat(s, ALLOC_SLAB);
    
    if (kmem_cache_debug(s)) {
        // 调试模式：特殊处理
        freelist = alloc_single_from_new_slab(s, slab, orig_size);
        return freelist;
    }
    
    // ✅ 直接设置 frozen
    freelist = slab->freelist;
    slab->freelist = NULL;
    slab->inuse = slab->objects;
    slab->frozen = 1;  // ← ✅ 设置为 frozen
}
```

### 3.2 `new_slab()` 函数详解

**位置**: `mm/slub.c:2642-2650`

```c
static struct slab *new_slab(struct kmem_cache *s, gfp_t flags, int node)
{
    if (unlikely(flags & GFP_SLAB_BUG_MASK))
        flags = kmalloc_fix_flags(flags);
    
    WARN_ON_ONCE(s->ctor && (flags & __GFP_ZERO));
    
    return allocate_slab(s,
        flags & (GFP_RECLAIM_MASK | GFP_CONSTRAINT_MASK), node);
}
```

**位置**: `mm/slub.c:3864-3871`

```c
/*
 * No other reference to the slab yet so we can
 * muck around with it freely without cmpxchg
 */
freelist = slab->freelist;
slab->freelist = NULL;
slab->inuse = slab->objects;
slab->frozen = 1;  // ← ✅ 直接设置为 frozen
```

**关键理解**：
- **新分配的 slab 还没有被任何 CPU 使用**，所以可以直接设置 `frozen = 1`
- **不需要原子操作**：因为还没有其他 CPU 访问这个 slab
- **`slab->freelist = NULL`**：清空 freelist（对象被移到 CPU 的 `c->freelist`）

### 3.3 完整流程

```
1. CPU 需要新的 slab
   └─ ___slab_alloc() → new_slab() → allocate_slab() → 从伙伴系统分配
   
2. 分配新 slab
   └─ allocate_slab() → alloc_slab_page() → alloc_pages()
   └─ 返回新的 slab（未 frozen）
   └─ slab->freelist → Object 0 → Object 1 → ... → NULL
   
3. 直接设置 frozen
   └─ slab->freelist = NULL（清空）
   └─ slab->inuse = slab->objects（所有对象都在使用）
   └─ slab->frozen = 1  ← ✅ 设置为 frozen
   
4. 设置到 CPU
   └─ c->slab = slab
   └─ c->freelist = freelist（从 slab->freelist 取走的对象）
   └─ slab->frozen = 1  ← ✅ 现在 slab 是 frozen 的
```

---

## 四、两种场景的对比

### 4.1 相同点

| 特性 | 场景 1（从 node->partial） | 场景 2（新分配） |
|------|-------------------------|-----------------|
| **结果** | `slab->frozen = 1` | `slab->frozen = 1` |
| **slab->freelist** | 被清空（`NULL`） | 被清空（`NULL`） |
| **slab->inuse** | `= slab->objects` | `= slab->objects` |
| **c->slab** | 指向该 slab | 指向该 slab |
| **c->freelist** | 包含所有对象 | 包含所有对象 |

### 4.2 不同点

| 特性 | 场景 1（从 node->partial） | 场景 2（新分配） |
|------|-------------------------|-----------------|
| **来源** | `node->partial` 列表 | 伙伴系统 |
| **操作方式** | 原子操作（`slab_update_freelist`） | 直接设置（无原子操作） |
| **原因** | 可能有其他 CPU 在访问 | 新分配，没有其他 CPU 访问 |
| **函数** | `freeze_slab()` | `new_slab()` + 直接设置 |

---

## 五、为什么需要 Frozen？

### 5.1 Frozen 的作用

**位置**: `mm/slub.c:78-86`

```c
/*
 * Frozen slabs
 *
 * If a slab is frozen then it is exempt from list management. It is
 * the cpu slab which is actively allocated from by the processor that
 * froze it and it is not on any list. The processor that froze the
 * slab is the one who can perform list operations on the slab. Other
 * processors may put objects onto the freelist but the processor that
 * froze the slab is the only one that can retrieve the objects from the
 * slab's freelist.
 */
```

**关键理解**：
1. **只有冻结它的 CPU 可以从 `slab->freelist` 中取对象**
2. **其他 CPU 可以释放对象到 `slab->freelist`**（但不能取对象）
3. **Frozen slab 不在任何列表中**（不在 node->partial 或 node->full 中）
4. **实现无锁快速分配**：因为只有当前 CPU 可以操作，所以不需要锁

### 5.2 无锁快速分配

**位置**: `mm/slub.c:3975-4004`

```c
static __always_inline void *__slab_alloc_node(...)
{
    c = raw_cpu_ptr(s->cpu_slab);
    
    // 从 c->freelist 快速分配（无锁）
    object = c->freelist;
    slab = c->slab;
    
    if (likely(object && slab && node_match(slab, node))) {
        // 无锁快速分配
        void *next_object = get_freepointer_safe(s, object);
        __update_cpu_freelist_fast(s, object, next_object, tid);
        return object;
    }
}
```

**关键理解**：
- **因为 slab 是 frozen 的，只有当前 CPU 可以操作**
- **所以不需要锁，可以实现无锁快速分配**

---

## 六、Slab 状态转换图

```
┌─────────────────────────────────────────┐
│ 从 node->partial 获取 slab              │
│ (未 frozen)                             │
└─────────────────────────────────────────┘
            │
            │ freeze_slab()
            │ - 原子操作
            │ - new.frozen = 1
            │ - slab->freelist = NULL
            ▼
┌─────────────────────────────────────────┐
│ Frozen Slab                             │
│ - slab->frozen = 1                     │
│ - 只有冻结它的 CPU 可以操作              │
│ - 不在任何列表中                        │
└─────────────────────────────────────────┘
            │
            │ 或
            │
┌─────────────────────────────────────────┐
│ 从伙伴系统分配新 slab                    │
│ (未 frozen)                             │
└─────────────────────────────────────────┘
            │
            │ 直接设置
            │ - slab->frozen = 1
            │ - slab->freelist = NULL
            ▼
┌─────────────────────────────────────────┐
│ Frozen Slab                             │
│ - slab->frozen = 1                     │
│ - 只有冻结它的 CPU 可以操作              │
│ - 不在任何列表中                        │
└─────────────────────────────────────────┘
            │
            │ CPU 不再使用
            │ deactivate_slab()
            │ - new.frozen = 0
            ▼
┌─────────────────────────────────────────┐
│ 未 Frozen Slab                          │
│ - slab->frozen = 0                     │
│ - 转移到 node->partial 或释放            │
└─────────────────────────────────────────┘
```

---

## 七、关键代码位置总结

| 操作 | 函数 | 文件 | 行号 |
|------|------|------|------|
| 从 node->partial 冻结 | `freeze_slab()` | `mm/slub.c` | 3624-3646 |
| 新分配时冻结 | `new_slab()` + 直接设置 | `mm/slub.c` | 3864-3871 |
| 调用 freeze_slab | `___slab_alloc()` | `mm/slub.c` | 3832 |
| 解冻 | `deactivate_slab()` | `mm/slub.c` | 3097 |

---

## 八、总结

### 8.1 关键点

1. **Slab 会在两个场景下被 frozen**：
   - **从 node->partial 获取时**：通过 `freeze_slab()` 函数（原子操作）
   - **从伙伴系统分配新 slab 时**：在 `new_slab()` 后直接设置（无原子操作）

2. **Frozen 的作用**：
   - **只有冻结它的 CPU 可以从 `slab->freelist` 中取对象**
   - **实现无锁快速分配**
   - **Frozen slab 不在任何列表中**

3. **Frozen 状态的特点**：
   - **`slab->frozen = 1`**
   - **`slab->freelist = NULL`**（对象被移到 CPU 的 `c->freelist`）
   - **`slab->inuse = slab->objects`**（所有对象都在使用）

### 8.2 设计优势

- **性能优化**：Frozen slab 实现无锁快速分配
- **简单性**：不需要复杂的锁机制
- **NUMA 本地性**：每个 CPU 有自己 frozen 的 slab

---

**核心理解**：Slab 会在**从 node->partial 获取时**（通过 `freeze_slab()`）或**从伙伴系统分配新 slab 时**（直接设置）被 frozen，以实现无锁快速分配。

