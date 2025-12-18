# `kmem_cache` 和 `kmem_cache_node` 分别管理什么

## 一、核心概念

在 SLUB 分配器中，`kmem_cache` 和 `kmem_cache_node` 是两个不同层次的管理结构：

- **`kmem_cache`**: 管理一个**特定大小**的对象分配器（Cache 级别）
- **`kmem_cache_node`**: 管理**某个 NUMA 节点**上的 slab 列表（Node 级别）

---

## 二、`kmem_cache` - Cache 描述符

### 2.1 管理职责

`kmem_cache` 是一个**全局的 Cache 描述符**，它管理**所有 NUMA 节点**上、**所有 CPU** 上、**同一大小**的对象分配。

### 2.2 核心字段

**位置**: `mm/slab.h:258`

```c
struct kmem_cache {
    /* === Per-CPU 快速路径数据 === */
    struct kmem_cache_cpu __percpu *cpu_slab;  // 每个 CPU 的 slab 指针
    
    /* === Cache 配置参数 === */
    unsigned int size;                          // 对象大小（含元数据）
    unsigned int object_size;                  // 对象实际大小（不含元数据）
    slab_flags_t flags;                        // Cache 标志位
    unsigned int align;                        // 对齐要求
    
    /* === Per-Node 数据 === */
    struct kmem_cache_node *node[MAX_NUMNODES];  // 每个 NUMA 节点的管理结构
    
    /* === 其他配置 === */
    const char *name;                          // Cache 名称
    struct list_head list;                     // 全局 slab_caches 链表
    // ...
};
```

### 2.3 管理内容

1. **对象大小和配置**
   - 定义该 cache 分配的对象大小（如 32 字节、64 字节等）
   - 定义对齐要求、标志位等

2. **Per-CPU 快速路径**
   - 每个 CPU 有一个 `kmem_cache_cpu` 结构
   - 存储当前 CPU 正在使用的 slab 和 freelist
   - 用于快速无锁分配/释放

3. **Per-Node 管理结构**
   - 通过 `node[]` 数组指向每个 NUMA 节点的 `kmem_cache_node`
   - 每个节点有自己独立的 partial/full slab 列表

### 2.4 使用场景

- **一个 `kmem_cache` 对应一种对象大小**
  - 例如：`kmalloc-32`、`kmalloc-64`、`task_struct` cache 等
- **全局唯一**
  - 所有 CPU、所有 NUMA 节点共享同一个 `kmem_cache` 结构
- **管理所有节点**
  - 通过 `node[]` 数组访问各个节点的管理结构

---

## 三、`kmem_cache_node` - Node 管理结构

### 3.1 管理职责

`kmem_cache_node` 是一个**节点级别的管理结构**，它管理**某个特定 NUMA 节点**上、**某个特定 Cache** 的 slab 列表。

### 3.2 核心字段

**位置**: `mm/slub.c:425`

```c
struct kmem_cache_node {
    spinlock_t list_lock;                      // 保护 partial/full 列表的锁
    unsigned long nr_partial;                   // partial slab 的数量
    struct list_head partial;                   // partial slab 链表
    
#ifdef CONFIG_SLUB_DEBUG
    atomic_long_t nr_slabs;                     // 该节点上的 slab 总数
    atomic_long_t total_objects;                // 该节点上的对象总数
    struct list_head full;                      // full slab 链表（调试模式）
#endif
};
```

### 3.3 管理内容

1. **Partial Slab 列表**
   - 存储**部分使用**的 slab（还有空闲对象）
   - 当 CPU slab 耗尽时，从这里获取新的 slab
   - 当 slab 从 full 变为 partial 时，加入此列表

2. **Full Slab 列表**（调试模式）
   - 存储**完全使用**的 slab（没有空闲对象）
   - 仅在 `CONFIG_SLUB_DEBUG` 启用时使用

3. **统计信息**
   - `nr_partial`: 当前 partial slab 的数量
   - `nr_slabs`: 该节点上的 slab 总数
   - `total_objects`: 该节点上的对象总数

### 3.4 使用场景

- **每个 Cache × 每个 Node = 一个 `kmem_cache_node`**
  - 例如：`kmalloc-32` 在 Node 0 有一个 `kmem_cache_node`
  - `kmalloc-32` 在 Node 1 有另一个 `kmem_cache_node`
- **管理节点本地内存**
  - 只管理从该节点分配的内存
  - 支持 NUMA 本地性优化

---

## 四、关系图

```
全局层面:
└── slab_caches (list_head)
    └── kmem_cache (kmalloc-32)
        ├── cpu_slab[CPU0] → kmem_cache_cpu
        ├── cpu_slab[CPU1] → kmem_cache_cpu
        ├── ...
        ├── node[0] → kmem_cache_node (Node 0)
        │   ├── partial → [slab1, slab2, ...]
        │   └── full → [slab3, ...]
        ├── node[1] → kmem_cache_node (Node 1)
        │   ├── partial → [slab4, slab5, ...]
        │   └── full → [slab6, ...]
        └── ...
```

---

## 五、分配流程中的角色

### 5.1 Fast Path（快速路径）

```c
// 1. 从 kmem_cache 获取当前 CPU 的 cpu_slab
struct kmem_cache_cpu *c = raw_cpu_ptr(s->cpu_slab);

// 2. 直接从 cpu_slab->freelist 分配（无锁）
if (c->freelist) {
    object = c->freelist;
    c->freelist = get_freepointer(s, object);
    return object;
}
```

**`kmem_cache` 的作用**: 提供 per-CPU 快速路径

---

### 5.2 Slow Path（慢速路径）

```c
// 1. 从 kmem_cache 获取当前节点的 node
struct kmem_cache_node *n = get_node(s, node);

// 2. 从 node->partial 列表获取一个 partial slab
slab = get_partial_node(s, n, &pc);

// 3. 如果 partial 列表为空，从伙伴系统分配新 slab
if (!slab)
    slab = new_slab(s, gfpflags, node);
```

**`kmem_cache_node` 的作用**: 管理节点上的 partial slab 列表

---

## 六、释放流程中的角色

### 6.1 Fast Path

```c
// 释放到当前 CPU 的 slab（无锁）
if (slab == c->slab) {
    set_freepointer(s, object, c->freelist);
    c->freelist = object;
    return;
}
```

**`kmem_cache` 的作用**: 提供 per-CPU 快速释放路径

---

### 6.2 Slow Path

```c
// 1. 从 kmem_cache 获取 slab 所在节点的 node
struct kmem_cache_node *n = get_node(s, slab_nid(slab));

// 2. 如果 slab 变为 partial，加入 node->partial 列表
if (slab->inuse < slab->objects) {
    spin_lock_irqsave(&n->list_lock, flags);
    add_partial(n, slab, DEACTIVATE_TO_TAIL);
    n->nr_partial++;
    spin_unlock_irqrestore(&n->list_lock, flags);
}

// 3. 如果 slab 变为空，且 nr_partial >= min_partial，释放回伙伴系统
if (slab->inuse == 0 && n->nr_partial >= s->min_partial) {
    remove_partial(n, slab);
    discard_slab(s, slab);
}
```

**`kmem_cache_node` 的作用**: 管理节点上的 partial slab 列表，决定 slab 的去留

---

## 七、总结对比

| 特性 | `kmem_cache` | `kmem_cache_node` |
|------|--------------|-------------------|
| **管理范围** | 全局（所有 CPU、所有 Node） | 单个 Node |
| **管理对象** | 一种对象大小的 Cache | 某个 Node 上的 Slab 列表 |
| **数量关系** | 一个 Cache = 一个 `kmem_cache` | 一个 Cache × 一个 Node = 一个 `kmem_cache_node` |
| **主要职责** | Cache 配置、Per-CPU 快速路径 | Node 上的 Partial/Full Slab 管理 |
| **访问方式** | 全局唯一，通过名称查找 | 通过 `kmem_cache->node[node_id]` 访问 |
| **锁保护** | Per-CPU 无锁（fast path） | `list_lock` 保护 partial/full 列表 |
| **典型操作** | `kmem_cache_alloc()` | `get_partial_node()`、`add_partial()` |

---

## 八、代码示例

### 8.1 创建 Cache 时初始化 Node

```c
// mm/slub.c:5241
static int init_kmem_cache_nodes(struct kmem_cache *s)
{
    int node;
    
    for_each_node_mask(node, slab_nodes) {
        struct kmem_cache_node *n;
        
        // 为每个节点分配一个 kmem_cache_node
        n = kmem_cache_alloc_node(kmem_cache_node, GFP_KERNEL, node);
        
        // 初始化 node 结构
        init_kmem_cache_node(n);
        
        // 将 node 挂到 kmem_cache 的 node[] 数组
        s->node[node] = n;
    }
    return 1;
}
```

### 8.2 从 Node 获取 Partial Slab

```c
// mm/slub.c:2828
static struct slab *get_partial_node(struct kmem_cache *s,
                                     struct kmem_cache_node *n,
                                     struct partial_context *pc)
{
    struct slab *slab;
    unsigned long flags;
    
    if (!n || !n->nr_partial)
        return NULL;
    
    // 加锁保护 partial 列表
    spin_lock_irqsave(&n->list_lock, flags);
    
    // 从 partial 列表获取一个 slab
    list_for_each_entry(slab, &n->partial, slab_list) {
        remove_partial(n, slab);
        n->nr_partial--;
        break;
    }
    
    spin_unlock_irqrestore(&n->list_lock, flags);
    return slab;
}
```

### 8.3 将 Slab 加入 Node 的 Partial 列表

```c
// mm/slub.c:2725 (简化版)
static void add_partial(struct kmem_cache_node *n, struct slab *slab, int tail)
{
    if (tail)
        list_add_tail(&slab->slab_list, &n->partial);
    else
        list_add(&slab->slab_list, &n->partial);
    n->nr_partial++;
}
```

---

## 九、关键理解点

1. **`kmem_cache` 是全局的，`kmem_cache_node` 是节点本地的**
   - 一个 `kmem_cache` 可以管理多个节点的内存
   - 每个节点有自己独立的 `kmem_cache_node`

2. **`kmem_cache` 提供快速路径，`kmem_cache_node` 提供慢速路径**
   - Fast path: 直接使用 `kmem_cache->cpu_slab`
   - Slow path: 从 `kmem_cache->node[node_id]->partial` 获取

3. **`kmem_cache_node` 是 `kmem_cache` 的组成部分**
   - `kmem_cache->node[]` 数组存储各个节点的 `kmem_cache_node`
   - 通过 `get_node(s, node_id)` 访问

4. **两者配合实现 NUMA 感知**
   - `kmem_cache` 知道所有节点的信息
   - `kmem_cache_node` 只管理本地节点的 slab
   - 分配时优先使用本地节点的 slab

---

通过以上分析，可以清楚地看到：
- **`kmem_cache`**: 管理**一种对象大小**的全局 Cache
- **`kmem_cache_node`**: 管理**某个节点**上该 Cache 的 Slab 列表

两者配合，实现了高效的、NUMA 感知的内存分配。

