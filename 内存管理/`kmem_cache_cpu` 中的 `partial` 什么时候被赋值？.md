# `kmem_cache_cpu` 中的 `partial` 什么时候被赋值？

## 一、核心问题

**问题**：`kmem_cache_cpu` 中的 `partial` 是什么时候被赋值的？不是直接加到 node 吗？

**答案**：**`c->partial` 会在两个场景下被赋值**：
1. **释放时，slab 从 full 变为 partial**：通过 `put_cpu_partial()` 加入 CPU partial
2. **从 node->partial 获取 slab 时**：如果 CPU partial 还没满，会加入 CPU partial

**为什么需要 CPU partial？**：**性能优化**。CPU partial 是 CPU 的本地缓存，减少从 node->partial 获取的频率，避免频繁的锁竞争。

---

## 二、场景 1：释放时，slab 从 full 变为 partial

### 2.1 调用链

**位置**: `mm/slub.c:4463-4470`

```c
static void __slab_free(struct kmem_cache *s, struct slab *slab,
            void *head, void *tail, int cnt,
            unsigned long addr)
{
    // ... 释放对象到 slab->freelist ...
    
    if (likely(!n)) {
        if (likely(was_frozen)) {
            // Slab 是 frozen 的，释放到 slab->freelist
            stat(s, FREE_FROZEN);
        } else if (kmem_cache_has_cpu_partial(s) && !prior) {
            /*
             * If we started with a full slab then put it onto the
             * per cpu partial list.
             */
            // ✅ 如果 slab 之前是 full（prior == NULL），现在释放了一个对象
            // 变成 partial，加入 CPU partial 列表
            put_cpu_partial(s, slab, 1);
            stat(s, CPU_PARTIAL_FREE);
        }
        return;
    }
}
```

**关键理解**：
- **`prior == NULL`**：表示 slab 之前是 full（`slab->freelist == NULL`）
- **现在释放了一个对象**：slab 从 full 变为 partial
- **如果系统启用了 CPU partial**：加入 `c->partial` 列表，而不是直接加入 `node->partial`

### 2.2 `put_cpu_partial()` 函数详解

**位置**: `mm/slub.c:3204-3242`

```c
static void put_cpu_partial(struct kmem_cache *s, struct slab *slab, int drain)
{
    struct slab *oldslab;
    struct slab *slab_to_put = NULL;
    unsigned long flags;
    int slabs = 0;
    
    local_lock_irqsave(&s->cpu_slab->lock, flags);
    
    // 1. 读取当前 CPU 的 partial 列表
    oldslab = this_cpu_read(s->cpu_slab->partial);
    
    if (oldslab) {
        if (drain && oldslab->slabs >= s->cpu_partial_slabs) {
            /*
             * Partial array is full. Move the existing set to the
             * per node partial list. Postpone the actual unfreezing
             * outside of the critical section.
             */
            // ✅ CPU partial 列表满了，需要批量转移到 node->partial
            slab_to_put = oldslab;
            oldslab = NULL;
        } else {
            slabs = oldslab->slabs;  // ← 记录当前有多少个 slab
        }
    }
    
    slabs++;
    
    // 2. 将新 slab 加入 CPU partial 列表头部
    slab->slabs = slabs;  // ← 记录从该 slab 开始，后续还有多少个 slab
    slab->next = oldslab;  // ← 链到旧的 partial 列表
    
    // ✅ 赋值给 c->partial
    this_cpu_write(s->cpu_slab->partial, slab);
    
    local_unlock_irqrestore(&s->cpu_slab->lock, flags);
    
    // 3. 如果 CPU partial 满了，批量转移到 node->partial
    if (slab_to_put) {
        __put_partials(s, slab_to_put);
        stat(s, CPU_PARTIAL_DRAIN);
    }
}
```

**关键理解**：
- **`c->partial = slab`**：将新 slab 设置为 CPU partial 列表的头部
- **`slab->next = oldslab`**：将旧的 partial 列表链到新 slab 后面
- **`slab->slabs = slabs`**：记录从该 slab 开始，后续还有多少个 slab
- **如果 CPU partial 满了**：批量转移到 `node->partial`

---

## 三、场景 2：从 node->partial 获取 slab 时

### 3.1 调用链

**位置**: `mm/slub.c:2865-2872`

```c
static struct slab *get_partial_node(struct kmem_cache *s,
            struct kmem_cache_node *n, struct partial_context *pc)
{
    struct slab *slab;
    int objects = 0;
    int pages = 0;
    
    // 从 node->partial 获取 slab
    list_for_each_entry_safe(slab, slab2, &n->partial, slab_list) {
        // ... 检查 slab 是否可用 ...
        
        if (objects) {
            // 已经获取到足够的对象
            stat(s, ALLOC_FROM_PARTIAL);
            
            if ((slub_get_cpu_partial(s) == 0)) {
                break;
            }
        } else {
            // ✅ 如果 CPU partial 还没满，加入 CPU partial
            put_cpu_partial(s, slab, 0);
            stat(s, CPU_PARTIAL_NODE);
            
            if (++partial_slabs > slub_get_cpu_partial(s) / 2) {
                break;
            }
        }
    }
}
```

**关键理解**：
- **从 `node->partial` 获取 slab 时**：如果 CPU partial 还没满，会加入 CPU partial
- **这样下次分配时**：可以直接从 `c->partial` 获取，不需要访问 `node->partial`

---

## 四、为什么需要 CPU partial？

### 4.1 性能优化

**问题**：为什么不直接加到 node->partial？

**答案**：**性能优化**。CPU partial 是 CPU 的本地缓存，有以下优势：

1. **减少锁竞争**：
   - **`node->partial`**：需要 `node->list_lock`（跨 CPU 竞争）
   - **`c->partial`**：只需要 `c->lock`（本地锁，不跨 CPU）

2. **减少 node 访问**：
   - **从 `c->partial` 获取**：不需要访问 `node->partial`，减少跨 CPU 访问
   - **当 `c->slab` 用完后**：优先从 `c->partial` 获取，而不是从 `node->partial`

3. **快速切换**：
   - **当 `c->slab` 用完后**：可以从 `c->partial` 快速获取新的 slab
   - **不需要加锁访问 `node->partial`**

### 4.2 使用场景

**位置**: `mm/slub.c:3766-3795`

```c
#ifdef CONFIG_SLUB_CPU_PARTIAL
while (slub_percpu_partial(c)) {
    local_lock_irqsave(&s->cpu_slab->lock, flags);
    if (unlikely(c->slab)) {
        local_unlock_irqrestore(&s->cpu_slab->lock, flags);
        goto reread_slab;
    }
    if (unlikely(!slub_percpu_partial(c))) {
        local_unlock_irqrestore(&s->cpu_slab->lock, flags);
        goto new_objects;
    }
    
    // ✅ 从 CPU partial 获取 slab
    slab = slub_percpu_partial(c);
    slub_set_percpu_partial(c, slab);
    
    if (likely(node_match(slab, node) &&
               pfmemalloc_match(slab, gfpflags))) {
        c->slab = slab;
        freelist = get_freelist(s, slab);
        VM_BUG_ON(!freelist);
        stat(s, CPU_PARTIAL_ALLOC);
        goto load_freelist;
    }
    
    local_unlock_irqrestore(&s->cpu_slab->lock, flags);
    
    // 如果 node 不匹配，转移到 node->partial
    slab->next = NULL;
    __put_partials(s, slab);
}
#endif
```

**关键理解**：
- **当 `c->slab` 用完后**：优先从 `c->partial` 获取新的 slab
- **如果 node 匹配**：直接使用，快速切换
- **如果 node 不匹配**：转移到 `node->partial`

---

## 五、CPU partial 与 node->partial 的关系

### 5.1 数据流向

```
释放时（slab 从 full 变为 partial）：
┌─────────────────────────────────────┐
│ __slab_free()                      │
│ - prior == NULL (之前是 full)      │
│ - 现在释放了一个对象，变成 partial │
│                                     │
│ put_cpu_partial(s, slab, 1)        │
│ → c->partial = slab                │ ← ✅ 加入 CPU partial
└─────────────────────────────────────┘

CPU partial 满了：
┌─────────────────────────────────────┐
│ put_cpu_partial()                  │
│ - oldslab->slabs >= cpu_partial_slabs│
│                                     │
│ __put_partials(s, oldslab)         │
│ → add_partial(n, slab)             │ ← 转移到 node->partial
└─────────────────────────────────────┘

从 node->partial 获取时：
┌─────────────────────────────────────┐
│ get_partial_node()                  │
│ - 从 node->partial 获取 slab        │
│                                     │
│ put_cpu_partial(s, slab, 0)         │
│ → c->partial = slab                │ ← ✅ 加入 CPU partial
└─────────────────────────────────────┘

分配时（c->slab 用完后）：
┌─────────────────────────────────────┐
│ ___slab_alloc()                     │
│ - c->slab = NULL                    │
│                                     │
│ 从 c->partial 获取                  │
│ → slab = slub_percpu_partial(c)    │
│ → c->slab = slab                    │ ← 从 CPU partial 获取
└─────────────────────────────────────┘
```

### 5.2 完整生命周期

```
阶段 1: Slab 从 full 变为 partial
┌─────────────────────────────────────┐
│ __slab_free()                      │
│ - prior == NULL (之前是 full)      │
│ - 现在释放了一个对象                │
│                                     │
│ put_cpu_partial(s, slab, 1)        │
│ → c->partial = slab                │ ← ✅ 加入 CPU partial
└─────────────────────────────────────┘

阶段 2: CPU partial 列表增长
┌─────────────────────────────────────┐
│ c->partial → Slab 1                 │
│   └─ Slab 2                         │
│       └─ Slab 3                     │
│           └─ ...                    │
└─────────────────────────────────────┘

阶段 3: CPU partial 满了
┌─────────────────────────────────────┐
│ put_cpu_partial()                   │
│ - oldslab->slabs >= cpu_partial_slabs│
│                                     │
│ __put_partials(s, oldslab)          │
│ → 批量转移到 node->partial          │
└─────────────────────────────────────┘

阶段 4: 从 node->partial 获取时
┌─────────────────────────────────────┐
│ get_partial_node()                  │
│ - 从 node->partial 获取 slab        │
│                                     │
│ put_cpu_partial(s, slab, 0)         │
│ → c->partial = slab                │ ← ✅ 加入 CPU partial
└─────────────────────────────────────┘

阶段 5: 分配时使用
┌─────────────────────────────────────┐
│ ___slab_alloc()                     │
│ - c->slab = NULL                    │
│                                     │
│ 从 c->partial 获取                  │
│ → c->slab = slab                    │
└─────────────────────────────────────┘
```

---

## 六、关键代码位置总结

| 操作 | 函数 | 文件 | 行号 |
|------|------|------|------|
| 赋值给 c->partial | `put_cpu_partial()` | `mm/slub.c` | 3234 |
| 释放时调用 | `__slab_free()` | `mm/slub.c` | 4468 |
| 从 node 获取时调用 | `get_partial_node()` | `mm/slub.c` | 2871 |
| 从 c->partial 获取 | `slub_percpu_partial()` | `mm/slub.c` | 3779 |
| 批量转移到 node | `__put_partials()` | `mm/slub.c` | 3128-3167 |

---

## 七、总结

### 7.1 关键点

1. **`c->partial` 会在两个场景下被赋值**：
   - **释放时**：slab 从 full 变为 partial，通过 `put_cpu_partial()` 加入
   - **从 node->partial 获取时**：如果 CPU partial 还没满，会加入 CPU partial

2. **为什么需要 CPU partial？**：
   - **性能优化**：减少锁竞争，减少 node 访问
   - **快速切换**：当 `c->slab` 用完后，可以从 `c->partial` 快速获取
   - **本地缓存**：CPU 的本地缓存，避免频繁访问 `node->partial`

3. **CPU partial 与 node->partial 的关系**：
   - **CPU partial 满了**：批量转移到 `node->partial`
   - **从 node->partial 获取时**：如果 CPU partial 还没满，会加入 CPU partial
   - **分配时**：优先从 `c->partial` 获取，而不是从 `node->partial`

### 7.2 设计优势

- **性能优化**：减少锁竞争，减少 node 访问
- **快速切换**：当 `c->slab` 用完后，可以从 `c->partial` 快速获取
- **本地缓存**：CPU 的本地缓存，避免频繁访问 `node->partial`

---

**核心理解**：**`c->partial` 会在释放时（slab 从 full 变为 partial）或从 node->partial 获取时被赋值**。CPU partial 是 CPU 的本地缓存，用于性能优化，减少锁竞争和 node 访问。当 CPU partial 满了时，会批量转移到 `node->partial`。

