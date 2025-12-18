# Slab 如何被 `kmem_cache_cpu` 和 `kmem_cache_node` 记录和管理

## 一、核心问题

**问题**：分出来的 slab 是否都被 `kmem_cache` 中的 `kmem_cache_cpu` 和 `kmem_cache_node` 记录和管理？

**答案**：**不是所有的 slab 都被记录**。只有**正在使用或部分使用的 slab** 才会被记录，**完全空的 slab** 会被释放回伙伴系统。

---

## 二、Slab 的所有可能状态

### 2.1 Slab 状态分类

一个 slab 可能处于以下状态之一：

1. **在某个 CPU 的 `c->slab` 中**（frozen，正在使用）
2. **在某个 CPU 的 `c->partial` 中**（CPU partial 列表）
3. **在某个 node 的 `node->partial` 中**（node partial 列表）
4. **在某个 node 的 `node->full` 中**（如果启用了调试，full slab 列表）
5. **完全空了，被释放回伙伴系统**（不在任何列表中）

---

## 三、Slab 状态转换图

```
┌─────────────────────────────────────────────────────────────┐
│ 从伙伴系统分配新 Slab                                        │
│ allocate_slab() → alloc_slab_page() → alloc_pages()         │
└─────────────────────────────────────────────────────────────┘
                    │
                    ▼
┌─────────────────────────────────────────────────────────────┐
│ 状态 1: 在 CPU 的 c->slab 中（Frozen）                      │
│ - c->slab = slab                                            │
│ - slab->frozen = 1                                          │
│ - 只有该 CPU 可以从 slab->freelist 取对象                  │
└─────────────────────────────────────────────────────────────┘
                    │
                    │ CPU 不再使用（NUMA 不匹配、类型不匹配等）
                    ▼
┌─────────────────────────────────────────────────────────────┐
│ 状态 2: 在 CPU 的 c->partial 中（CPU Partial）              │
│ - c->partial = slab                                         │
│ - slab->frozen = 0                                          │
│ - 当 c->slab 用完后，可以从 c->partial 快速获取              │
└─────────────────────────────────────────────────────────────┘
                    │
                    │ CPU partial 满了，批量转移
                    ▼
┌─────────────────────────────────────────────────────────────┐
│ 状态 3: 在 Node 的 node->partial 中（Node Partial）         │
│ - node->partial 链表                                        │
│ - slab->frozen = 0                                          │
│ - 所有 CPU 都可以从 node->partial 获取                      │
└─────────────────────────────────────────────────────────────┘
                    │
                    │ 所有对象都被释放，且 nr_partial >= min_partial
                    ▼
┌─────────────────────────────────────────────────────────────┐
│ 状态 4: 完全空了，释放回伙伴系统                             │
│ - discard_slab() → free_slab() → __free_slab()             │
│ - __free_pages() → 归还给伙伴系统                           │
│ - 不再被任何结构记录                                         │
└─────────────────────────────────────────────────────────────┘
```

---

## 四、各状态详细说明

### 4.1 状态 1：在 CPU 的 `c->slab` 中（Frozen）

**位置**: `mm/slub.c:3903-3905`

```c
// 设置新的 slab 到 CPU
c->slab = slab;
c->freelist = get_freepointer(s, freelist);
```

**特点**:
- **`slab->frozen = 1`**：只有该 CPU 可以操作
- **不在任何列表中**：frozen slab 不在 node->partial 或 node->full 列表中
- **快速分配**：CPU 可以从 `c->freelist` 快速分配对象

**记录位置**:
- **`kmem_cache_cpu`**：`c->slab` 指向该 slab

---

### 4.2 状态 2：在 CPU 的 `c->partial` 中（CPU Partial）

**位置**: `mm/slub.c:3204-3242`

```c
static void put_cpu_partial(struct kmem_cache *s, struct slab *slab, int drain)
{
    oldslab = this_cpu_read(s->cpu_slab->partial);
    
    // 将新 slab 加入 CPU partial 列表头部
    slab->slabs = slabs;
    slab->next = oldslab;
    this_cpu_write(s->cpu_slab->partial, slab);
}
```

**特点**:
- **`slab->frozen = 0`**：未冻结
- **不在 node 列表中**：CPU partial 列表中的 slab 不在 node->partial 中
- **快速获取**：当 `c->slab` 用完后，可以从 `c->partial` 快速获取

**记录位置**:
- **`kmem_cache_cpu`**：`c->partial` 指向该 slab（链表头）

---

### 4.3 状态 3：在 Node 的 `node->partial` 中（Node Partial）

**位置**: `mm/slub.c:2721-2735`

```c
static inline void __add_partial(struct kmem_cache_node *n, struct slab *slab, int tail)
{
    n->nr_partial++;
    slab->inuse = slab->objects - slab->freelist->objects;
    if (tail)
        list_add_tail(&slab->slab_list, &n->partial);
    else
        list_add(&slab->slab_list, &n->partial);
}
```

**特点**:
- **`slab->frozen = 0`**：未冻结
- **在 node->partial 链表中**：所有 CPU 都可以从该列表获取
- **NUMA 本地性**：优先从本地 node 获取

**记录位置**:
- **`kmem_cache_node`**：`node->partial` 链表

---

### 4.4 状态 4：在 Node 的 `node->full` 中（Full，仅调试模式）

**位置**: `mm/slub.c:425-433`

```c
struct kmem_cache_node {
    spinlock_t list_lock;
    unsigned long nr_partial;
    struct list_head partial;
#ifdef CONFIG_SLUB_DEBUG
    atomic_long_t nr_slabs;
    atomic_long_t total_objects;
    struct list_head full;  // ← 仅在调试模式下使用
#endif
};
```

**特点**:
- **仅在调试模式下使用**：`CONFIG_SLUB_DEBUG` 启用时才有
- **完全满了**：`slab->inuse == slab->objects`
- **用于调试**：帮助检测内存泄漏等问题

**记录位置**:
- **`kmem_cache_node`**：`node->full` 链表（仅调试模式）

---

### 4.5 状态 5：完全空了，释放回伙伴系统

**位置**: `mm/slub.c:3113-3116` 和 `mm/slub.c:2692-2696`

```c
// 在 deactivate_slab() 中
if (!new.inuse && n->nr_partial >= s->min_partial) {
    stat(s, DEACTIVATE_EMPTY);
    discard_slab(s, slab);  // ← 释放 slab
    stat(s, FREE_SLAB);
}

static void discard_slab(struct kmem_cache *s, struct slab *slab)
{
    dec_slabs_node(s, slab_nid(slab), slab->objects);
    free_slab(s, slab);  // ← 释放到伙伴系统
}
```

**位置**: `mm/slub.c:2653-2667`

```c
static void __free_slab(struct kmem_cache *s, struct slab *slab)
{
    struct folio *folio = slab_folio(slab);
    int order = folio_order(folio);
    int pages = 1 << order;
    
    __slab_clear_pfmemalloc(slab);
    folio->mapping = NULL;
    __folio_clear_slab(folio);  // ← 清除 slab 标志
    mm_account_reclaimed_pages(pages);
    unaccount_slab(slab, order, s);
    __free_pages(&folio->page, order);  // ← 归还给伙伴系统
}
```

**特点**:
- **完全空了**：`slab->inuse == 0`
- **node->partial 已经足够多**：`nr_partial >= min_partial`
- **不再被任何结构记录**：释放后，slab 不再存在于任何列表中

**记录位置**:
- **无**：释放后，slab 不再被任何结构记录

---

## 五、Slab 状态转换的完整流程

### 5.1 从伙伴系统分配新 Slab

```
allocate_slab()
  └─ alloc_slab_page()
      └─ alloc_pages()  ← 从伙伴系统分配页面
          └─ 返回 struct slab *
```

**初始状态**:
- 不在任何列表中
- 需要设置到 CPU 或 node

### 5.2 设置到 CPU 的 `c->slab`

```
new_slab()
  └─ freeze_slab()  ← 冻结 slab
      └─ c->slab = slab  ← 设置到 CPU
```

**状态**:
- **状态 1**：在 CPU 的 `c->slab` 中（frozen）

### 5.3 CPU 不再使用，转移到 `c->partial`

```
deactivate_slab()
  └─ put_cpu_partial()  ← 加入 CPU partial
      └─ c->partial = slab
```

**状态**:
- **状态 2**：在 CPU 的 `c->partial` 中

### 5.4 CPU partial 满了，转移到 `node->partial`

```
put_cpu_partial()
  └─ __put_partials()  ← 批量转移
      └─ add_partial(n, slab)  ← 加入 node partial
```

**状态**:
- **状态 3**：在 Node 的 `node->partial` 中

### 5.5 所有对象都被释放，释放回伙伴系统

```
__slab_free()
  └─ 检查: !new.inuse && n->nr_partial >= s->min_partial
      └─ discard_slab()
          └─ free_slab()
              └─ __free_slab()
                  └─ __free_pages()  ← 归还给伙伴系统
```

**状态**:
- **状态 5**：完全空了，释放回伙伴系统

---

## 六、关键代码位置

| 操作 | 函数 | 文件 | 行号 |
|------|------|------|------|
| 分配新 slab | `allocate_slab()` | `mm/slub.c` | - |
| 设置到 CPU | `freeze_slab()` | `mm/slub.c` | 3624-3645 |
| 加入 CPU partial | `put_cpu_partial()` | `mm/slub.c` | 3204-3242 |
| 加入 node partial | `add_partial()` | `mm/slub.c` | 2721-2735 |
| 释放 slab | `discard_slab()` | `mm/slub.c` | 2692-2696 |
| 归还给伙伴系统 | `__free_slab()` | `mm/slub.c` | 2653-2667 |

---

## 七、总结

### 7.1 关键点

1. **不是所有的 slab 都被记录**
   - **正在使用或部分使用的 slab** 会被记录
   - **完全空的 slab** 会被释放回伙伴系统，不再被记录

2. **Slab 可能被记录在以下位置**：
   - **`kmem_cache_cpu->slab`**：当前 CPU 正在使用的 slab（frozen）
   - **`kmem_cache_cpu->partial`**：CPU partial 列表
   - **`kmem_cache_node->partial`**：Node partial 列表
   - **`kmem_cache_node->full`**：Full 列表（仅调试模式）

3. **Slab 状态转换**：
   - 从伙伴系统分配 → 设置到 CPU → CPU partial → Node partial → 释放回伙伴系统

### 7.2 设计优势

- **内存利用率**：完全空的 slab 会被释放，避免浪费
- **性能优化**：CPU partial 列表减少 node 访问
- **NUMA 本地性**：Node partial 列表支持 NUMA 本地性优化

---

## 八、完整示例

```
时间线：

T1: 从伙伴系统分配新 Slab A
    └─ allocate_slab() → alloc_pages()
    └─ 状态：不在任何列表中

T2: 设置到 CPU 0 的 c->slab
    └─ freeze_slab() → c->slab = Slab A
    └─ 状态：在 CPU 0 的 c->slab 中（frozen）

T3: CPU 0 不再使用，转移到 c->partial
    └─ deactivate_slab() → put_cpu_partial()
    └─ 状态：在 CPU 0 的 c->partial 中

T4: CPU partial 满了，转移到 node->partial
    └─ __put_partials() → add_partial()
    └─ 状态：在 Node 0 的 node->partial 中

T5: 所有对象都被释放，且 nr_partial >= min_partial
    └─ __slab_free() → discard_slab() → __free_pages()
    └─ 状态：完全空了，释放回伙伴系统（不再被记录）
```

---

**核心理解**：**不是所有的 slab 都被记录**。只有**正在使用或部分使用的 slab** 才会被 `kmem_cache_cpu` 或 `kmem_cache_node` 记录，**完全空的 slab** 会被释放回伙伴系统，不再被任何结构记录。

