# CPU 从 NUMA 0 迁移到 NUMA 1 时 Slab 的切换机制

## 一、核心问题

**问题**：如果一个 CPU 突然从 NUMA 0 切换到 NUMA 1，那么它的 slab 是怎么切换的？

**答案**：**每次分配时都会检查 NUMA 节点匹配**。如果当前 slab 的 node 不匹配，会触发 `deactivate_slab()`，然后从新的 node 获取 slab。

---

## 二、关键机制

### 2.1 每次分配时检查 NUMA 节点

**位置**: `mm/slub.c:3975-3980`

```c
static __always_inline void *__slab_alloc_node(...)
{
    c = raw_cpu_ptr(s->cpu_slab);
    object = c->freelist;
    slab = c->slab;
    
    // ✅ 每次分配时都检查 node 匹配
    if (!USE_LOCKLESS_FAST_PATH() ||
        unlikely(!object || !slab || !node_match(slab, node))) {
        // 如果不匹配，进入 Slow Path
        object = __slab_alloc(s, gfpflags, node, addr, c, orig_size);
    }
}
```

**关键理解**：
- **每次分配时都会检查 `node_match(slab, node)`**
- **`node` 参数是当前 CPU 所在的 NUMA 节点**（通过 `numa_node_id()` 或 `cpu_to_node()` 获取）
- **如果 `slab->nid != node`，会触发 Slow Path**

### 2.2 `node_match()` 函数

**位置**: `mm/slub.c:3394-3401`

```c
static inline int node_match(struct slab *slab, int node)
{
#ifdef CONFIG_NUMA
    if (node != NUMA_NO_NODE && slab_nid(slab) != node)
        return 0;  // ← 不匹配
#endif
    return 1;  // ← 匹配
}
```

**关键理解**：
- **如果 `slab_nid(slab) != node`，返回 0（不匹配）**
- **如果 `node == NUMA_NO_NODE`，总是返回 1（匹配）**

---

## 三、CPU 迁移时的完整流程

### 3.1 场景：CPU 从 NUMA 0 迁移到 NUMA 1

```
时间线：

T1: CPU 在 NUMA 0 上运行
    ┌─────────────────────────────────────┐
    │ CPU 0 (NUMA 0)                     │
    │ c->slab → Slab A (nid=0)            │
    │ c->freelist → Object 0 → ...        │
    └─────────────────────────────────────┘

T2: 调度器将任务迁移到 NUMA 1
    ┌─────────────────────────────────────┐
    │ CPU 0 (NUMA 1)  ← CPU 迁移          │
    │ c->slab → Slab A (nid=0)            │ ← 还是旧的 slab
    │ c->freelist → Object 0 → ...        │
    └─────────────────────────────────────┘

T3: 第一次分配请求（在 NUMA 1 上）
    ┌─────────────────────────────────────┐
    │ __slab_alloc_node()                 │
    │ node = numa_node_id() = 1           │ ← 当前 node
    │ slab = c->slab = Slab A (nid=0)     │ ← 旧的 slab
    │                                     │
    │ 检查: node_match(Slab A, 1)        │
    │ → slab_nid(Slab A) = 0 != 1        │
    │ → 返回 0（不匹配）                   │
    │                                     │
    │ → 进入 Slow Path                    │
    └─────────────────────────────────────┘

T4: Slow Path 处理
    ┌─────────────────────────────────────┐
    │ ___slab_alloc()                     │
    │ 检查: !node_match(slab, node)      │
    │ → stat(s, ALLOC_NODE_MISMATCH)     │
    │ → goto deactivate_slab              │
    └─────────────────────────────────────┘

T5: 解冻并转移旧的 Slab
    ┌─────────────────────────────────────┐
    │ deactivate_slab(s, Slab A, freelist)│
    │ - 解冻 Slab A (frozen = 0)          │
    │ - 合并 c->freelist 到 slab->freelist│
    │ - 加入 node->partial (nid=0)       │ ← 回到 NUMA 0 的 partial
    └─────────────────────────────────────┘

T6: 从 NUMA 1 获取新的 Slab
    ┌─────────────────────────────────────┐
    │ get_partial(s, node=1, &pc)         │
    │ → 从 node[1]->partial 获取 Slab B   │
    │                                     │
    │ freeze_slab(s, Slab B)              │
    │ → Slab B (nid=1) frozen = 1         │
    │                                     │
    │ c->slab = Slab B (nid=1)            │ ← 新的 slab
    │ c->freelist = Slab B 的对象         │
    └─────────────────────────────────────┘

T7: 后续分配（在 NUMA 1 上）
    ┌─────────────────────────────────────┐
    │ CPU 0 (NUMA 1)                      │
    │ c->slab → Slab B (nid=1)            │ ← 新的 slab
    │ c->freelist → Object X → ...        │
    │                                     │
    │ 检查: node_match(Slab B, 1)        │
    │ → slab_nid(Slab B) = 1 == 1        │
    │ → 返回 1（匹配）                     │
    │                                     │
    │ → Fast Path 分配                    │
    └─────────────────────────────────────┘
```

---

## 四、关键代码位置

### 4.1 Fast Path 检查

**位置**: `mm/slub.c:3975-3980`

```c
static __always_inline void *__slab_alloc_node(...)
{
    c = raw_cpu_ptr(s->cpu_slab);
    object = c->freelist;
    slab = c->slab;
    
    // ✅ 每次分配时都检查 node 匹配
    if (!USE_LOCKLESS_FAST_PATH() ||
        unlikely(!object || !slab || !node_match(slab, node))) {
        // 如果不匹配，进入 Slow Path
        object = __slab_alloc(s, gfpflags, node, addr, c, orig_size);
    } else {
        // Fast Path：无锁快速分配
        void *next_object = get_freepointer_safe(s, object);
        __update_cpu_freelist_fast(s, object, next_object, tid);
        return object;
    }
}
```

### 4.2 Slow Path 检查

**位置**: `mm/slub.c:3692-3702`

```c
static void *___slab_alloc(...)
{
    slab = READ_ONCE(c->slab);
    
    if (!slab) {
        // 没有 slab，分配新的
        goto new_slab;
    }
    
    // ✅ 检查 NUMA 节点匹配
    if (unlikely(!node_match(slab, node))) {
        /*
         * same as above but node_match() being false already
         * implies node != NUMA_NO_NODE
         */
        if (!node_isset(node, slab_nodes)) {
            node = NUMA_NO_NODE;
        } else {
            stat(s, ALLOC_NODE_MISMATCH);  // ← 统计 NUMA 不匹配
            goto deactivate_slab;  // ← 解冻并转移旧的 slab
        }
    }
    
    // 继续处理...
}
```

### 4.3 解冻并转移

**位置**: `mm/slub.c:3750-3762`

```c
deactivate_slab:
    local_lock_irqsave(&s->cpu_slab->lock, flags);
    if (slab != c->slab) {
        local_unlock_irqrestore(&s->cpu_slab->lock, flags);
        goto reread_slab;
    }
    freelist = c->freelist;
    c->slab = NULL;  // ← 清空 CPU 的 slab
    c->freelist = NULL;
    c->tid = next_tid(c->tid);
    local_unlock_irqrestore(&s->cpu_slab->lock, flags);
    
    // 解冻并转移到 node->partial
    deactivate_slab(s, slab, freelist);
```

### 4.4 从新 node 获取 slab

**位置**: `mm/slub.c:3817-3833`

```c
new_objects:
    pc.flags = gfpflags;
    pc.orig_size = orig_size;
    
    // 从新的 node 获取 partial slab
    slab = get_partial(s, node, &pc);
    if (slab) {
        if (kmem_cache_debug(s)) {
            // 调试模式
            return freelist;
        }
        
        // ✅ 冻结新的 slab
        freelist = freeze_slab(s, slab);
        goto retry_load_slab;
    }
    
    // 如果 get_partial() 失败，从伙伴系统分配新 slab
    slab = new_slab(s, pc.flags, node);
```

---

## 五、如何确定目标 node？

### 5.1 分配时确定 node

**位置**: `mm/slub.c:4127-4141`

```c
static __fastpath_inline void *slab_alloc_node(struct kmem_cache *s,
        struct list_lru *lru, gfp_t gfpflags, int node,
        unsigned long addr, size_t orig_size)
{
    void *object;
    bool init = false;
    
    s = slab_pre_alloc_hook(s, gfpflags);
    if (unlikely(!s))
        return NULL;
    
    // ✅ 如果没有指定 node，使用当前 CPU 的 node
    if (node == NUMA_NO_NODE)
        node = numa_node_id();  // ← 获取当前 CPU 的 NUMA 节点
    
    object = __slab_alloc_node(s, gfpflags, node, addr, orig_size);
    // ...
}
```

**关键理解**：
- **如果 `node == NUMA_NO_NODE`，使用 `numa_node_id()` 获取当前 CPU 的 NUMA 节点**
- **`numa_node_id()` 返回当前 CPU 所在的 NUMA 节点**

### 5.2 CPU 迁移时 node 的变化

```
CPU 在 NUMA 0 上：
  └─ numa_node_id() = 0
  └─ 分配请求: node = 0
  └─ 检查: node_match(slab, 0)

CPU 迁移到 NUMA 1：
  └─ numa_node_id() = 1  ← 变化了
  └─ 分配请求: node = 1
  └─ 检查: node_match(slab, 1)
  └─ 如果 slab->nid = 0，不匹配
  └─ 触发 deactivate_slab()
```

---

## 六、设计优势

### 6.1 自动适应 CPU 迁移

- **不需要显式的迁移通知**：每次分配时自动检查
- **自动切换 slab**：如果 node 不匹配，自动切换到新的 node
- **NUMA 本地性**：确保 CPU 使用本地 node 的 slab

### 6.2 性能优化

- **Fast Path**：如果 node 匹配，无锁快速分配
- **Slow Path**：如果 node 不匹配，解冻并转移旧的 slab，获取新的 slab
- **统计信息**：`ALLOC_NODE_MISMATCH` 统计 NUMA 不匹配的次数

### 6.3 内存管理

- **旧的 slab 回到原来的 node**：`deactivate_slab()` 会将 slab 放回原来的 node->partial
- **新的 slab 从新的 node 获取**：`get_partial()` 会从新的 node->partial 获取
- **避免跨 node 访问**：减少跨 node 的内存访问延迟

---

## 七、完整示例

### 7.1 场景：CPU 从 NUMA 0 迁移到 NUMA 1

```
初始状态：
┌─────────────────────────────────────┐
│ CPU 0 (NUMA 0)                      │
│ c->slab → Slab A (nid=0)            │
│ c->freelist → Object 0 → Object 1   │
└─────────────────────────────────────┘

CPU 迁移到 NUMA 1：
┌─────────────────────────────────────┐
│ CPU 0 (NUMA 1)  ← 迁移              │
│ c->slab → Slab A (nid=0)            │ ← 还是旧的
│ c->freelist → Object 0 → Object 1   │
└─────────────────────────────────────┘

第一次分配（触发切换）：
┌─────────────────────────────────────┐
│ kmalloc(100, GFP_KERNEL)            │
│   └─ slab_alloc_node()              │
│       └─ node = numa_node_id() = 1 │
│       └─ __slab_alloc_node()       │
│           └─ 检查: node_match(Slab A, 1)│
│               └─ 不匹配！            │
│               └─ ___slab_alloc()   │
│                   └─ deactivate_slab()│
│                       └─ 解冻 Slab A│
│                       └─ 加入 node[0]->partial│
│                   └─ get_partial(s, node=1)│
│                       └─ 从 node[1]->partial 获取 Slab B│
│                       └─ freeze_slab(Slab B)│
│                           └─ Slab B (nid=1) frozen = 1│
│                   └─ c->slab = Slab B│
│                   └─ c->freelist = Slab B 的对象│
└─────────────────────────────────────┘

后续分配（Fast Path）：
┌─────────────────────────────────────┐
│ CPU 0 (NUMA 1)                      │
│ c->slab → Slab B (nid=1)            │ ← 新的 slab
│ c->freelist → Object X → ...        │
│                                     │
│ 检查: node_match(Slab B, 1)        │
│ → 匹配！                            │
│ → Fast Path 分配                    │
└─────────────────────────────────────┘
```

---

## 八、关键代码位置总结

| 操作 | 函数 | 文件 | 行号 |
|------|------|------|------|
| Fast Path 检查 | `__slab_alloc_node()` | `mm/slub.c` | 3975-3980 |
| Slow Path 检查 | `___slab_alloc()` | `mm/slub.c` | 3692-3702 |
| Node 匹配检查 | `node_match()` | `mm/slub.c` | 3394-3401 |
| 解冻并转移 | `deactivate_slab()` | `mm/slub.c` | 3047-3125 |
| 从新 node 获取 | `get_partial()` | `mm/slub.c` | 3817-3833 |
| 获取当前 node | `numa_node_id()` | - | - |

---

## 九、总结

### 9.1 关键点

1. **每次分配时都会检查 NUMA 节点匹配**
   - **Fast Path**：检查 `node_match(slab, node)`
   - **如果不匹配，进入 Slow Path**

2. **CPU 迁移时的处理**：
   - **检测到 node 不匹配**：`slab->nid != node`
   - **解冻并转移旧的 slab**：`deactivate_slab()` 将旧的 slab 放回原来的 node->partial
   - **从新的 node 获取 slab**：`get_partial()` 从新的 node->partial 获取
   - **冻结新的 slab**：`freeze_slab()` 将新的 slab 设置为 frozen

3. **自动适应**：
   - **不需要显式的迁移通知**：每次分配时自动检查
   - **自动切换 slab**：如果 node 不匹配，自动切换到新的 node
   - **NUMA 本地性**：确保 CPU 使用本地 node 的 slab

### 9.2 设计优势

- **自动适应**：不需要显式的迁移通知
- **性能优化**：Fast Path 无锁快速分配
- **NUMA 本地性**：确保 CPU 使用本地 node 的 slab
- **统计信息**：`ALLOC_NODE_MISMATCH` 统计 NUMA 不匹配的次数

---

**核心理解**：**每次分配时都会检查 NUMA 节点匹配**。如果当前 slab 的 node 不匹配（比如 CPU 迁移了），会触发 `deactivate_slab()` 解冻并转移旧的 slab，然后从新的 node 获取新的 slab。这个过程是**自动的**，不需要显式的迁移通知。

