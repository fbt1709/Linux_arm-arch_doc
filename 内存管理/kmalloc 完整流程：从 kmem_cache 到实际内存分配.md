# kmalloc 完整流程：从 kmem_cache 到实际内存分配

## 一、核心理解

**`kmem_cache` 不是一个"地址空间"，而是一个"管理结构"**。它包含了：
- 对象大小、对齐等配置信息
- **指向实际 slab 页面的指针**（通过 `cpu_slab` 和 `node[]`）
- 管理这些 slab 页面的数据结构

**实际的地址空间在 slab 页面中**，这些页面是从伙伴系统分配的。

---

## 二、kmalloc 完整流程

### 2.1 调用入口

```c
kmalloc(size, flags)
  → alloc_hooks(kmalloc_noprof(size, flags))
    → __kmalloc_noprof(size, flags)
      → __do_kmalloc_node(size, NULL, flags, NUMA_NO_NODE, _RET_IP_)
```

**位置**: `mm/slub.c:4293-4296`

---

### 2.2 选择对应的 kmem_cache

**位置**: `include/linux/slab.h:413-424`

```c
static inline struct kmem_cache *
kmalloc_slab(size_t size, kmem_buckets *b, gfp_t flags, unsigned long caller)
{
    unsigned int index;
    
    // 1. 如果没有传入 b，根据 flags 选择对应的 kmalloc_caches 行
    if (!b)
        b = &kmalloc_caches[kmalloc_type(flags, caller)];
    
    // 2. 根据 size 计算 index
    if (size <= 192)
        index = kmalloc_size_index[size_index_elem(size)];
    else
        index = fls(size - 1);
    
    // 3. 返回对应的 kmem_cache 指针
    return (*b)[index];  // ← 这里返回的是 kmem_cache 描述符
}
```

**关键理解**:
- `kmalloc_caches` 是一个二维数组：`kmem_buckets kmalloc_caches[NR_KMALLOC_TYPES]`
- `kmem_buckets` 是 `struct kmem_cache *[KMALLOC_SHIFT_HIGH + 1]`
- `(*b)[index]` 返回的是预先创建好的 `kmem_cache` 指针（如 `kmalloc-32`、`kmalloc-64` 等）
- **这个 `kmem_cache` 只是一个"模板"**，描述了对象大小等信息

---

### 2.3 使用 kmem_cache 进行实际分配

**位置**: `mm/slub.c:4263-4285`

```c
static __always_inline
void *__do_kmalloc_node(size_t size, kmem_buckets *b, gfp_t flags, int node,
            unsigned long caller)
{
    struct kmem_cache *s;
    void *ret;
    
    // 1. 大块内存直接走伙伴系统
    if (unlikely(size > KMALLOC_MAX_CACHE_SIZE)) {
        ret = __kmalloc_large_node_noprof(size, flags, node);
        return ret;
    }
    
    // 2. 选择对应的 kmem_cache
    s = kmalloc_slab(size, b, flags, caller);
    
    // 3. 从该 kmem_cache 分配对象（关键步骤！）
    ret = slab_alloc_node(s, NULL, flags, node, caller, size);
    
    // 4. KASAN 等安全检查
    ret = kasan_kmalloc(s, ret, size, flags);
    
    return ret;
}
```

---

## 三、slab_alloc_node：真正分配内存的地方

**位置**: `mm/slub.c:4127-4156`

```c
static __fastpath_inline void *slab_alloc_node(struct kmem_cache *s, ...)
{
    void *object;
    
    // 1. Fast Path：从 per-CPU freelist 快速分配
    object = __slab_alloc_node(s, gfpflags, node, addr, orig_size);
    
    // 2. 后续处理（初始化、KASAN 等）
    // ...
    
    return object;
}
```

---

## 四、`__slab_alloc_node`：Fast Path vs Slow Path

**位置**: `mm/slub.c:3934-4007`

### 4.1 Fast Path（快速路径）

```c
static __always_inline void *__slab_alloc_node(...)
{
    struct kmem_cache_cpu *c;
    struct slab *slab;
    void *object;
    
    // 1. 获取当前 CPU 的 cpu_slab（关键！）
    c = raw_cpu_ptr(s->cpu_slab);
    
    // 2. 读取 per-CPU 的 freelist
    object = c->freelist;      // ← 这里！对象已经在 slab 页面中了
    slab = c->slab;            // ← slab 页面指针
    
    // 3. 如果 fast path 可用，直接返回对象
    if (likely(object && slab && node_match(slab, node))) {
        // 无锁快速分配
        void *next_object = get_freepointer_safe(s, object);
        __update_cpu_freelist_fast(s, object, next_object, tid);
        return object;  // ← 返回对象指针
    }
    
    // 4. Fast Path 失败，进入 Slow Path
    object = __slab_alloc(s, gfpflags, node, addr, c, orig_size);
    return object;
}
```

**关键理解**:
- `c->slab` 指向一个**已经存在的 slab 页面**（这个页面之前已经从伙伴系统分配了）
- `c->freelist` 指向该 slab 页面中的**第一个空闲对象**
- **对象已经存在于 slab 页面中**，fast path 只是更新指针，无需分配新页面

---

### 4.2 Slow Path（慢速路径）

**位置**: `mm/slub.c:3667-3906`

当 Fast Path 失败时（`c->freelist` 为空或 `c->slab` 为 NULL），进入 Slow Path：

#### 步骤 1: 尝试从当前 slab 获取对象

```c
static void *___slab_alloc(...)
{
    struct kmem_cache_cpu *c;
    struct slab *slab;
    
    // 检查当前 CPU 是否有 slab
    slab = READ_ONCE(c->slab);
    if (!slab)
        goto new_slab;  // ← 没有 slab，需要获取新的
    
    // 尝试从当前 slab 的 freelist 获取对象
    freelist = get_freelist(s, slab);
    if (freelist)
        return freelist;  // ← 成功获取对象
}
```

#### 步骤 2: 从 node->partial 列表获取 slab

```c
new_objects:
    // 尝试从 node->partial 列表获取一个 partial slab
    slab = get_partial(s, node, &pc);
    if (slab) {
        // 从该 slab 获取对象
        freelist = freeze_slab(s, slab);
        // 设置 c->slab 和 c->freelist
        c->slab = slab;
        c->freelist = get_freepointer(s, freelist);
        return freelist;
    }
```

**关键理解**:
- `get_partial()` 从 `kmem_cache_node->partial` 列表获取一个**已经存在的 slab 页面**
- 这些 slab 页面之前已经从伙伴系统分配，现在只是"重用"

#### 步骤 3: 从伙伴系统分配新的 slab 页面

如果 partial 列表也为空，需要**真正分配新页面**：

```c
    // 向伙伴系统申请新的 slab 页面
    slab = new_slab(s, pc.flags, node);
    
    if (unlikely(!slab))
        return NULL;  // ← 分配失败
    
    // 从新 slab 获取对象
    freelist = slab->freelist;
    slab->freelist = NULL;
    
    // 设置 per-CPU 数据
    c->slab = slab;
    c->freelist = get_freepointer(s, freelist);
    
    return freelist;  // ← 返回对象指针
```

**`new_slab()` → `allocate_slab()` → `alloc_slab_page()`**:

**位置**: `mm/slub.c:2417-2439`

```c
static inline struct slab *alloc_slab_page(gfp_t flags, int node,
        struct kmem_cache_order_objects oo)
{
    struct folio *folio;
    struct slab *slab;
    unsigned int order = oo_order(oo);
    
    // ← 关键！向伙伴系统申请物理页面
    if (node == NUMA_NO_NODE)
        folio = (struct folio *)alloc_pages(flags, order);
    else
        folio = (struct folio *)__alloc_pages_node(node, flags, order);
    
    if (!folio)
        return NULL;
    
    // 将 page 转换为 slab（实际上是复用同一块内存）
    slab = folio_slab(folio);
    
    // 标记为 slab 页面
    __folio_set_slab(folio);
    
    return slab;  // ← 返回 slab 页面指针
}
```

---

## 五、完整的对象分配流程

### 5.1 流程图

```
kmalloc(size, flags)
  │
  ├─ 1. 选择 kmem_cache
  │   └─ kmalloc_slab() → 返回 s = kmalloc_caches[type][index]
  │
  ├─ 2. 尝试 Fast Path
  │   ├─ c = raw_cpu_ptr(s->cpu_slab)  ← 获取 per-CPU 数据
  │   ├─ object = c->freelist          ← 对象已经在 slab 页面中！
  │   └─ 如果可用 → 直接返回 object
  │
  ├─ 3. Fast Path 失败，尝试 Slow Path
  │   ├─ 3a. 从当前 slab 获取
  │   │   └─ get_freelist(s, c->slab) → 返回对象
  │   │
  │   ├─ 3b. 从 node->partial 列表获取
  │   │   ├─ get_partial() → 从 node->partial 获取 slab
  │   │   └─ 从该 slab 获取对象 → 返回对象
  │   │
  │   └─ 3c. 从伙伴系统分配新页面（真正的地址空间分配！）
  │       ├─ new_slab() → allocate_slab() → alloc_slab_page()
  │       │   └─ alloc_pages(order)  ← 向伙伴系统申请物理页面
  │       ├─ 初始化 slab（建立 freelist）
  │       └─ 从新 slab 获取对象 → 返回对象
  │
  └─ 4. 返回对象指针（对象在 slab 页面中）
```

---

## 六、关键数据结构关系

### 6.1 kmem_cache 包含的指针

```c
struct kmem_cache {
    struct kmem_cache_cpu __percpu *cpu_slab;  // ← per-CPU 数据
    struct kmem_cache_node *node[MAX_NUMNODES]; // ← per-Node 数据
    // ... 其他配置信息
};
```

### 6.2 kmem_cache_cpu 指向实际的 slab 页面

```c
struct kmem_cache_cpu {
    struct slab *slab;      // ← 指向当前使用的 slab 页面（实际地址空间！）
    void *freelist;          // ← 指向 slab 页面中的第一个空闲对象
    // ...
};
```

### 6.3 kmem_cache_node 管理 partial slab 列表

```c
struct kmem_cache_node {
    struct list_head partial;  // ← 包含多个 slab 页面的链表（实际地址空间！）
    // ...
};
```

### 6.4 slab 页面包含对象

```c
struct slab {
    void *freelist;              // ← 指向 slab 页面中的空闲对象链表
    unsigned int inuse;          // ← 已使用对象数
    unsigned int objects;        // ← 总对象数
    struct kmem_cache *slab_cache; // ← 反向指针，指向所属的 kmem_cache
};
```

**关键理解**:
- `struct slab` **复用** `struct page` 的内存空间
- slab 页面的**物理地址**是从伙伴系统分配的
- 对象就**存储在这个 slab 页面的虚拟地址空间**中

---

## 七、对象在 slab 页面中的布局

```
Slab 页面（2^order 张物理页，例如 1 页 = 4KB）

┌─────────────────────────────────────┐
│ struct page (复用为 struct slab)    │ ← page 描述符
├─────────────────────────────────────┤
│ Object 0  (32 字节)                  │ ← 实际对象
│ Object 1  (32 字节)                  │
│ Object 2  (32 字节)                  │
│ ...                                  │
│ Object N  (32 字节)                  │
└─────────────────────────────────────┘
         ↑
         │
    freelist 指向 Object 0，Object 0 内部有 next 指针指向 Object 1
```

**对象分配过程**:
1. `c->freelist` 指向 Object 0
2. 返回 Object 0 的地址给用户
3. 更新 `c->freelist = Object 0->next`（指向 Object 1）

---

## 八、总结

### 8.1 关键点

1. **`kmem_cache` 是模板**，不是地址空间
   - 它描述了对象大小、对齐等配置
   - 它包含指向实际 slab 页面的指针

2. **实际的地址空间在 slab 页面中**
   - slab 页面是从伙伴系统分配的（`alloc_pages()`）
   - 对象就存储在这些 slab 页面中

3. **分配流程**
   - Fast Path: 从已有的 slab 页面（`c->slab`）获取对象
   - Slow Path: 
     - 优先从 `node->partial` 列表获取已有 slab
     - 最后才向伙伴系统申请新页面

4. **`kmalloc_slab()` 的作用**
   - 只是选择一个合适的 `kmem_cache`（模板）
   - 真正的分配由 `slab_alloc_node()` 完成

### 8.2 完整调用链

```
kmalloc(size, flags)
  ↓
__do_kmalloc_node()
  ├─ kmalloc_slab() → 返回 kmem_cache *s（模板）
  └─ slab_alloc_node(s, ...)
      └─ __slab_alloc_node(s, ...)
          ├─ Fast Path: c->freelist（对象已在 slab 页面中）
          └─ Slow Path: 
              ├─ get_partial()（从已有 slab 获取）
              └─ new_slab() → alloc_pages()（分配新页面）
                  └─ 从新 slab 获取对象
```

---

## 九、代码位置总结

| 功能 | 文件 | 函数 | 行号 |
|------|------|------|------|
| kmalloc 入口 | `include/linux/slab.h` | `kmalloc_noprof()` | 869-883 |
| 选择 kmem_cache | `include/linux/slab.h` | `kmalloc_slab()` | 413-424 |
| 通用分配函数 | `mm/slub.c` | `__do_kmalloc_node()` | 4263-4285 |
| 分配入口 | `mm/slub.c` | `slab_alloc_node()` | 4127-4156 |
| Fast/Slow Path | `mm/slub.c` | `__slab_alloc_node()` | 3934-4007 |
| Slow Path 实现 | `mm/slub.c` | `___slab_alloc()` | 3667-3906 |
| 分配新 slab | `mm/slub.c` | `new_slab()` | 2642-2651 |
| 向伙伴系统申请 | `mm/slub.c` | `alloc_slab_page()` | 2417-2439 |

---

通过以上流程，可以清楚地看到：
- `kmem_cache` 只是一个"模板"，描述了如何管理对象
- 真正的地址空间分配发生在 `alloc_pages()`（向伙伴系统申请 slab 页面）
- 对象存储在 slab 页面中，通过 freelist 链接
- Fast Path 只是从已有 slab 获取对象，无需分配新页面

