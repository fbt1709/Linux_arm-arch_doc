# Slab 如何被加入到 node->partial 列表？

## 一、核心问题

**问题**：`slab->partial` 是什么时候被赋值的？不是直接放到 node 吗？

**答案**：**`struct slab` 中没有 `partial` 字段**。Slab 是通过 **`slab->slab_list`** 字段加入到 `node->partial` 链表中的，并使用 **`PG_workingset` 位**来标记 slab 是否在 node->partial 列表中。

---

## 二、关键理解

### 2.1 `struct slab` 中没有 `partial` 字段

**位置**: `mm/slab.h:52-98`

```c
struct slab {
    unsigned long __page_flags;
    
    struct kmem_cache *slab_cache;
    union {
        struct {
            union {
                struct list_head slab_list;  // ← 用于链表操作
#ifdef CONFIG_SLUB_CPU_PARTIAL
                struct {
                    struct slab *next;       // ← 用于 CPU partial 链表
                    int slabs;
                };
#endif
            };
            // ...
        };
    };
    // ...
};
```

**关键理解**：
- **`slab->slab_list`**：用于将 slab 加入到 `node->partial` 或 `node->full` 链表
- **`slab->next`**：用于 CPU partial 链表（仅在 `CONFIG_SLUB_CPU_PARTIAL` 启用时）
- **没有 `slab->partial` 字段**

### 2.2 如何标记 slab 在 node->partial 列表中？

**位置**: `mm/slub.c:2698-2715`

```c
/*
 * SLUB reuses PG_workingset bit to keep track of whether it's on
 * the per-node partial list.
 */
static inline bool slab_test_node_partial(const struct slab *slab)
{
    return folio_test_workingset(slab_folio(slab));
}

static inline void slab_set_node_partial(struct slab *slab)
{
    set_bit(PG_workingset, folio_flags(slab_folio(slab), 0));
}

static inline void slab_clear_node_partial(struct slab *slab)
{
    clear_bit(PG_workingset, folio_flags(slab_folio(slab), 0));
}
```

**关键理解**：
- **`PG_workingset` 位**：用于标记 slab 是否在 node->partial 列表中
- **`slab_set_node_partial()`**：设置 `PG_workingset` 位，标记 slab 在 node->partial 列表中
- **`slab_clear_node_partial()`**：清除 `PG_workingset` 位，标记 slab 不在 node->partial 列表中

---

## 三、Slab 被加入到 node->partial 的完整流程

### 3.1 `add_partial()` 函数

**位置**: `mm/slub.c:2721-2736`

```c
__add_partial(struct kmem_cache_node *n, struct slab *slab, int tail)
{
    n->nr_partial++;  // ← 增加 node->partial 列表中的 slab 数量
    
    // ✅ 将 slab 加入到 node->partial 链表
    if (tail == DEACTIVATE_TO_TAIL)
        list_add_tail(&slab->slab_list, &n->partial);  // ← 添加到尾部
    else
        list_add(&slab->slab_list, &n->partial);      // ← 添加到头部
    
    // ✅ 标记 slab 在 node->partial 列表中
    slab_set_node_partial(slab);  // ← 设置 PG_workingset 位
}

static inline void add_partial(struct kmem_cache_node *n,
                struct slab *slab, int tail)
{
    lockdep_assert_held(&n->list_lock);  // ← 必须持有锁
    __add_partial(n, slab, tail);
}
```

**关键理解**：
1. **`list_add()` 或 `list_add_tail()`**：将 `slab->slab_list` 加入到 `node->partial` 链表
2. **`slab_set_node_partial()`**：设置 `PG_workingset` 位，标记 slab 在 node->partial 列表中
3. **`n->nr_partial++`**：增加 node->partial 列表中的 slab 数量

### 3.2 从 node->partial 移除 slab

**位置**: `mm/slub.c:2738-2745`

```c
static inline void remove_partial(struct kmem_cache_node *n,
                    struct slab *slab)
{
    lockdep_assert_held(&n->list_lock);
    
    // ✅ 从 node->partial 链表移除
    list_del(&slab->slab_list);  // ← 从链表中删除
    
    // ✅ 清除标记
    slab_clear_node_partial(slab);  // ← 清除 PG_workingset 位
    
    n->nr_partial--;  // ← 减少 node->partial 列表中的 slab 数量
}
```

**关键理解**：
1. **`list_del()`**：从 `node->partial` 链表中移除 `slab->slab_list`
2. **`slab_clear_node_partial()`**：清除 `PG_workingset` 位，标记 slab 不在 node->partial 列表中
3. **`n->nr_partial--`**：减少 node->partial 列表中的 slab 数量

---

## 四、何时调用 `add_partial()`？

### 4.1 场景 1：`deactivate_slab()` 时

**位置**: `mm/slub.c:3117-3121`

```c
static void deactivate_slab(struct kmem_cache *s, struct slab *slab,
            void *freelist)
{
    // ... 解冻 slab，合并 freelist ...
    
    // ✅ 如果 slab 还有空闲对象，加入 node->partial
    if (!new.inuse && n->nr_partial >= s->min_partial) {
        // Slab 完全空了，且 node->partial 已经足够多
        discard_slab(s, slab);  // ← 释放回伙伴系统
    } else if (new.freelist) {
        // Slab 还有空闲对象
        spin_lock_irqsave(&n->list_lock, flags);
        add_partial(n, slab, tail);  // ← ✅ 加入 node->partial
        spin_unlock_irqrestore(&n->list_lock, flags);
        stat(s, tail);
    } else {
        // Slab 完全满了
        stat(s, DEACTIVATE_FULL);
    }
}
```

**调用时机**：
- CPU 不再使用 slab 时（NUMA 不匹配、类型不匹配等）
- Slab 还有空闲对象（`new.freelist != NULL`）
- Slab 不是完全空的（`new.inuse > 0`）

### 4.2 场景 2：`__slab_free()` 时

**位置**: `mm/slub.c:4491-4494`

```c
static void __slab_free(struct kmem_cache *s, struct slab *slab,
            void *head, void *tail, int cnt,
            unsigned long addr)
{
    // ... 释放对象到 slab->freelist ...
    
    // ✅ 如果 slab 从 full 变为 partial，加入 node->partial
    if (!kmem_cache_has_cpu_partial(s) && unlikely(!prior)) {
        // prior == NULL 表示之前是 full
        // 现在释放了一个对象，变成 partial
        add_partial(n, slab, DEACTIVATE_TO_TAIL);  // ← ✅ 加入 node->partial
        stat(s, FREE_ADD_PARTIAL);
    }
}
```

**调用时机**：
- 释放对象时，slab 从 full 变为 partial
- 系统没有启用 CPU partial 功能（`!kmem_cache_has_cpu_partial(s)`）

---

## 五、完整的数据结构关系

### 5.1 Node Partial 列表结构

```
struct kmem_cache_node {
    spinlock_t list_lock;
    unsigned long nr_partial;
    struct list_head partial;  // ← 链表头
};

struct slab {
    union {
        struct list_head slab_list;  // ← 链表节点
        // ...
    };
    // ...
};

node->partial
  └─ slab1->slab_list  ← 通过 slab_list 链接
      └─ slab2->slab_list
          └─ slab3->slab_list
              └─ NULL
```

### 5.2 链表操作

**加入链表**：
```c
list_add(&slab->slab_list, &n->partial);
// 或
list_add_tail(&slab->slab_list, &n->partial);
```

**从链表移除**：
```c
list_del(&slab->slab_list);
```

**遍历链表**：
```c
list_for_each_entry(slab, &n->partial, slab_list) {
    // 处理每个 slab
}
```

---

## 六、关键代码位置总结

| 操作 | 函数 | 文件 | 行号 |
|------|------|------|------|
| 加入 node->partial | `add_partial()` | `mm/slub.c` | 2721-2736 |
| 从 node->partial 移除 | `remove_partial()` | `mm/slub.c` | 2738-2745 |
| 标记在 node->partial | `slab_set_node_partial()` | `mm/slub.c` | 2707-2710 |
| 清除标记 | `slab_clear_node_partial()` | `mm/slub.c` | 2712-2715 |
| 检查是否在 node->partial | `slab_test_node_partial()` | `mm/slub.c` | 2702-2705 |
| 调用 add_partial | `deactivate_slab()` | `mm/slub.c` | 3119 |
| 调用 add_partial | `__slab_free()` | `mm/slub.c` | 4492 |

---

## 七、总结

### 7.1 关键点

1. **`struct slab` 中没有 `partial` 字段**
   - Slab 通过 **`slab->slab_list`** 字段加入到 `node->partial` 链表
   - 使用 **`PG_workingset` 位**来标记 slab 是否在 node->partial 列表中

2. **加入 node->partial 的流程**：
   - **`list_add()` 或 `list_add_tail()`**：将 `slab->slab_list` 加入到 `node->partial` 链表
   - **`slab_set_node_partial()`**：设置 `PG_workingset` 位
   - **`n->nr_partial++`**：增加计数

3. **调用时机**：
   - **`deactivate_slab()`**：CPU 不再使用 slab 时
   - **`__slab_free()`**：Slab 从 full 变为 partial 时

### 7.2 设计优势

- **复用 `struct page` 的字段**：`slab->slab_list` 复用 `struct page` 的 `lru` 字段
- **标记位复用**：`PG_workingset` 位用于标记 slab 是否在 node->partial 列表中
- **标准链表操作**：使用 Linux 内核标准的 `list_head` 链表操作

---

**核心理解**：**`struct slab` 中没有 `partial` 字段**。Slab 通过 **`slab->slab_list`** 字段加入到 `node->partial` 链表中，并使用 **`PG_workingset` 位**来标记 slab 是否在 node->partial 列表中。这个过程是通过 `add_partial()` 函数完成的，通常在 `deactivate_slab()` 或 `__slab_free()` 时调用。

