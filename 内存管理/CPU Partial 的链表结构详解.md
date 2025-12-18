# CPU Partial 的链表结构详解

## 一、核心问题

**问题**：从 `node->partial` 获取 slab 时，如果 CPU partial 还没满，会加入 CPU partial。但是 CPU partial 不就是一个 slab 吗？他怎么增加呢？

**答案**：**`c->partial` 是一个指向链表头部的指针**。CPU partial 是一个**链表**，通过 `slab->next` 链接多个 slab。每个 slab 的 `slab->slabs` 字段记录从该 slab 开始，后续还有多少个 slab。

---

## 二、CPU Partial 的链表结构

### 2.1 数据结构

**位置**: `mm/slub.c:384-400`

```c
struct kmem_cache_cpu {
    // ...
    struct slab *slab;  /* The slab from which we are allocating */
#ifdef CONFIG_SLUB_CPU_PARTIAL
    struct slab *partial;  /* Partially allocated slabs */  ← 指向链表头部
#endif
    // ...
};
```

**位置**: `mm/slab.h:60-64`

```c
#ifdef CONFIG_SLUB_CPU_PARTIAL
struct {
    struct slab *next;  /* 指向下一个 slab */
    int slabs;          /* Nr of slabs left */  ← 记录后续还有多少个 slab
};
#endif
```

**关键理解**：
- **`c->partial`**：指向 CPU partial 链表的头部（`struct slab *`）
- **`slab->next`**：指向链表中的下一个 slab
- **`slab->slabs`**：记录从该 slab 开始，后续还有多少个 slab

### 2.2 链表结构示例

```
c->partial
  └─ Slab 1 (slabs=3)  ← 链表头部
      └─ next → Slab 2 (slabs=2)
          └─ next → Slab 3 (slabs=1)
              └─ next → Slab 4 (slabs=0)
                  └─ next → NULL
```

**关键理解**：
- **`c->partial` 指向 Slab 1**（链表头部）
- **`Slab 1->slabs = 3`**：表示从 Slab 1 开始，后续还有 3 个 slab（Slab 2, 3, 4）
- **`Slab 1->next` 指向 Slab 2**
- **`Slab 2->slabs = 2`**：表示从 Slab 2 开始，后续还有 2 个 slab（Slab 3, 4）
- **`Slab 2->next` 指向 Slab 3**
- **以此类推...**

---

## 三、如何增加 slab 到 CPU partial？

### 3.1 `put_cpu_partial()` 函数详解

**位置**: `mm/slub.c:3204-3242`

```c
static void put_cpu_partial(struct kmem_cache *s, struct slab *slab, int drain)
{
    struct slab *oldslab;
    struct slab *slab_to_put = NULL;
    unsigned long flags;
    int slabs = 0;
    
    local_lock_irqsave(&s->cpu_slab->lock, flags);
    
    // 1. 读取当前 CPU 的 partial 列表头部
    oldslab = this_cpu_read(s->cpu_slab->partial);
    
    if (oldslab) {
        if (drain && oldslab->slabs >= s->cpu_partial_slabs) {
            // CPU partial 列表满了，需要批量转移到 node->partial
            slab_to_put = oldslab;
            oldslab = NULL;
        } else {
            // ✅ 读取当前链表中的 slab 数量
            slabs = oldslab->slabs;  // ← 例如：oldslab->slabs = 3
        }
    }
    
    // 2. 计算新的 slab 数量
    slabs++;  // ← 例如：slabs = 3 + 1 = 4
    
    // 3. 将新 slab 插入到链表头部
    slab->slabs = slabs;      // ← 新 slab 的 slabs = 4（后续还有 4 个 slab）
    slab->next = oldslab;     // ← 新 slab 的 next 指向旧的链表头部
    
    // ✅ 更新 c->partial 指向新的链表头部
    this_cpu_write(s->cpu_slab->partial, slab);
    
    local_unlock_irqrestore(&s->cpu_slab->lock, flags);
    
    // 4. 如果 CPU partial 满了，批量转移到 node->partial
    if (slab_to_put) {
        __put_partials(s, slab_to_put);
        stat(s, CPU_PARTIAL_DRAIN);
    }
}
```

### 3.2 完整示例

#### 初始状态

```
c->partial = NULL  ← 链表为空
```

#### 第一次加入 Slab 1

```
put_cpu_partial(s, Slab 1, 0)

oldslab = NULL
slabs = 0 + 1 = 1

Slab 1->slabs = 1
Slab 1->next = NULL
c->partial = Slab 1
```

**结果**：
```
c->partial
  └─ Slab 1 (slabs=1)
      └─ next → NULL
```

#### 第二次加入 Slab 2

```
put_cpu_partial(s, Slab 2, 0)

oldslab = Slab 1 (slabs=1)
slabs = 1 + 1 = 2

Slab 2->slabs = 2
Slab 2->next = Slab 1
c->partial = Slab 2
```

**结果**：
```
c->partial
  └─ Slab 2 (slabs=2)  ← 新的头部
      └─ next → Slab 1 (slabs=1)
          └─ next → NULL
```

#### 第三次加入 Slab 3

```
put_cpu_partial(s, Slab 3, 0)

oldslab = Slab 2 (slabs=2)
slabs = 2 + 1 = 3

Slab 3->slabs = 3
Slab 3->next = Slab 2
c->partial = Slab 3
```

**结果**：
```
c->partial
  └─ Slab 3 (slabs=3)  ← 新的头部
      └─ next → Slab 2 (slabs=2)
          └─ next → Slab 1 (slabs=1)
              └─ next → NULL
```

---

## 四、如何从 CPU partial 获取 slab？

### 4.1 `slub_percpu_partial()` 和 `slub_set_percpu_partial()`

**位置**: `mm/slab.h:230-235`

```c
#define slub_percpu_partial(c)         ((c)->partial)  ← 返回链表头部

#define slub_set_percpu_partial(c, p)  \
({                                      \
    slub_percpu_partial(c) = (p)->next;  ← 更新链表头部为下一个 slab
})
```

**关键理解**：
- **`slub_percpu_partial(c)`**：返回 `c->partial`（链表头部）
- **`slub_set_percpu_partial(c, slab)`**：将 `c->partial` 更新为 `slab->next`（从链表中移除 slab）

### 4.2 从 CPU partial 获取 slab 的流程

**位置**: `mm/slub.c:3779-3780`

```c
// 1. 获取链表头部
slab = slub_percpu_partial(c);  // ← slab = c->partial

// 2. 从链表中移除（更新链表头部）
slub_set_percpu_partial(c, slab);  // ← c->partial = slab->next
```

### 4.3 完整示例

#### 初始状态

```
c->partial
  └─ Slab 3 (slabs=3)
      └─ next → Slab 2 (slabs=2)
          └─ next → Slab 1 (slabs=1)
              └─ next → NULL
```

#### 获取 Slab 3

```
slab = slub_percpu_partial(c);  // ← slab = Slab 3
slub_set_percpu_partial(c, slab);  // ← c->partial = Slab 3->next = Slab 2
```

**结果**：
```
c->partial
  └─ Slab 2 (slabs=2)  ← 新的头部
      └─ next → Slab 1 (slabs=1)
          └─ next → NULL

slab = Slab 3  ← 已从链表中移除
```

---

## 五、`slab->slabs` 字段的作用

### 5.1 记录后续 slab 数量

**关键理解**：
- **`slab->slabs`**：记录从该 slab 开始，**后续还有多少个 slab**
- **用于限制 CPU partial 列表的长度**：`cpu_partial_slabs`

### 5.2 检查是否满了

**位置**: `mm/slub.c:3216`

```c
if (drain && oldslab->slabs >= s->cpu_partial_slabs) {
    // CPU partial 列表满了，需要批量转移到 node->partial
    slab_to_put = oldslab;
    oldslab = NULL;
}
```

**关键理解**：
- **`oldslab->slabs`**：当前链表中的 slab 数量
- **`s->cpu_partial_slabs`**：CPU partial 列表的最大长度
- **如果 `oldslab->slabs >= s->cpu_partial_slabs`**：链表满了，需要批量转移

### 5.3 示例

```
假设 cpu_partial_slabs = 3

c->partial
  └─ Slab 3 (slabs=3)  ← oldslab->slabs = 3
      └─ next → Slab 2 (slabs=2)
          └─ next → Slab 1 (slabs=1)
              └─ next → NULL

检查: oldslab->slabs (3) >= cpu_partial_slabs (3)  ✓
→ 链表满了，需要批量转移到 node->partial
```

---

## 六、完整的数据流

### 6.1 从 node->partial 获取时加入 CPU partial

```
初始状态：
c->partial = NULL

从 node->partial 获取 Slab 1：
  └─ get_partial_node() → 获取 Slab 1
  └─ put_cpu_partial(s, Slab 1, 0)
      └─ c->partial = Slab 1
      └─ Slab 1->slabs = 1
      └─ Slab 1->next = NULL

结果：
c->partial
  └─ Slab 1 (slabs=1)
      └─ next → NULL

从 node->partial 获取 Slab 2：
  └─ get_partial_node() → 获取 Slab 2
  └─ put_cpu_partial(s, Slab 2, 0)
      └─ oldslab = Slab 1 (slabs=1)
      └─ slabs = 1 + 1 = 2
      └─ Slab 2->slabs = 2
      └─ Slab 2->next = Slab 1
      └─ c->partial = Slab 2

结果：
c->partial
  └─ Slab 2 (slabs=2)
      └─ next → Slab 1 (slabs=1)
          └─ next → NULL
```

### 6.2 分配时从 CPU partial 获取

```
初始状态：
c->partial
  └─ Slab 2 (slabs=2)
      └─ next → Slab 1 (slabs=1)
          └─ next → NULL

分配时（c->slab 用完后）：
  └─ ___slab_alloc()
      └─ slab = slub_percpu_partial(c)  // ← slab = Slab 2
      └─ slub_set_percpu_partial(c, slab)  // ← c->partial = Slab 2->next = Slab 1
      └─ c->slab = Slab 2

结果：
c->partial
  └─ Slab 1 (slabs=1)
      └─ next → NULL

c->slab = Slab 2  ← 已从链表中移除
```

---

## 七、关键代码位置总结

| 操作 | 函数/宏 | 文件 | 行号 |
|------|---------|------|------|
| 加入 CPU partial | `put_cpu_partial()` | `mm/slub.c` | 3204-3242 |
| 获取链表头部 | `slub_percpu_partial()` | `mm/slab.h` | 230 |
| 从链表移除 | `slub_set_percpu_partial()` | `mm/slab.h` | 232-235 |
| 从 CPU partial 获取 | `___slab_alloc()` | `mm/slub.c` | 3779-3780 |
| 批量转移到 node | `__put_partials()` | `mm/slub.c` | 3128-3167 |

---

## 八、总结

### 8.1 关键点

1. **`c->partial` 是一个指向链表头部的指针**
   - **不是单个 slab**，而是一个链表的头部
   - **通过 `slab->next` 链接多个 slab**

2. **如何增加 slab？**
   - **`put_cpu_partial()`**：将新 slab 插入到链表头部
   - **`slab->next = oldslab`**：链到旧的链表头部
   - **`slab->slabs = slabs`**：记录后续还有多少个 slab
   - **`c->partial = slab`**：更新链表头部

3. **如何获取 slab？**
   - **`slub_percpu_partial(c)`**：获取链表头部
   - **`slub_set_percpu_partial(c, slab)`**：从链表中移除（更新链表头部）

4. **`slab->slabs` 字段的作用**
   - **记录从该 slab 开始，后续还有多少个 slab**
   - **用于限制 CPU partial 列表的长度**

### 8.2 设计优势

- **链表结构**：可以存储多个 slab，而不是单个 slab
- **头部插入**：新 slab 插入到头部，O(1) 时间复杂度
- **头部移除**：从头部移除，O(1) 时间复杂度
- **计数优化**：通过 `slab->slabs` 快速检查链表长度

---

**核心理解**：**`c->partial` 是一个指向链表头部的指针**。CPU partial 是一个**链表**，通过 `slab->next` 链接多个 slab。当加入新的 slab 时，会将它插入到链表头部，并更新 `slab->slabs` 计数。当从链表获取 slab 时，会从头部取出，并更新 `c->partial` 指向下一个 slab。

