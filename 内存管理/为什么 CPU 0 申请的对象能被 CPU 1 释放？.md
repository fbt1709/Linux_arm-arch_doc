# 为什么 CPU 0 申请的对象能被 CPU 1 释放？

## 一、核心问题

**问题**：为什么 CPU 0 申请的对象能被 CPU 1 释放？这不是 per-CPU 数据吗？

**答案**：**`kmem_cache_cpu` 是 per-CPU 数据，但对象本身不是 per-CPU 数据**。对象可以在任何 CPU 上被释放，不管它是在哪个 CPU 上分配的。

---

## 二、关键理解

### 2.1 Per-CPU 的是什么？

**Per-CPU 的数据**：
- **`kmem_cache_cpu`**：每个 CPU 有自己的 `kmem_cache_cpu` 结构
- **`c->slab`**：每个 CPU 当前使用的 slab
- **`c->freelist`**：每个 CPU 的本地 freelist

**不是 Per-CPU 的数据**：
- **对象本身**：对象是全局的，可以在任何 CPU 上被释放
- **`struct slab`**：Slab 是全局的，可以被任何 CPU 访问
- **`kmem_cache`**：Cache 是全局的，所有 CPU 共享

### 2.2 对象如何找到对应的 Slab？

**位置**: `mm/slub.c:4726-4746`

```c
void kfree(const void *object)
{
    struct folio *folio;
    struct slab *slab;
    struct kmem_cache *s;
    
    // 1. 从对象地址找到对应的 folio（页面）
    folio = virt_to_folio(object);
    
    // 2. 检查是否是 slab 页面
    if (unlikely(!folio_test_slab(folio))) {
        free_large_kmalloc(folio, (void *)object);
        return;
    }
    
    // 3. 从 folio 找到对应的 slab
    slab = folio_slab(folio);
    
    // 4. 从 slab 找到对应的 cache
    s = slab->slab_cache;
    
    // 5. 释放对象
    slab_free(s, slab, x, _RET_IP_);
}
```

**关键理解**：
- **对象地址本身包含了它所在的 slab 信息**
- 通过 `virt_to_folio(object)` 可以找到对象所在的页面
- 通过 `folio_slab(folio)` 可以找到对应的 slab
- **不需要知道对象是在哪个 CPU 上分配的**

---

## 三、释放流程详解

### 3.1 Fast Path：释放到当前 CPU 的 Slab

**位置**: `mm/slub.c:4528-4581`

```c
static __always_inline void do_slab_free(struct kmem_cache *s,
            struct slab *slab, void *head, void *tail,
            int cnt, unsigned long addr)
{
    struct kmem_cache_cpu *c;
    unsigned long tid;
    void **freelist;
    
redo:
    // 1. 获取当前 CPU 的 kmem_cache_cpu
    c = raw_cpu_ptr(s->cpu_slab);
    tid = READ_ONCE(c->tid);
    barrier();
    
    // 2. 检查释放的对象是否在当前 CPU 的 slab 中
    if (unlikely(slab != c->slab)) {
        // ❌ 不是当前 CPU 的 slab，进入 Slow Path
        __slab_free(s, slab, head, tail, cnt, addr);
        return;
    }
    
    // 3. ✅ 是当前 CPU 的 slab，Fast Path 释放
    if (USE_LOCKLESS_FAST_PATH()) {
        freelist = READ_ONCE(c->freelist);
        
        // 将释放的对象链到 c->freelist 头部
        set_freepointer(s, tail, freelist);
        
        // 无锁原子更新
        if (unlikely(!__update_cpu_freelist_fast(s, freelist, head, tid))) {
            note_cmpxchg_failure("slab_free", s, tid);
            goto redo;
        }
    }
    
    stat_add(s, FREE_FASTPATH, cnt);
}
```

**流程**：
1. 从对象地址找到对应的 slab（通过 `virt_to_folio` 和 `folio_slab`）
2. 检查 `slab == c->slab`（是否是当前 CPU 的 slab）
3. 如果是，释放到 `c->freelist`（Fast Path，无锁）
4. 如果不是，进入 Slow Path

### 3.2 Slow Path：释放到不同的 Slab

**位置**: `mm/slub.c:4400-4510`

```c
static void __slab_free(struct kmem_cache *s, struct slab *slab,
            void *head, void *tail, int cnt,
            unsigned long addr)
{
    void *prior;
    int was_frozen;
    struct slab new;
    unsigned long counters;
    struct kmem_cache_node *n = NULL;
    unsigned long flags;
    bool on_node_partial;
    
    stat(s, FREE_SLOWPATH);
    
    do {
        prior = slab->freelist;  // ← 读取 slab 的 freelist
        counters = slab->counters;
        
        // 将释放的对象链到 slab->freelist 头部
        set_freepointer(s, tail, prior);
        new.counters = counters;
        was_frozen = new.frozen;
        new.inuse -= cnt;
        
        // 原子更新 slab->freelist 和 slab->counters
    } while (!slab_update_freelist(s, slab,
        prior, counters,
        head, new.counters,
        "__slab_free"));
    
    if (likely(!n)) {
        if (likely(was_frozen)) {
            // Slab 是 frozen 的，释放到 slab->freelist
            // 冻结它的 CPU 可以从 slab->freelist 中取对象
            stat(s, FREE_FROZEN);
        } else if (kmem_cache_has_cpu_partial(s) && !prior) {
            // Slab 从 full 变为 partial，加入 CPU partial 列表
            put_cpu_partial(s, slab, 1);
            stat(s, CPU_PARTIAL_FREE);
        }
        return;
    }
    
    // 处理 slab 状态变化（加入 node->partial 或释放）
    // ...
}
```

**流程**：
1. 释放对象到 `slab->freelist`（不是 `c->freelist`）
2. 如果 slab 是 frozen 的，对象会被释放到 `slab->freelist`
3. 冻结它的 CPU 可以从 `slab->freelist` 中取对象
4. 如果 slab 不是 frozen 的，根据状态决定归宿

---

## 四、完整示例

### 4.1 场景：CPU 0 分配，CPU 1 释放

```
时间线：

T1: CPU 0 分配对象
    ┌─────────────────────────────────────┐
    │ CPU 0                               │
    │ c->slab → Slab A (frozen)           │
    │ c->freelist → Object 0 → ...        │
    └─────────────────────────────────────┘
    
    分配 Object 0 给用户
    c->freelist → Object 1 → ...
    slab->inuse = 1

T2: 用户将 Object 0 传递给 CPU 1
    ┌─────────────────────────────────────┐
    │ CPU 1                               │
    │ c->slab → Slab B (frozen)           │
    │ c->freelist → Object X → ...        │
    └─────────────────────────────────────┘
    
    用户代码在 CPU 1 上调用 kfree(Object 0)

T3: CPU 1 执行 kfree(Object 0)
    ┌─────────────────────────────────────┐
    │ kfree(Object 0)                    │
    │ 1. virt_to_folio(Object 0)         │
    │    → 找到 Object 0 所在的 folio     │
    │ 2. folio_slab(folio)                │
    │    → 找到 Slab A                    │
    │ 3. slab_free(s, Slab A, Object 0)   │
    └─────────────────────────────────────┘

T4: CPU 1 执行 do_slab_free()
    ┌─────────────────────────────────────┐
    │ CPU 1                               │
    │ c->slab → Slab B                    │
    │                                     │
    │ 检查: Slab A != c->slab (Slab B)   │
    │ → 进入 Slow Path                    │
    └─────────────────────────────────────┘

T5: CPU 1 执行 __slab_free()
    ┌─────────────────────────────────────┐
    │ Slab A                              │
    │ slab->freelist → Object 0 → NULL   │ ← 释放到这里
    │ slab->frozen = 1                    │
    │ slab->inuse = 0                     │
    └─────────────────────────────────────┘

T6: CPU 0 可以从 slab->freelist 获取对象
    ┌─────────────────────────────────────┐
    │ CPU 0                               │
    │ c->slab → Slab A (frozen)           │
    │ c->freelist = NULL                  │
    │                                     │
    │ get_freelist(s, Slab A)            │
    │ → 从 slab->freelist 获取 Object 0  │
    │ → c->freelist → Object 0 → NULL    │
    └─────────────────────────────────────┘
```

### 4.2 关键代码位置

**位置**: `mm/slub.c:4549-4552`

```c
if (unlikely(slab != c->slab)) {
    // 释放的对象不在当前 CPU 的 slab 中
    // 进入 Slow Path，释放到 slab->freelist
    __slab_free(s, slab, head, tail, cnt, addr);
    return;
}
```

**关键理解**：
- **`slab` 是从对象地址找到的**（通过 `virt_to_folio` 和 `folio_slab`）
- **`c->slab` 是当前 CPU 的 slab**
- **如果 `slab != c->slab`，说明对象不在当前 CPU 的 slab 中**
- **进入 Slow Path，释放到 `slab->freelist`**

---

## 五、为什么这样设计？

### 5.1 灵活性

- **对象可以在任何 CPU 上释放**，不需要知道它是在哪个 CPU 上分配的
- **支持跨 CPU 传递对象**，比如网络数据包、消息队列等

### 5.2 性能优化

- **Fast Path**：如果对象在当前 CPU 的 slab 中，无锁快速释放
- **Slow Path**：如果对象不在当前 CPU 的 slab 中，释放到 `slab->freelist`，让冻结它的 CPU 使用

### 5.3 内存管理

- **对象地址本身包含了它所在的 slab 信息**
- **不需要额外的元数据来跟踪对象是在哪个 CPU 上分配的**
- **通过 `virt_to_folio` 和 `folio_slab` 可以快速找到对应的 slab**

---

## 六、总结

### 6.1 关键点

1. **`kmem_cache_cpu` 是 per-CPU 数据**，但**对象本身不是 per-CPU 数据**
2. **对象可以在任何 CPU 上被释放**，不管它是在哪个 CPU 上分配的
3. **通过对象地址可以找到对应的 slab**（`virt_to_folio` → `folio_slab`）
4. **Fast Path**：如果对象在当前 CPU 的 slab 中，释放到 `c->freelist`
5. **Slow Path**：如果对象不在当前 CPU 的 slab 中，释放到 `slab->freelist`

### 6.2 设计优势

- **灵活性**：支持跨 CPU 传递对象
- **性能**：Fast Path 无锁快速释放
- **简单性**：不需要额外的元数据来跟踪对象分配位置

---

## 七、代码位置总结

| 操作 | 函数 | 文件 | 行号 |
|------|------|------|------|
| 释放入口 | `kfree()` | `mm/slub.c` | 4726-4747 |
| 找到 slab | `virt_to_folio()` + `folio_slab()` | - | - |
| Fast Path 释放 | `do_slab_free()` | `mm/slub.c` | 4528-4581 |
| Slow Path 释放 | `__slab_free()` | `mm/slub.c` | 4400-4510 |
| 检查是否当前 CPU 的 slab | `slab != c->slab` | `mm/slub.c` | 4549-4552 |

