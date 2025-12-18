# Page 如何知道它被使用或被 Slab 使用

## 一、核心机制：`page_type` 字段

在 Linux 内核中，`struct page` 通过 `page_type` 字段来标记页面的用途。这个字段是一个 `unsigned int`，其**高 8 位**（bit 24-31）存储页面类型。

### 1.1 `page_type` 字段定义

**位置**: `include/linux/mm_types.h:166`

```c
union {
    /*
     * For head pages of typed folios, the value stored here
     * allows for determining what this page is used for.
     */
    unsigned int page_type;  // 高 8 位存储页面类型
    
    /*
     * For pages that are part of non-typed folios,
     * encodes the number of times this page is directly
     * referenced by a page table.
     */
    atomic_t _mapcount;
};
```

### 1.2 页面类型定义

**位置**: `include/linux/page-flags.h:942-947`

```c
enum page_type {
    PGTY_buddy  = 0xf0,  // 伙伴系统管理的页面
    PGTY_slab   = 0xf5,  // Slab 分配器管理的页面
    // ... 其他类型
};
```

---

## 二、Slab 页面的标记机制

### 2.1 设置 Slab 标志

当从伙伴系统分配页面用于 slab 时，会设置 `page_type` 字段：

**位置**: `mm/slub.c:2417-2439`

```c
static inline struct slab *alloc_slab_page(gfp_t flags, int node,
        struct kmem_cache_order_objects oo)
{
    struct folio *folio;
    struct slab *slab;
    unsigned int order = oo_order(oo);
    
    // 1. 从伙伴系统分配页面
    if (node == NUMA_NO_NODE)
        folio = (struct folio *)alloc_pages(flags, order);
    else
        folio = (struct folio *)__alloc_pages_node(node, flags, order);
    
    if (!folio)
        return NULL;
    
    // 2. 将 page 转换为 slab
    slab = folio_slab(folio);
    
    // 3. 设置 slab 标志（关键步骤！）
    __folio_set_slab(folio);
    
    // 4. 内存屏障，确保标志可见性
    smp_wmb();
    
    if (folio_is_pfmemalloc(folio))
        slab_set_pfmemalloc(slab);
    
    return slab;
}
```

**`__folio_set_slab()` 的实现**:

**位置**: `include/linux/page-flags.h:975-980`

```c
#define FOLIO_TYPE_OPS(lname, fname)                    \
static __always_inline void __folio_set_##fname(struct folio *folio) \
{                                                       \
    if (folio_test_##fname(folio))                    \
        return;                                        \
    VM_BUG_ON_FOLIO(data_race(folio->page.page_type) != UINT_MAX, \
            folio);                                    \
    folio->page.page_type = (unsigned int)PGTY_##lname << 24; \
}
```

**展开后**:
```c
static __always_inline void __folio_set_slab(struct folio *folio)
{
    if (folio_test_slab(folio))  // 如果已经是 slab，直接返回
        return;
    
    // 确保 page_type 之前是 UINT_MAX（未初始化状态）
    VM_BUG_ON_FOLIO(folio->page.page_type != UINT_MAX, folio);
    
    // 设置高 8 位为 PGTY_slab (0xf5)
    folio->page.page_type = (unsigned int)PGTY_slab << 24;
    // 即: page_type = 0xf5000000
}
```

---

### 2.2 检查 Slab 标志

**位置**: `include/linux/page-flags.h:970-974`

```c
#define FOLIO_TYPE_OPS(lname, fname)                    \
static __always_inline bool folio_test_##fname(const struct folio *folio) \
{                                                       \
    return data_race(folio->page.page_type >> 24) == PGTY_##lname; \
}
```

**展开后**:
```c
static __always_inline bool folio_test_slab(const struct folio *folio)
{
    // 读取高 8 位，检查是否等于 PGTY_slab (0xf5)
    return (folio->page.page_type >> 24) == PGTY_slab;
}
```

**`PageSlab()` 宏**:

**位置**: `include/linux/page-flags.h:1073-1076`

```c
static inline bool PageSlab(const struct page *page)
{
    return folio_test_slab(page_folio(page));
}
```

---

### 2.3 清除 Slab 标志

当 slab 被释放回伙伴系统时，需要清除 slab 标志：

**位置**: `mm/slub.c:2653-2667`

```c
static void __free_slab(struct kmem_cache *s, struct slab *slab)
{
    struct folio *folio = slab_folio(slab);
    int order = folio_order(folio);
    int pages = 1 << order;
    
    __slab_clear_pfmemalloc(slab);
    folio->mapping = NULL;
    
    // 内存屏障，确保 mapping 重置可见
    smp_wmb();
    
    // 清除 slab 标志（关键步骤！）
    __folio_clear_slab(folio);
    
    mm_account_reclaimed_pages(pages);
    unaccount_slab(slab, order, s);
    
    // 释放回伙伴系统
    __free_pages(&folio->page, order);
}
```

**`__folio_clear_slab()` 的实现**:

**位置**: `include/linux/page-flags.h:981-987`

```c
#define FOLIO_TYPE_OPS(lname, fname)                    \
static __always_inline void __folio_clear_##fname(struct folio *folio) \
{                                                       \
    VM_BUG_ON_FOLIO(!folio_test_##fname(folio), folio); \
    folio->page.page_type = UINT_MAX;                   \
}
```

**展开后**:
```c
static __always_inline void __folio_clear_slab(struct folio *folio)
{
    // 确保确实是 slab 页面
    VM_BUG_ON_FOLIO(!folio_test_slab(folio), folio);
    
    // 清除标志，恢复为未初始化状态
    folio->page.page_type = UINT_MAX;
}
```

---

## 三、Buddy 页面的标记机制

### 3.1 Buddy 页面的标记

当页面在伙伴系统中时，会设置 `PGTY_buddy` 标志：

**位置**: `mm/page_alloc.c` (简化示例)

```c
// 当页面加入伙伴系统时
__SetPageBuddy(page);
// 设置 page->page_type = PGTY_buddy << 24 (即 0xf0000000)

// 当页面从伙伴系统分配出去时
__ClearPageBuddy(page);
// 清除标志: page->page_type = UINT_MAX
```

---

## 四、页面状态转换流程

### 4.1 从伙伴系统到 Slab

```
1. 伙伴系统管理的页面
   page->page_type = PGTY_buddy << 24 (或 UINT_MAX)

2. alloc_slab_page() 从伙伴系统分配
   alloc_pages() → 返回 struct page *

3. __folio_set_slab(folio)
   page->page_type = PGTY_slab << 24 (0xf5000000)

4. 页面现在属于 Slab
   PageSlab(page) == true
```

### 4.2 从 Slab 回到伙伴系统

```
1. Slab 管理的页面
   page->page_type = PGTY_slab << 24 (0xf5000000)
   PageSlab(page) == true

2. __free_slab() 释放 slab
   __folio_clear_slab(folio)
   page->page_type = UINT_MAX

3. __free_pages() 释放回伙伴系统
   页面重新加入伙伴系统的 free list

4. 页面现在属于伙伴系统
   PageSlab(page) == false
```

---

## 五、如何区分页面状态

### 5.1 检查函数

| 函数 | 检查内容 | 返回值 |
|------|---------|--------|
| `PageSlab(page)` | 页面是否被 slab 使用 | `true` = slab 页面 |
| `PageBuddy(page)` | 页面是否在伙伴系统中 | `true` = buddy 页面 |
| `page->page_type >> 24` | 直接读取页面类型 | `0xf5` = slab, `0xf0` = buddy |

### 5.2 使用示例

```c
// 检查页面是否被 slab 使用
if (PageSlab(page)) {
    struct slab *slab = folio_slab(page_folio(page));
    struct kmem_cache *s = slab->slab_cache;
    // 处理 slab 页面
}

// 检查页面是否在伙伴系统中
if (PageBuddy(page)) {
    unsigned int order = page_private(page);
    // 处理 buddy 页面
}

// 直接检查页面类型
unsigned int type = page->page_type >> 24;
if (type == PGTY_slab) {
    // Slab 页面
} else if (type == PGTY_buddy) {
    // Buddy 页面
}
```

---

## 六、关键数据结构关系

### 6.1 `struct page` 与 `struct slab` 的关系

**重要**: `struct slab` **复用** `struct page` 的内存空间！

**位置**: `mm/slab.h:52-98`

```c
/* Reuses the bits in struct page */
struct slab {
    unsigned long __page_flags;      // 复用 page->flags
    struct kmem_cache *slab_cache;   // 复用 page->compound_head 的位置
    // ... 其他字段复用 page 的 union
    unsigned int __page_type;        // 复用 page->page_type
    atomic_t __page_refcount;        // 复用 page->_refcount
};
```

**验证**:
```c
SLAB_MATCH(flags, __page_flags);
SLAB_MATCH(compound_head, slab_cache);
SLAB_MATCH(_refcount, __page_refcount);
static_assert(sizeof(struct slab) <= sizeof(struct page));
```

这意味着：
- **同一个物理页面**，既可以作为 `struct page` 使用，也可以作为 `struct slab` 使用
- 通过 `page_type` 字段区分用途
- `folio_slab(folio)` 只是简单的类型转换：`(struct slab *)folio`

---

## 七、完整生命周期示例

### 7.1 页面从伙伴系统到 Slab 再到伙伴系统

```
阶段 1: 伙伴系统管理
┌─────────────────────┐
│ page->page_type     │ = PGTY_buddy << 24 (或 UINT_MAX)
│ page 在 zone->free_area[order] 链表中 │
└─────────────────────┘
         │
         │ alloc_slab_page()
         │ __folio_set_slab()
         ▼
阶段 2: Slab 管理
┌─────────────────────┐
│ page->page_type     │ = PGTY_slab << 24 (0xf5000000)
│ slab->slab_cache    │ = kmem_cache 指针
│ slab->freelist      │ = 空闲对象链表
│ slab->inuse         │ = 已分配对象数
│ PageSlab(page)      │ = true
└─────────────────────┘
         │
         │ __free_slab()
         │ __folio_clear_slab()
         │ __free_pages()
         ▼
阶段 3: 回到伙伴系统
┌─────────────────────┐
│ page->page_type     │ = UINT_MAX
│ page 重新加入 zone->free_area[order] │
│ PageSlab(page)      │ = false
└─────────────────────┘
```

---

## 八、总结

### 8.1 核心要点

1. **`page_type` 字段的高 8 位存储页面类型**
   - `PGTY_slab = 0xf5`: Slab 页面
   - `PGTY_buddy = 0xf0`: Buddy 页面

2. **设置标志**
   - `__folio_set_slab()`: 设置 `page_type = PGTY_slab << 24`
   - 在 `alloc_slab_page()` 中调用

3. **检查标志**
   - `PageSlab(page)`: 检查 `(page_type >> 24) == PGTY_slab`
   - `PageBuddy(page)`: 检查 `(page_type >> 24) == PGTY_buddy`

4. **清除标志**
   - `__folio_clear_slab()`: 设置 `page_type = UINT_MAX`
   - 在 `__free_slab()` 中调用

5. **`struct slab` 复用 `struct page` 的内存空间**
   - 通过 `page_type` 区分用途
   - `folio_slab()` 只是类型转换

### 8.2 关键代码位置

| 功能 | 文件 | 函数 |
|------|------|------|
| 设置 slab 标志 | `mm/slub.c:2433` | `__folio_set_slab()` |
| 清除 slab 标志 | `mm/slub.c:2663` | `__folio_clear_slab()` |
| 检查 slab 标志 | `include/linux/page-flags.h:1073` | `PageSlab()` |
| 页面类型定义 | `include/linux/page-flags.h:942-947` | `enum page_type` |

通过 `page_type` 字段，内核可以清楚地知道每个页面的用途，从而正确地管理内存。

