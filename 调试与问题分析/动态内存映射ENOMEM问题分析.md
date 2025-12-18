# 动态内存映射ENOMEM问题分析

## 问题描述

**现象**：
- 在`plat_setup.c`中添加了很多内存映射区域后
- `test_ea_ffh.c`中的`mmap_add_dynamic_region()`返回`-12`（ENOMEM）
- 无法申请动态内存映射区域

## 问题原因

### 根本原因：MAX_MMAP_REGIONS限制

**动态内存映射区域有数量限制**：
- 系统定义了`MAX_MMAP_REGIONS`常量
- 这个常量限制了内存映射区域的最大数量
- 包括：
  - **静态映射区域**（plat_setup.c中定义的）
  - **动态映射区域**（通过mmap_add_dynamic_region添加的）

### 内存映射区域分配机制

**内存映射区域数组结构**：
```c
// xlat_tables_context.c
REGISTER_XLAT_CONTEXT(tf, MAX_MMAP_REGIONS, MAX_XLAT_TABLES,
                      PLAT_VIRT_ADDR_SPACE_SIZE, PLAT_PHY_ADDR_SPACE_SIZE,
                      EL1_EL0_REGIME, tf_xlat_ctx);
```

**关键点**：
- 系统预分配一个固定大小的数组（`MAX_MMAP_REGIONS`）
- 静态映射区域占用数组的一部分
- 动态映射区域占用数组的剩余部分
- **如果静态映射区域太多，动态映射区域就没有空间了**

### mmap_add_dynamic_region的实现

```c
// xlat_tables_core.c
int mmap_add_dynamic_region_ctx(xlat_ctx_t *ctx, mmap_region_t *mm)
{
    // ...
    // 查找空闲的映射区域槽位
    for (i = ctx->mmap_num; i < ctx->mmap_entries_limit; i++) {
        if (ctx->mmap[i].size == 0U) {
            // 找到空闲槽位
            break;
        }
    }
    
    if (i >= ctx->mmap_entries_limit) {
        // 没有空闲槽位，返回ENOMEM
        return -ENOMEM;
    }
    
    // ...
}
```

**关键逻辑**：
- 从`ctx->mmap_num`（静态映射区域数量）开始查找
- 查找空闲的映射区域槽位
- 如果`i >= ctx->mmap_entries_limit`（MAX_MMAP_REGIONS），返回ENOMEM

## 解决方案

### 方案1：增加MAX_MMAP_REGIONS（推荐）

**在平台定义文件中增加MAX_MMAP_REGIONS**：

```c
// plat/arm/fvp/include/platform_def.h
#ifndef MAX_MMAP_REGIONS
#define MAX_MMAP_REGIONS    20  // 增加这个值（默认可能是10或16）
#endif
```

**优点**：
- 简单直接
- 不影响其他代码
- 可以同时支持更多静态和动态映射

**缺点**：
- 增加内存占用（每个映射区域占用一个mmap_region_t结构体）

### 方案2：减少静态映射区域

**优化plat_setup.c中的映射区域**：

```c
// 合并相邻的映射区域
// 例如：如果DEVICE0和DEVICE1相邻，可以合并为一个区域
MAP_REGION_FLAT(DEVICE0_BASE, DEVICE0_SIZE + DEVICE1_SIZE, 
                MT_DEVICE | MT_RW | MT_NS),
```

**优点**：
- 不增加内存占用
- 优化映射结构

**缺点**：
- 可能需要调整映射区域的大小和对齐
- 如果区域属性不同，无法合并

### 方案3：使用后立即释放动态映射

**在test_ea_ffh.c中及时释放映射**：

```c
test_result_t test_inject_syncEA(void)
{
    int rc;
    
    // 添加动态映射
    rc = mmap_add_dynamic_region(TEST_ADDRESS, TEST_ADDRESS, PAGE_SIZE,
                                 MT_DEVICE | MT_RO | MT_NS);
    if (rc != 0) {
        return TEST_RESULT_FAIL;
    }
    
    // 立即使用
    rc = mmio_read_32(TEST_ADDRESS);
    
    // 立即释放
    rc = mmap_remove_dynamic_region(TEST_ADDRESS, PAGE_SIZE);
    
    return TEST_RESULT_SUCCESS;
}
```

**优点**：
- 不增加内存占用
- 及时释放资源

**缺点**：
- 如果测试需要保持映射，无法使用

### 方案4：检查当前映射区域数量

**添加调试代码检查映射区域使用情况**：

```c
void check_mmap_usage(void)
{
    // 需要访问xlat_tables的内部结构
    // 或者添加调试接口
    tftf_testcase_printf("当前映射区域使用情况:\n");
    tftf_testcase_printf("  静态映射: %d\n", static_mmap_count);
    tftf_testcase_printf("  动态映射: %d\n", dynamic_mmap_count);
    tftf_testcase_printf("  最大映射: %d\n", MAX_MMAP_REGIONS);
    tftf_testcase_printf("  剩余空间: %d\n", 
                         MAX_MMAP_REGIONS - static_mmap_count - dynamic_mmap_count);
}
```

## 诊断方法

### 方法1：检查MAX_MMAP_REGIONS的值

```c
// 在测试代码中添加
tftf_testcase_printf("MAX_MMAP_REGIONS = %d\n", MAX_MMAP_REGIONS);
```

### 方法2：检查静态映射区域数量

```c
// 在plat_setup.c中计算
void count_static_mmap_regions(void)
{
    const mmap_region_t *mmap = tftf_platform_get_mmap();
    int count = 0;
    
    while (mmap[count].size != 0) {
        count++;
    }
    
    tftf_testcase_printf("静态映射区域数量: %d\n", count);
}
```

### 方法3：检查动态映射区域使用情况

```c
// 在mmap_add_dynamic_region失败时打印
if (rc == -ENOMEM) {
    tftf_testcase_printf("动态映射失败: ENOMEM\n");
    tftf_testcase_printf("可能原因: MAX_MMAP_REGIONS限制\n");
    tftf_testcase_printf("建议: 增加MAX_MMAP_REGIONS或减少静态映射\n");
}
```

## 具体解决步骤

### 步骤1：查找MAX_MMAP_REGIONS的定义

```bash
# 在平台定义文件中查找
grep -r "MAX_MMAP_REGIONS" plat/arm/fvp/
```

### 步骤2：增加MAX_MMAP_REGIONS

```c
// plat/arm/fvp/include/platform_def.h
#ifndef MAX_MMAP_REGIONS
#define MAX_MMAP_REGIONS    20  // 从默认值（如10）增加到20
#endif
```

### 步骤3：验证

```c
// 重新编译并测试
// 检查mmap_add_dynamic_region是否成功
```

## 总结

**问题原因**：
- ✅ **MAX_MMAP_REGIONS限制**：内存映射区域有数量限制
- ✅ **静态映射占用过多**：plat_setup.c中的映射区域占用了大部分空间
- ✅ **动态映射空间不足**：剩余空间不足以添加新的动态映射

**解决方案**：
1. **增加MAX_MMAP_REGIONS**（最简单，推荐）
2. **减少静态映射区域**（优化映射结构）
3. **及时释放动态映射**（如果可能）
4. **检查映射区域使用情况**（诊断问题）

**建议**：
- 首先检查MAX_MMAP_REGIONS的当前值
- 计算静态映射区域的数量
- 根据需求增加MAX_MMAP_REGIONS
- 如果可能，优化静态映射区域的结构

