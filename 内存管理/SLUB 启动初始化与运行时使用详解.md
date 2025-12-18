# SLUB 启动初始化与运行时使用详解

本文档详细解释 SLUB 在启动过程中做了什么，以及运行时如何使用 slab 分配器。

---

## 一、启动过程中的 Slab 初始化

### 1.1 调用时机

`kmem_cache_init()` 在 `start_kernel()` → `mm_init()` → `kmem_cache_init()` 中被调用，此时：
- 伙伴系统已经可用（`mem_init()` 已调用）
- 但 slab 分配器本身还未初始化
- 需要使用**静态分配的临时结构**来"自举"（bootstrap）

### 1.2 初始化流程详解

#### 阶段 1: 准备静态临时结构

**位置**: `mm/slub.c:5909`

```c
void __init kmem_cache_init(void)
{
    // 1. 声明两个静态的临时 kmem_cache 结构
    static __initdata struct kmem_cache boot_kmem_cache,
        boot_kmem_cache_node;
    
    // 2. 将全局指针指向这些静态结构
    kmem_cache_node = &boot_kmem_cache_node;  // 用于分配 kmem_cache_node
    kmem_cache = &boot_kmem_cache;            // 用于分配 kmem_cache
```

**为什么需要静态结构？**
- 此时 slab 分配器还未初始化，无法使用 `kmem_cache_alloc()` 分配 `kmem_cache`
- 必须使用静态分配的 `boot_kmem_cache` 和 `boot_kmem_cache_node` 作为"临时容器"
- 这些结构在编译时分配在 `.init.data` 段，启动完成后会被丢弃

---

#### 阶段 2: 初始化 `kmem_cache_node` Cache

**位置**: `mm/slub.c:5932`

```c
    // 3. 初始化 NUMA 节点掩码
    for_each_node_state(node, N_NORMAL_MEMORY)
        node_set(node, slab_nodes);
    
    // 4. 创建 kmem_cache_node 的 boot cache
    create_boot_cache(kmem_cache_node, "kmem_cache_node",
            sizeof(struct kmem_cache_node),
            SLAB_HWCACHE_ALIGN | SLAB_NO_OBJ_EXT, 0, 0);
```

**`create_boot_cache()` 做了什么？**

**位置**: `mm/slab_common.c:645`

```c
void __init create_boot_cache(struct kmem_cache *s, const char *name,
        unsigned int size, slab_flags_t flags,
        unsigned int useroffset, unsigned int usersize)
{
    // 1. 计算对齐要求
    unsigned int align = calculate_alignment(flags, align, size);
    
    // 2. 调用 do_kmem_cache_create() 初始化 s
    err = do_kmem_cache_create(s, name, size, &kmem_args, flags);
    
    // 3. 设置 refcount = -1，禁止合并
    s->refcount = -1;
}
```

**`do_kmem_cache_create()` 的关键步骤**:

1. **计算对象大小和 slab order** (`calculate_sizes()`)
   - 根据 `object_size` 计算 `size`（含元数据）
   - 计算最优的 `order`（每个 slab 包含多少页）
   - 计算 `offset`（freelist 指针在对象中的位置）

2. **初始化 per-CPU 数据** (`alloc_kmem_cache_cpus()`)
   - 为每个 CPU 分配 `kmem_cache_cpu` 结构
   - 初始化 `c->tid` 和 `c->lock`

3. **初始化 per-Node 数据** (`init_kmem_cache_nodes()`)
   - 为每个 NUMA 节点分配 `kmem_cache_node` 结构
   - 初始化 `n->list_lock`、`n->partial`、`n->full` 列表

**此时的状态**:
- `kmem_cache_node` 已经可以分配 `struct kmem_cache_node` 对象
- `slab_state = PARTIAL`（可以分配 node 结构，但还不能分配其他对象）

---

#### 阶段 3: 初始化 `kmem_cache` Cache

**位置**: `mm/slub.c:5941`

```c
    // 5. 创建 kmem_cache 的 boot cache
    create_boot_cache(kmem_cache, "kmem_cache",
            offsetof(struct kmem_cache, node) +
                nr_node_ids * sizeof(struct kmem_cache_node *),
            SLAB_HWCACHE_ALIGN | SLAB_NO_OBJ_EXT, 0, 0);
```

**注意**: 这里传入的 `size` 是 `kmem_cache` 结构的大小（包含 `node[]` 数组），而不是 `sizeof(struct kmem_cache)`。

**此时的状态**:
- `kmem_cache` 已经可以分配 `struct kmem_cache` 对象
- 但此时 `kmem_cache` 和 `kmem_cache_node` 还是指向静态的 `boot_kmem_cache` 和 `boot_kmem_cache_node`

---

#### 阶段 4: Bootstrap - 用动态分配替换静态结构

**位置**: `mm/slub.c:5946`

```c
    // 6. 关键步骤：用动态分配的 kmem_cache 替换静态的
    kmem_cache = bootstrap(&boot_kmem_cache);
    kmem_cache_node = bootstrap(&boot_kmem_cache_node);
```

**`bootstrap()` 做了什么？**

**位置**: `mm/slub.c:5880`

```c
static struct kmem_cache * __init bootstrap(struct kmem_cache *static_cache)
{
    int node;
    struct kmem_cache *s;
    struct kmem_cache_node *n;
    
    // 1. 使用已经初始化的 kmem_cache 分配一个新的 kmem_cache
    s = kmem_cache_zalloc(kmem_cache, GFP_NOWAIT);
    
    // 2. 将静态结构的内容复制到新分配的结构
    memcpy(s, static_cache, kmem_cache->object_size);
    
    // 3. 更新所有已分配的 slab 中的 slab_cache 指针
    for_each_kmem_cache_node(s, node, n) {
        struct slab *p;
        list_for_each_entry(p, &n->partial, slab_list)
            p->slab_cache = s;  // 指向新的动态分配的 cache
    }
    
    // 4. 将新的 cache 加入全局链表
    list_add(&s->list, &slab_caches);
    
    return s;  // 返回动态分配的 kmem_cache
}
```

**Bootstrap 的关键点**:
1. **自举过程**: 使用已经初始化的 `kmem_cache` 来分配一个新的 `kmem_cache`，然后用新分配的替换静态的
2. **指针更新**: 所有之前分配的 slab 中的 `slab_cache` 指针都需要更新，指向新的动态分配的 cache
3. **结果**: `kmem_cache` 和 `kmem_cache_node` 现在指向动态分配的结构，静态的 `boot_kmem_cache` 和 `boot_kmem_cache_node` 不再使用

**为什么需要 bootstrap？**
- 静态结构在 `.init.data` 段，启动完成后会被释放
- 必须用动态分配的结构替换，才能让 slab 系统正常工作
- 这是一个"自举"过程：用自己来分配自己

---

#### 阶段 5: 创建 kmalloc Caches

**位置**: `mm/slub.c:5950`

```c
    // 7. 设置 kmalloc 大小索引表
    setup_kmalloc_cache_index_table();
    
    // 8. 创建所有 kmalloc caches
    create_kmalloc_caches();
```

**`create_kmalloc_caches()` 做了什么？**

**位置**: `mm/slab_common.c:944`

```c
void __init create_kmalloc_caches(void)
{
    int i;
    enum kmalloc_cache_type type;
    
    // 为每种类型（NORMAL, RECLAIM, DMA, CGROUP 等）创建 caches
    for (type = KMALLOC_NORMAL; type < NR_KMALLOC_TYPES; type++) {
        // 创建非 2 的幂次大小的 cache（32, 64 字节）
        if (KMALLOC_MIN_SIZE <= 32)
            new_kmalloc_cache(1, type);  // 32 字节
        if (KMALLOC_MIN_SIZE <= 64)
            new_kmalloc_cache(2, type);  // 64 字节
        
        // 创建 2 的幂次大小的 caches（8, 16, 32, 64, 128, ... KB）
        for (i = KMALLOC_SHIFT_LOW; i <= KMALLOC_SHIFT_HIGH; i++)
            new_kmalloc_cache(i, type);  // 2^i 字节
    }
    
    slab_state = UP;  // 现在 slab 系统完全可用了
}
```

**kmalloc caches 的作用**:
- `kmalloc()` 不是直接分配，而是根据大小选择对应的 `kmem_cache`
- 例如：`kmalloc(100, GFP_KERNEL)` → 选择 `kmalloc-128` cache
- 这样可以减少内存碎片，提高分配效率

**kmalloc cache 命名规则**:
- `kmalloc-8`, `kmalloc-16`, `kmalloc-32`, `kmalloc-64`, ...
- `kmalloc-1k`, `kmalloc-2k`, `kmalloc-4k`, ...
- 每种类型（NORMAL, RECLAIM, DMA）都有独立的 cache

---

#### 阶段 6: 其他初始化

**位置**: `mm/slub.c:5953`

```c
    // 9. 初始化 freelist 随机化
    init_freelist_randomization();
    
    // 10. 注册 CPU 热插拔回调
    cpuhp_setup_state_nocalls(CPUHP_SLUB_DEAD, "slub:dead", NULL,
                  slub_cpu_dead);
    
    // 11. 打印初始化信息
    pr_info("SLUB: HWalign=%d, Order=%u-%u, MinObjects=%u, CPUs=%u, Nodes=%u\n",
        cache_line_size(),
        slub_min_order, slub_max_order, slub_min_objects,
        nr_cpu_ids, nr_node_ids);
}
```

**初始化完成后的状态**:
- `slab_state = UP`（slab 系统完全可用）
- `kmem_cache` 和 `kmem_cache_node` 指向动态分配的结构
- 所有 kmalloc caches 已创建
- 可以正常使用 `kmalloc()` 和 `kmem_cache_alloc()`

---

### 1.3 启动初始化流程图

```
kmem_cache_init()
│
├─ 1. 声明静态结构
│  └─ boot_kmem_cache, boot_kmem_cache_node
│
├─ 2. 初始化 kmem_cache_node cache
│  └─ create_boot_cache(kmem_cache_node, ...)
│     ├─ do_kmem_cache_create()
│     │  ├─ calculate_sizes()        → 计算对象大小、order
│     │  ├─ alloc_kmem_cache_cpus() → 分配 per-CPU 数据
│     │  └─ init_kmem_cache_nodes() → 分配 per-Node 数据
│     └─ slab_state = PARTIAL
│
├─ 3. 初始化 kmem_cache cache
│  └─ create_boot_cache(kmem_cache, ...)
│     └─ 同上步骤
│
├─ 4. Bootstrap（自举）
│  └─ kmem_cache = bootstrap(&boot_kmem_cache)
│     ├─ s = kmem_cache_zalloc(kmem_cache, ...)  ← 用自己分配自己
│     ├─ memcpy(s, static_cache, ...)            ← 复制数据
│     └─ 更新所有 slab->slab_cache 指针
│
├─ 5. 创建 kmalloc caches
│  └─ create_kmalloc_caches()
│     └─ 创建 kmalloc-8, kmalloc-16, ..., kmalloc-1k, ...
│
└─ 6. 其他初始化
   ├─ init_freelist_randomization()
   ├─ cpuhp_setup_state_nocalls()
   └─ slab_state = UP
```

---

## 二、运行过程中如何使用 Slab

### 2.1 两种使用方式

#### 方式 1: kmalloc() - 通用内存分配

**使用场景**: 分配任意大小的内存，不需要特定的 cache

**调用链**:
```
kmalloc(size, flags)
  └─ __kmalloc_noprof(size, flags)
     └─ __do_kmalloc_node(size, ..., flags, node, caller)
        ├─ kmalloc_slab(size, ...)  → 根据大小选择 cache
        └─ slab_alloc_node(s, NULL, flags, node, caller, size)
```

**示例**:
```c
// 分配 100 字节
void *ptr = kmalloc(100, GFP_KERNEL);
// 内部会选择 kmalloc-128 cache

// 分配 2KB
void *ptr2 = kmalloc(2048, GFP_KERNEL);
// 内部会选择 kmalloc-2k cache

// 释放
kfree(ptr);
```

**kmalloc 大小映射**:
- `kmalloc(1-8)` → `kmalloc-8`
- `kmalloc(9-16)` → `kmalloc-16`
- `kmalloc(17-32)` → `kmalloc-32`
- `kmalloc(33-64)` → `kmalloc-64`
- `kmalloc(65-96)` → `kmalloc-96`（如果支持）
- `kmalloc(97-128)` → `kmalloc-128`
- ...
- `kmalloc(> KMALLOC_MAX_CACHE_SIZE)` → 直接从伙伴系统分配（大对象）

---

#### 方式 2: kmem_cache_alloc() - 专用 Cache 分配

**使用场景**: 频繁分配相同大小的对象，需要更高的性能

**步骤**:

**1. 创建专用 Cache**（通常在模块初始化时）:
```c
// 定义全局变量
static struct kmem_cache *my_cache;

// 在模块初始化函数中
static int __init my_module_init(void)
{
    // 创建专用的 cache，对象大小为 sizeof(struct my_struct)
    my_cache = kmem_cache_create("my_struct_cache",
                                  sizeof(struct my_struct),
                                  0,  // 对齐（0 表示使用默认）
                                  SLAB_HWCACHE_ALIGN,
                                  NULL);  // 构造函数（可选）
    if (!my_cache)
        return -ENOMEM;
    
    return 0;
}
```

**2. 分配对象**:
```c
struct my_struct *obj = kmem_cache_alloc(my_cache, GFP_KERNEL);
if (!obj)
    return -ENOMEM;
```

**3. 释放对象**:
```c
kmem_cache_free(my_cache, obj);
```

**4. 销毁 Cache**（通常在模块退出时）:
```c
static void __exit my_module_exit(void)
{
    kmem_cache_destroy(my_cache);
}
```

**调用链**:
```
kmem_cache_alloc(s, flags)
  └─ slab_alloc_node(s, NULL, flags, NUMA_NO_NODE, _RET_IP_, s->object_size)
     └─ __slab_alloc_node(...)
        ├─ Fast Path: 从 c->freelist 直接分配
        └─ Slow Path: 从 partial/full 列表或新分配 slab
```

---

### 2.2 运行时分配流程（Fast Path）

**位置**: `mm/slub.c:3934`

```c
static __always_inline void *__slab_alloc_node(...)
{
    struct kmem_cache_cpu *c;
    void *object;
    unsigned long tid;
    
    redo:
    // 1. 获取当前 CPU 的 cache_cpu 指针
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
            return object;  // ✅ 成功，无锁快速分配
        }
        note_cmpxchg_failure("slab_alloc", s, tid);
        goto redo;  // cmpxchg 失败，重试
    }
    
    // 4. Fast path 失败，进入慢路径
    object = __slab_alloc(s, gfpflags, node, addr, c, orig_size);
    return object;
}
```

**Fast Path 成功条件**:
1. `c->freelist` 非空（有可用对象）
2. `c->slab` 非空（有活跃的 slab）
3. `node_match()` 匹配（NUMA 亲和性正确）

**性能优势**: 整个过程**无锁**，使用 `cmpxchg_double` 原子操作，非常快。

---

### 2.3 运行时分配流程（Slow Path）

当 fast path 失败时（CPU slab 为空或 freelist 为空），进入慢路径：

**位置**: `mm/slub.c:3667`

```c
static void *___slab_alloc(...)
{
    reread_slab:
    slab = READ_ONCE(c->slab);
    
    // === 情况 1: CPU slab 存在但 freelist 为空 ===
    if (slab && node_match(slab, node)) {
        local_lock_irqsave(&c->lock, flags);
        freelist = get_freelist(s, slab);  // 从 slab->freelist 获取
        
        if (freelist) {
            c->freelist = get_freepointer(s, freelist);
            c->tid = next_tid(c->tid);
            return freelist;  // ✅ 成功从 slab 获取
        }
        
        // slab 已耗尽，需要新 slab
        c->slab = NULL;
        deactivate_slab(s, slab, NULL);  // 将旧 slab 放回 node partial
    }
    
    // === 情况 2: 从 CPU Partial 列表获取 ===
    #ifdef CONFIG_SLUB_CPU_PARTIAL
    while (slub_percpu_partial(c)) {
        slab = slub_percpu_partial(c);
        slub_set_percpu_partial(c, slab->next);
        
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

---

### 2.4 运行时释放流程

**入口**: `kmem_cache_free()` 或 `kfree()`

**调用链**:
```
kmem_cache_free(s, x)
  └─ slab_free(s, virt_to_slab(x), x, _RET_IP_)
     └─ do_slab_free(s, slab, object, object, 1, addr)
        ├─ Fast Path: 释放到当前 CPU 的 slab（无锁）
        └─ Slow Path: 释放到不同的 slab（需要锁）
```

**Fast Path Free**:
```c
static __always_inline void do_slab_free(...)
{
    c = raw_cpu_ptr(s->cpu_slab);
    tid = READ_ONCE(c->tid);
    
    if (likely(slab == c->slab)) {
        // 释放到当前 CPU 的冻结 slab
        freelist = READ_ONCE(c->freelist);
        set_freepointer(s, tail, freelist);  // 将对象加入 freelist
        
        // 无锁更新：cmpxchg_double(freelist, tid, head, tid+1)
        if (likely(__update_cpu_freelist_fast(s, freelist, head, tid))) {
            stat_add(s, FREE_FASTPATH, cnt);
            return;  // ✅ 成功，无锁快速释放
        }
        goto redo;
    }
    
    // Slow path：释放到不同的 slab
    __slab_free(s, slab, head, tail, cnt, addr);
}
```

**Slow Path Free**:
- 如果释放到不同的 slab，需要更新 `slab->freelist` 和 `slab->counters`
- 根据 slab 状态决定归宿：
  - 如果 slab 变空 → `discard_slab()`（归还给伙伴系统）
  - 如果 slab 从 full 变为 partial → 加入 `node->partial` 列表
  - 如果 slab 已经在 partial 列表 → 只更新状态

---

### 2.5 运行时使用示例

#### 示例 1: 使用 kmalloc

```c
// 分配
void *ptr = kmalloc(100, GFP_KERNEL);
if (!ptr)
    return -ENOMEM;

// 使用
memset(ptr, 0, 100);
// ... 使用 ptr ...

// 释放
kfree(ptr);
```

#### 示例 2: 使用专用 Cache

```c
// 定义结构
struct my_data {
    int id;
    char name[32];
    struct list_head list;
};

// 模块初始化
static struct kmem_cache *my_data_cache;

static int __init my_init(void)
{
    my_data_cache = kmem_cache_create("my_data_cache",
                                      sizeof(struct my_data),
                                      0,
                                      SLAB_HWCACHE_ALIGN,
                                      NULL);
    if (!my_data_cache)
        return -ENOMEM;
    return 0;
}

// 分配对象
struct my_data *data = kmem_cache_alloc(my_data_cache, GFP_KERNEL);
if (!data)
    return -ENOMEM;

data->id = 1;
strcpy(data->name, "test");
INIT_LIST_HEAD(&data->list);

// 释放对象
kmem_cache_free(my_data_cache, data);

// 模块退出
static void __exit my_exit(void)
{
    kmem_cache_destroy(my_data_cache);
}
```

---

## 三、启动 vs 运行时的关键区别

| 方面 | 启动时 | 运行时 |
|------|--------|--------|
| **分配方式** | 使用静态结构 `boot_kmem_cache` | 使用动态分配的 `kmem_cache` |
| **内存来源** | 从伙伴系统直接分配（通过 `new_slab()`） | 从 slab cache 分配（fast/slow path） |
| **初始化顺序** | 必须先初始化 `kmem_cache_node`，再初始化 `kmem_cache` | 可以随时创建新的 cache |
| **Bootstrap** | 需要 bootstrap 替换静态结构 | 不需要 |
| **状态** | `slab_state = DOWN → PARTIAL → UP` | `slab_state = UP`（完全可用） |
| **性能** | 初始化阶段，性能不是重点 | 优化 fast path，追求高性能 |

---

## 四、总结

### 启动过程做了什么：
1. **创建临时静态结构** (`boot_kmem_cache`, `boot_kmem_cache_node`)
2. **初始化核心 caches** (`kmem_cache_node`, `kmem_cache`)
3. **Bootstrap 自举**（用动态分配的结构替换静态结构）
4. **创建 kmalloc caches**（为 `kmalloc()` 提供各种大小的 cache）
5. **完成初始化**（`slab_state = UP`）

### 运行时如何使用：
1. **kmalloc()**: 根据大小自动选择对应的 kmalloc cache
2. **kmem_cache_alloc()**: 使用专用的 cache 分配对象
3. **Fast Path**: 大部分分配/释放通过无锁 fast path 完成
4. **Slow Path**: 当 fast path 失败时，从 partial 列表或新分配 slab

### 关键设计思想：
- **自举（Bootstrap）**: 用自己来分配自己，解决"先有鸡还是先有蛋"的问题
- **Fast Path 优先**: 大部分操作通过无锁 fast path 完成，追求极致性能
- **分层管理**: CPU → Node → Buddy System 的分层结构，平衡性能和内存利用率

