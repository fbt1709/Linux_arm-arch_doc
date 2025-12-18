# ARM64 内核启动中的映射创建流程

## 映射创建的三个阶段

### 阶段 1: 身份映射（Identity Map）
**位置**: `arch/arm64/kernel/head.S:88-93`

```asm
adrp	x0, init_idmap_pg_dir
mov	x1, xzr
bl	__pi_create_init_idmap
```

**作用**: 
- 创建恒等映射（VA = PA），用于安全地启用 MMU
- 映射范围：内核代码段 + FDT + 页表操作代码
- 存储在 `init_idmap_pg_dir`，加载到 **TTBR0_EL1**

**为什么需要**:
- MMU 启用瞬间，CPU 仍在执行代码
- 需要确保代码和数据在 MMU 启用后仍可访问
- 恒等映射保证 VA=PA，代码继续执行

---

### 阶段 2: 早期内核线性映射（Early Kernel Linear Map）
**位置**: `arch/arm64/kernel/head.S:531`

```asm
bl	__pi_early_map_kernel		// Map and relocate the kernel
```

**实现**: `arch/arm64/kernel/pi/map_kernel.c::map_kernel()`

**作用**:
- 映射内核代码段到高虚拟地址（`KIMAGE_VADDR`）
- 映射范围：
  - `_stext` → `_etext` (代码段，只读可执行)
  - `__start_rodata` → `__inittext_begin` (只读数据)
  - `__inittext_begin` → `__inittext_end` (初始化代码)
  - `__initdata_begin` → `__initdata_end` (初始化数据)
  - `_data` → `_end` (数据段)
- 处理 KASLR 重定位
- 将页表复制到 `swapper_pg_dir`，加载到 **TTBR1_EL1**

**关键代码** (`map_kernel.c:81-90`):
```c
map_segment(init_pg_dir, &pgdp, va_offset, _stext, _etext, prot, ...);
map_segment(init_pg_dir, &pgdp, va_offset, __start_rodata, ...);
map_segment(init_pg_dir, &pgdp, va_offset, __initdata_begin, ...);
map_segment(init_pg_dir, &pgdp, va_offset, _data, _end, ...);
```

**此时状态**:
- TTBR0: 指向身份映射（恒等映射）
- TTBR1: 指向内核线性映射（高虚拟地址）
- 内核代码可以正常运行在高虚拟地址

---

### 阶段 3: 完整线性映射（Full Linear Map）
**位置**: `arch/arm64/kernel/setup.c:335` → `arch/arm64/mm/mmu.c:808`

```c
void __init paging_init(void)
{
	map_mem(swapper_pg_dir);  // ← 这里创建完整的线性映射！
	...
}
```

**实现**: `arch/arm64/mm/mmu.c::map_mem()`

**作用**:
- 映射**所有物理内存**到线性映射区域
- 线性映射公式：`VA = PA + PAGE_OFFSET`
- 映射范围：从 `PAGE_OFFSET` 开始的所有 memblock 内存区域

**关键代码** (`mmu.c:623-686`):
```c
static void __init map_mem(pgd_t *pgdp)
{
	// 遍历所有 memblock 内存区域
	for_each_mem_range(i, &start, &end) {
		if (start >= end)
			break;
		// 创建线性映射：VA = PA + PAGE_OFFSET
		__map_memblock(pgdp, start, end, pgprot_tagged(PAGE_KERNEL), flags);
	}
	
	// 映射内核代码的线性别名（用于休眠等功能）
	__map_memblock(pgdp, kernel_start, kernel_end, PAGE_KERNEL, ...);
}
```

**映射关系**:
```
物理地址范围          →  虚拟地址范围（线性映射）
[0x40000000, ...]   →  [PAGE_OFFSET, PAGE_OFFSET + size]
```

**此时状态**:
- TTBR1: 指向 `swapper_pg_dir`，包含：
  - 内核代码的高虚拟地址映射（阶段 2 创建）
  - 所有物理内存的线性映射（阶段 3 创建）
- 内核可以访问所有物理内存

---

## 完整时序图

```
启动流程：
┌─────────────────────────────────────────────────────────────┐
│ 1. head.S:88-93                                             │
│    __pi_create_init_idmap()                                 │
│    → 创建身份映射（TTBR0）                                   │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 2. head.S:524                                               │
│    __enable_mmu()                                           │
│    → 启用 MMU，使用身份映射（TTBR0）                         │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 3. head.S:531                                               │
│    __pi_early_map_kernel()                                  │
│    → 创建内核代码的线性映射（TTBR1）                         │
│    → 切换到 TTBR1，内核运行在高虚拟地址                      │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 4. start_kernel() → setup_arch() → paging_init()           │
│    map_mem(swapper_pg_dir)                                  │
│    → 创建所有物理内存的完整线性映射（TTBR1）                │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 5. setup.c:321                                              │
│    cpu_uninstall_idmap()                                    │
│    → 卸载身份映射（TTBR0 不再需要）                          │
└─────────────────────────────────────────────────────────────┘
```

---

## 为什么需要三个阶段？

1. **身份映射（阶段 1）**:
   - 安全启用 MMU 的过渡
   - 保证 MMU 启用瞬间代码仍可执行

2. **早期内核映射（阶段 2）**:
   - 内核代码需要运行在高虚拟地址
   - 此时 memblock 还未初始化，无法映射所有内存

3. **完整线性映射（阶段 3）**:
   - memblock 已初始化，知道所有物理内存范围
   - 创建完整的线性映射，内核可以访问所有物理内存
   - 用于 `__pa()` / `__va()` 转换

---

## 关键数据结构

- **`init_idmap_pg_dir`**: 身份映射页表（TTBR0）
- **`init_pg_dir`**: 临时页表（用于早期映射）
- **`swapper_pg_dir`**: 最终页表（TTBR1），包含：
  - 内核代码的高虚拟地址映射
  - 所有物理内存的线性映射

---

## 总结

**你看到的 `head.S:88-93` 只是创建了身份映射**，线性映射的创建在：

1. **早期阶段**: `head.S:531` → `__pi_early_map_kernel()` 
   - 映射内核代码到高虚拟地址

2. **完整阶段**: `setup.c:335` → `paging_init()` → `map_mem()`
   - 映射所有物理内存到线性映射区域

所以线性映射的创建**不在 `head.S` 的 88-93 行**，而是在后面的 `__pi_early_map_kernel()` 和 `paging_init()` 中！

