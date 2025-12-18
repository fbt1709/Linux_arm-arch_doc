# 为什么 `kmem_cache_node` 有 partial 的 slab？

## 一、核心理解

**你的理解部分正确，但不完整**。确实，很多 slab 是从 CPU 转移到 node 的，但**不是"用完了"才转移，而是"CPU 不再使用但还有空闲对象"时转移**。

---

## 二、Slab 从 CPU 转移到 Node 的几种场景

### 2.1 场景 1：分配时的 NUMA 不匹配

**位置**: `mm/slub.c:3692-3702`

```c
static void *___slab_alloc(...)
{
    slab = READ_ONCE(c->slab);
    
    // 检查 NUMA 节点是否匹配
    if (unlikely(!node_match(slab, node))) {
        stat(s, ALLOC_NODE_MISMATCH);
        goto deactivate_slab;  // ← 转移到 node
    }
    
deactivate_slab:
    // 解冻 slab，转移到 node->partial
    deactivate_slab(s, slab, freelist);
}
```

**情况**:
- CPU 当前使用的 slab 来自 Node A
- 但分配请求要求从 Node B 分配
- **此时 slab 可能还有空闲对象**（partial），但 CPU 不能继续使用
- 解冻 slab，转移到 `node->partial` 列表

---

### 2.2 场景 2：分配时的 PFMEMALLOC 不匹配

**位置**: `mm/slub.c:3710-3711`

```c
    // 检查 PFMEMALLOC 标志是否匹配
    if (unlikely(!pfmemalloc_match(slab, gfpflags)))
        goto deactivate_slab;  // ← 转移到 node
```

**情况**:
- CPU 的 slab 是 PFMEMALLOC 页面（紧急内存）
- 但分配请求是普通内存（或反之）
- **slab 还有空闲对象**，但类型不匹配
- 转移到 `node->partial`，让其他匹配的分配使用

---

### 2.3 场景 3：当前 slab 的 freelist 为空，但 slab 还有对象

**位置**: `mm/slub.c:3723-3730`

```c
    // 尝试从当前 slab 获取 freelist
    freelist = get_freelist(s, slab);
    
    if (!freelist) {
        // slab->freelist 为空，但 slab->inuse < slab->objects
        // 说明 slab 还有对象，但都在其他 CPU 的 freelist 中
        c->slab = NULL;
        goto new_slab;  // ← 需要获取新的 slab
    }
```

**情况**:
- `c->freelist` 为空（当前 CPU 的本地 freelist 用完了）
- `slab->freelist` 也为空（当 slab 被冻结时，所有对象都被取到 `c->freelist`，所以 `slab->freelist` 是 NULL）
- 然而 `slab->inuse < slab->objects`（slab 中还有对象已经被分配出去，但还没有释放回来）
- **关键理解**：一个 frozen slab **只能被一个 CPU 使用**（冻结它的 CPU），其他 CPU 不能从这个 slab 分配对象
- 但是，**其他 CPU 可以释放对象到这个 slab**（通过 Slow Path 释放到 `slab->freelist`）
- 当前 CPU 的 `c->freelist` 用完了，但 `slab->freelist` 还是空的（其他 CPU 还没有释放对象回来）
- CPU 放弃这个 slab，解冻它并转移到 `node->partial`，等待对象被释放回来后再使用

---

### 2.4 场景 4：释放时从 full 变为 partial

**位置**: `mm/slub.c:4491-4494`

```c
static void __slab_free(...)
{
    // 如果 slab 之前是 full（prior == NULL），现在释放了一个对象
    if (!kmem_cache_has_cpu_partial(s) && unlikely(!prior)) {
        // 从 full 变为 partial，加入 node->partial
        add_partial(n, slab, DEACTIVATE_TO_TAIL);
        stat(s, FREE_ADD_PARTIAL);
    }
}
```

**情况**:
- 释放对象时，slab 从 **full** 变为 **partial**
- 如果系统没有启用 CPU partial 功能，直接加入 `node->partial`
- **此时 slab 还有大量空闲对象**（刚释放了一个）

---

### 2.5 场景 5：CPU partial 列表满了，需要 drain

**位置**: `mm/slub.c:3204-3242`

```c
static void put_cpu_partial(struct kmem_cache *s, struct slab *slab, int drain)
{
    oldslab = this_cpu_read(s->cpu_slab->partial);
    
    if (oldslab) {
        if (drain && oldslab->slabs >= s->cpu_partial_slabs) {
            // CPU partial 列表满了，转移到 node->partial
            slab_to_put = oldslab;
            oldslab = NULL;
        }
    }
    
    // 将新 slab 加入 CPU partial
    slab->slabs = slabs;
    slab->next = oldslab;
    this_cpu_write(s->cpu_slab->partial, slab);
    
    // 如果 CPU partial 满了，批量转移到 node
    if (slab_to_put) {
        __put_partials(s, slab_to_put);
        stat(s, CPU_PARTIAL_DRAIN);
    }
}
```

**情况**:
- CPU partial 列表达到 `cpu_partial_slabs` 限制
- 需要将旧的 CPU partial slab 批量转移到 `node->partial`
- **这些 slab 都还有空闲对象**（partial）

---

## 三、`deactivate_slab()` 的决策逻辑

**位置**: `mm/slub.c:3047-3125`

这是 slab 从 CPU 转移到 node 的核心函数：

```c
static void deactivate_slab(struct kmem_cache *s, struct slab *slab,
            void *freelist)
{
    struct kmem_cache_node *n = get_node(s, slab_nid(slab));
    
    // 1. 解冻 slab，合并 CPU 的 freelist 到 slab->freelist
    // 2. 更新 slab->inuse（减少已分配对象数）
    
    // 3. 根据 slab 状态决定归宿
    if (!new.inuse && n->nr_partial >= s->min_partial) {
        // 情况 A: slab 完全空了，且 node->partial 已经足够多
        // → 直接释放回伙伴系统
        discard_slab(s, slab);
    } else if (new.freelist) {
        // 情况 B: slab 还有空闲对象（partial）
        // → 加入 node->partial 列表
        add_partial(n, slab, tail);
    } else {
        // 情况 C: slab 完全满了（不应该发生）
        stat(s, DEACTIVATE_FULL);
    }
}
```

**关键理解**:
- **只有 slab 还有空闲对象时，才会加入 `node->partial`**
- 如果 slab 完全空了，且 `node->partial` 已经足够多（`nr_partial >= min_partial`），会直接释放回伙伴系统

---

## 四、为什么需要 node->partial？

### 4.1 性能优化

1. **NUMA 本地性**
   - 当 CPU 的 slab 来自错误的 NUMA 节点时，转移到正确的 node
   - 其他 CPU 可以从本地 node 获取，减少跨节点访问

2. **负载均衡**
   - 某个 CPU 的 slab 用完了，但其他 CPU 可能还需要
   - 转移到 node，让所有 CPU 共享

3. **内存回收**
   - 当内存压力大时，可以从 `node->partial` 回收 slab
   - 如果只在 CPU 上，难以统一管理

### 4.2 避免浪费

- **如果 CPU 的 slab 还有对象就释放**，会造成浪费
- 转移到 `node->partial`，让其他 CPU 继续使用，提高利用率

---

## 五、完整生命周期示例

### 5.1 Slab 从 CPU 到 Node 的流程

```
阶段 1: CPU 使用 slab
┌─────────────────────┐
│ c->slab = slab      │ ← CPU 持有
│ c->freelist = obj0  │
│ slab->frozen = 1    │ ← 冻结，只有该 CPU 可用
└─────────────────────┘
         │
         │ 分配对象...
         │
阶段 2: CPU 不再需要（但 slab 还有对象）
┌─────────────────────┐
│ slab->inuse = 5/10   │ ← 还有 5 个空闲对象
│ slab->frozen = 0    │ ← 解冻
│ c->slab = NULL      │ ← CPU 放弃
└─────────────────────┘
         │
         │ deactivate_slab()
         │
阶段 3: 转移到 node->partial
┌─────────────────────┐
│ node->partial       │ ← 加入 node 的 partial 列表
│   └─ slab (5/10)    │ ← 其他 CPU 可以使用
└─────────────────────┘
         │
         │ 其他 CPU 从 node->partial 获取
         │
阶段 4: Slab 完全空了
┌─────────────────────┐
│ slab->inuse = 0/10  │ ← 所有对象都分配了
│ nr_partial >= min   │ ← node 的 partial 已经足够多
└─────────────────────┘
         │
         │ discard_slab()
         │
阶段 5: 释放回伙伴系统
┌─────────────────────┐
│ __free_pages()      │ ← 归还给伙伴系统
└─────────────────────┘
```

---

## 六、关键代码位置

| 场景 | 函数 | 文件 | 行号 |
|------|------|------|------|
| NUMA 不匹配 | `___slab_alloc()` | `mm/slub.c` | 3692-3702 |
| PFMEMALLOC 不匹配 | `___slab_alloc()` | `mm/slub.c` | 3710-3711 |
| 解冻并转移 | `deactivate_slab()` | `mm/slub.c` | 3047-3125 |
| 释放时加入 | `__slab_free()` | `mm/slub.c` | 4491-4494 |
| CPU partial 满了 | `put_cpu_partial()` | `mm/slub.c` | 3204-3242 |
| 批量转移 | `__put_partials()` | `mm/slub.c` | 3127-3167 |

---

## 七、总结

### 7.1 为什么 node 有 partial slab？

1. **不是"CPU 用完了"**，而是"CPU 不再使用但还有空闲对象"
2. **多种原因导致 CPU 放弃 slab**：
   - NUMA 节点不匹配
   - PFMEMALLOC 类型不匹配
   - CPU 的 freelist 用完了，但 slab 还有对象
   - CPU partial 列表满了，需要 drain
3. **转移到 node 的好处**：
   - 让其他 CPU 继续使用（负载均衡）
   - 提高内存利用率（避免浪费）
   - 支持 NUMA 本地性优化

### 7.2 关键区别

| 状态 | 描述 | 归宿 |
|------|------|------|
| **CPU 的 slab 用完了** | `c->freelist == NULL` 且 `slab->freelist == NULL` | 转移到 `node->partial`（如果还有对象）或释放 |
| **CPU 不再使用** | NUMA 不匹配、类型不匹配等 | 转移到 `node->partial`（如果还有对象） |
| **Slab 完全空了** | `slab->inuse == 0` | 如果 `nr_partial >= min_partial`，直接释放 |

---

**核心理解**：`node->partial` 中的 slab **不是"用完了"**，而是"**CPU 不再使用但还有空闲对象**"，转移到 node 让其他 CPU 继续使用，提高内存利用率。

