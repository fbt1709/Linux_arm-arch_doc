# Slab 的 Freelist 与 CPU 的关系详解

## 一、核心问题

**问题**：为什么当前 CPU 的 `c->freelist` 用完了，但 `slab->freelist` 也为空？难道一个 slab 还分不同的 CPU 使用吗？

**答案**：**一个 frozen slab 只能被一个 CPU 使用**，但其他 CPU 可以释放对象到这个 slab。

---

## 二、关键概念

### 2.1 Frozen Slab 的特性

**位置**: `mm/slub.c:78-86`

```c
/*
 * Frozen slabs
 *
 * If a slab is frozen then it is exempt from list management. It is
 * the cpu slab which is actively allocated from by the processor that
 * froze it and it is not on any list. The processor that froze the
 * slab is the one who can perform list operations on the slab. Other
 * processors may put objects onto the freelist but the processor that
 * froze the slab is the only one that can retrieve the objects from the
 * slab's freelist.
 */
```

**关键理解**:
1. **一个 frozen slab 只能被一个 CPU 使用**（冻结它的 CPU）
2. **只有冻结它的 CPU 可以从 `slab->freelist` 中取对象**
3. **其他 CPU 可以释放对象到 `slab->freelist`**（但不能取对象）

---

## 三、Freelist 的层次结构

### 3.1 两个层次的 Freelist

```
┌─────────────────────────────────────────┐
│  struct kmem_cache_cpu (Per-CPU)       │
│  ┌───────────────────────────────────┐  │
│  │ c->freelist                       │  │ ← CPU 本地缓存
│  │ c->slab → Slab A (frozen)         │  │
│  └───────────────────────────────────┘  │
└─────────────────────────────────────────┘
                    │
                    │ 指向
                    ▼
┌─────────────────────────────────────────┐
│  struct slab                             │
│  ┌───────────────────────────────────┐  │
│  │ slab->freelist                   │  │ ← Slab 本身的 freelist
│  │ slab->frozen = 1                 │  │
│  │ slab->inuse = 5                  │  │
│  │ slab->objects = 10               │  │
│  └───────────────────────────────────┘  │
└─────────────────────────────────────────┘
```

### 3.2 对象在两个 Freelist 之间的流动

#### 阶段 1：Slab 被冻结时

**位置**: `mm/slub.c:3624-3645`

```c
static inline void *freeze_slab(struct kmem_cache *s, struct slab *slab)
{
    // 从 slab->freelist 中取走所有对象
    freelist = slab->freelist;
    
    // 设置 slab->freelist = NULL
    new.frozen = 1;
    // slab->freelist 被清空
    
    return freelist;  // ← 返回所有对象，放到 c->freelist
}
```

**流程**:
```
冻结前：
slab->freelist → Object 0 → Object 1 → ... → Object 9 → NULL
c->freelist = NULL

冻结后：
slab->freelist = NULL  ← 被清空
c->freelist → Object 0 → Object 1 → ... → Object 9 → NULL  ← 所有对象移到这里
```

#### 阶段 2：CPU 从 c->freelist 分配对象

**位置**: `mm/slub.c:3975-4004`

```c
static __always_inline void *__slab_alloc_node(...)
{
    // 从 c->freelist 中取对象（Fast Path）
    object = c->freelist;
    
    if (likely(object && slab && node_match(slab, node))) {
        // 更新 c->freelist 指向下一个对象
        void *next_object = get_freepointer_safe(s, object);
        __update_cpu_freelist_fast(s, object, next_object, tid);
        return object;
    }
}
```

**流程**:
```
分配前：
c->freelist → Object 0 → Object 1 → ... → Object 9 → NULL
slab->inuse = 0

分配后：
c->freelist → Object 1 → ... → Object 9 → NULL
slab->inuse = 1  ← Object 0 被分配出去
```

#### 阶段 3：其他 CPU 释放对象到 slab->freelist

**位置**: `mm/slub.c:4549-4552`

```c
static __always_inline void do_slab_free(...)
{
    // 检查释放的对象是否在当前 CPU 的 slab 中
    if (unlikely(slab != c->slab)) {
        // 不是当前 CPU 的 slab，进入 Slow Path
        __slab_free(s, slab, head, tail, cnt, addr);
        return;
    }
}
```

**位置**: `mm/slub.c:4425-4453`

```c
static void __slab_free(...)
{
    // 释放对象到 slab->freelist（不是 c->freelist）
    prior = slab->freelist;
    set_freepointer(s, tail, prior);
    
    // 更新 slab->freelist
    slab_update_freelist(s, slab,
        prior, counters,
        head, new.counters,  // ← head 是释放的对象
        "__slab_free");
}
```

**流程**:
```
释放前（其他 CPU 释放 Object 0）：
c->freelist → Object 1 → ... → Object 9 → NULL  (CPU 0 的)
slab->freelist = NULL
slab->inuse = 1

释放后：
c->freelist → Object 1 → ... → Object 9 → NULL  (CPU 0 的，不变)
slab->freelist → Object 0 → NULL  ← 其他 CPU 释放的对象到这里
slab->inuse = 0
```

#### 阶段 4：当前 CPU 的 c->freelist 用完了

**位置**: `mm/slub.c:3719-3730`

```c
freelist = c->freelist;
if (freelist)
    goto load_freelist;  // ← 如果 c->freelist 还有对象，直接使用

// c->freelist 为空，尝试从 slab->freelist 获取
freelist = get_freelist(s, slab);

if (!freelist) {
    // slab->freelist 也为空，但 slab->inuse < slab->objects
    // 说明还有对象被分配出去了，但还没有释放回来
    c->slab = NULL;
    goto new_slab;  // ← 放弃这个 slab，获取新的
}
```

**流程**:
```
情况：
c->freelist = NULL  ← 当前 CPU 的 freelist 用完了
slab->freelist = NULL  ← slab 的 freelist 也为空（其他 CPU 还没有释放对象回来）
slab->inuse = 5  ← 还有 5 个对象被分配出去了
slab->objects = 10

结果：
CPU 放弃这个 slab，解冻它并转移到 node->partial
等待对象被释放回来后再使用
```

---

## 四、为什么会出现这种情况？

### 4.1 场景示例

```
时间线：

T1: CPU 0 冻结 Slab A
    - slab->freelist → Object 0-9 → NULL
    - c->freelist → Object 0-9 → NULL
    - slab->inuse = 0

T2: CPU 0 分配 Object 0-4
    - c->freelist → Object 5-9 → NULL
    - slab->inuse = 5

T3: CPU 1 释放 Object 0（通过 Slow Path）
    - slab->freelist → Object 0 → NULL  ← 其他 CPU 释放的对象
    - c->freelist → Object 5-9 → NULL  (CPU 0 的，不变)

T4: CPU 0 继续分配 Object 5-9
    - c->freelist = NULL  ← 用完了
    - slab->freelist → Object 0 → NULL  ← 还有对象，但不在 c->freelist 中

T5: CPU 0 尝试从 slab->freelist 获取
    - get_freelist(s, slab) 返回 Object 0
    - c->freelist → Object 0 → NULL
    - 继续使用

T6: CPU 0 分配 Object 0
    - c->freelist = NULL  ← 又用完了
    - slab->freelist = NULL  ← 也为空（其他 CPU 还没有释放 Object 1-4）
    - slab->inuse = 4  ← 还有 4 个对象被分配出去了

T7: CPU 0 放弃 Slab A
    - 解冻 Slab A
    - 转移到 node->partial
    - 等待 Object 1-4 被释放回来
```

### 4.2 关键代码

**位置**: `mm/slub.c:3596-3619`

```c
static inline void *get_freelist(struct kmem_cache *s, struct slab *slab)
{
    do {
        freelist = slab->freelist;  // ← 从 slab->freelist 获取
        counters = slab->counters;
        
        new.counters = counters;
        new.inuse = slab->objects;  // ← 设置 inuse = objects（所有对象都在使用）
        new.frozen = freelist != NULL;  // ← 如果 freelist 为空，解冻 slab
        
    } while (!__slab_update_freelist(s, slab,
        freelist, counters,
        NULL, new.counters,  // ← 清空 slab->freelist
        "get_freelist"));
    
    return freelist;  // ← 返回获取的对象
}
```

**关键理解**:
- `get_freelist()` 从 `slab->freelist` 中取走对象，放到 `c->freelist`
- 如果 `slab->freelist` 为空，`new.frozen = 0`，slab 被解冻
- 如果 `slab->freelist` 不为空，`new.frozen = 1`，slab 保持冻结状态

---

## 五、总结

### 5.1 关键点

1. **一个 frozen slab 只能被一个 CPU 使用**（冻结它的 CPU）
2. **其他 CPU 可以释放对象到 `slab->freelist`**（但不能取对象）
3. **`c->freelist` 是 CPU 的本地缓存**，从 `slab->freelist` 中取对象
4. **当 `c->freelist` 用完了，CPU 会从 `slab->freelist` 获取对象**
5. **如果 `slab->freelist` 也为空，但 `slab->inuse < slab->objects`**，说明还有对象被分配出去了，但还没有释放回来
6. **CPU 会放弃这个 slab，解冻它并转移到 `node->partial`**，等待对象被释放回来后再使用

### 5.2 为什么需要 node->partial？

- **等待对象释放**：当 CPU 的 `c->freelist` 和 `slab->freelist` 都用完了，但还有对象被分配出去时，CPU 会放弃这个 slab
- **其他 CPU 可以继续使用**：当对象被释放回来时，其他 CPU 可以从 `node->partial` 获取这个 slab
- **提高利用率**：避免 slab 还有对象就释放，造成浪费

---

## 六、代码位置总结

| 操作 | 函数 | 文件 | 行号 |
|------|------|------|------|
| 冻结 slab | `freeze_slab()` | `mm/slub.c` | 3624-3645 |
| 从 c->freelist 分配 | `__slab_alloc_node()` | `mm/slub.c` | 3975-4004 |
| 从 slab->freelist 获取 | `get_freelist()` | `mm/slub.c` | 3596-3619 |
| 其他 CPU 释放对象 | `__slab_free()` | `mm/slub.c` | 4400-4510 |
| 解冻并转移 | `deactivate_slab()` | `mm/slub.c` | 3047-3125 |

