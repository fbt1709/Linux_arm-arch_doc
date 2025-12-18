# SPD 配置详解：为什么仅仅设置 SPD 还不够

## 问题说明

你看到的代码：

```c
#if SPM_MM
    if (spm_mm_setup() != 0) {
        ret = 1;
    }
#endif

#if defined(SPD_spmd)
    if (spmd_setup() != 0) {
        ret = 1;
    }
#endif
```

这段代码说明：**仅仅设置 `SPD=spmd` 还不够，还需要其他配置选项**。

## 宏定义机制

### 1. SPD 宏的定义

在 `Makefile:822` 中：

```makefile
$(eval $(call add_defines,\
    ...
    SPD_${SPD} \      # ← 这里定义 SPD_spmd
    ...
))
```

**工作原理**：
- 当 `SPD=spmd` 时，`SPD_${SPD}` 展开为 `SPD_spmd`
- `add_defines` 会将其添加到 `DEFINES` 变量
- 编译时会生成 `-DSPD_spmd` 宏定义

**结果**：代码中的 `#if defined(SPD_spmd)` 会为真。

### 2. 其他配置宏的定义

在 `Makefile:824-827` 中：

```makefile
$(eval $(call add_defines,\
    ...
    SPM_MM \              # ← 需要单独设置 SPM_MM=1
    SPMC_AT_EL3 \
    SPMD_SPM_AT_SEL2 \     # ← 需要单独设置 SPMD_SPM_AT_SEL2=1
    ...
))
```

**关键点**：这些宏是**独立的配置选项**，不会因为设置 `SPD=spmd` 而自动定义。

## 完整的配置流程

### 示例：配置 SPMD

#### 步骤 1：设置 SPD

```bash
make PLAT=fvp SPD=spmd all
```

**结果**：
- ✅ `SPD_spmd` 宏被定义（`-DSPD_spmd`）
- ❌ `SPM_MM` 宏**不会**被定义（除非单独设置）
- ❌ `SPMD_SPM_AT_SEL2` 宏**不会**被定义（除非单独设置）

#### 步骤 2：设置其他必需的配置

```bash
# 如果需要 SPM_MM 功能
make PLAT=fvp SPD=spmd SPM_MM=1 all

# 如果需要 SPMD_SPM_AT_SEL2 功能
make PLAT=fvp SPD=spmd SPMD_SPM_AT_SEL2=1 all

# 如果需要多个配置
make PLAT=fvp SPD=spmd SPM_MM=1 SPMD_SPM_AT_SEL2=1 all
```

## 各种 SPD 的配置要求

### 1. TSPD（最简单）

```bash
# 只需要设置 SPD
make PLAT=fvp SPD=tspd all
```

**自动配置**：
- ✅ `SPD_tspd` 宏被定义
- ✅ `NEED_BL32 := yes`（在 `tspd.mk` 中设置）
- ✅ TSP 自动编译

### 2. OPTEED

```bash
# 基本配置
make PLAT=fvp SPD=opteed BL32=~/optee_os/out/arm-plat-fvp/core/tee.bin all
```

**自动配置**：
- ✅ `SPD_opteed` 宏被定义
- ✅ `NEED_BL32 := yes`（在 `opteed.mk` 中设置）
- ❌ 需要手动提供 `BL32`（OP-TEE 二进制）

### 3. SPMD（最复杂）

```bash
# 基本配置
make PLAT=fvp SPD=spmd all

# 如果需要 SPM_MM（Secure Partition Manager Memory Management）
make PLAT=fvp SPD=spmd SPM_MM=1 all

# 如果需要 SPM 在 S-EL2 运行
make PLAT=fvp SPD=spmd SPMD_SPM_AT_SEL2=1 SP_LAYOUT_FILE=sp_layout.json all

# 如果需要 Logical Partition
make PLAT=fvp SPD=spmd ENABLE_SPMD_LP=1 all
```

**配置选项**：
- `SPM_MM=1`：启用 SPM Memory Management
- `SPMD_SPM_AT_SEL2=1`：SPM 在 S-EL2 运行（需要 `SP_LAYOUT_FILE`）
- `ENABLE_SPMD_LP=1`：启用 Logical Partition
- `SP_LAYOUT_FILE=<file>`：SP 布局文件（JSON）

## 宏定义到编译选项的转换

### add_defines 函数

```makefile
# make_helpers/build_macros.mk:53
define add_defines
    $(foreach def,$1,$(eval $(call add_define,$(def))))
endef

# make_helpers/build_macros.mk:47
define add_define
    DEFINES += -D$(1)$(if $(value $(1)),=$(value $(1)),)
endef
```

**工作原理**：
1. `add_defines(SPD_spmd, SPM_MM)` 遍历每个变量名
2. 对每个变量调用 `add_define`
3. `add_define` 检查变量值：
   - 如果变量值为空或未定义 → `-DSPD_spmd`
   - 如果变量值非空 → `-DSPD_spmd=1`（例如 `SPM_MM=1` → `-DSPM_MM=1`）

### 实际编译选项

```bash
# 设置 SPD=spmd
gcc -DSPD_spmd ...

# 设置 SPD=spmd SPM_MM=1
gcc -DSPD_spmd -DSPM_MM=1 ...

# 设置 SPD=spmd SPMD_SPM_AT_SEL2=1
gcc -DSPD_spmd -DSPMD_SPM_AT_SEL2=1 ...
```

## 代码中的条件编译

### std_svc_setup.c

```c
// 只有当 SPM_MM=1 时，这段代码才会编译
#if SPM_MM
    if (spm_mm_setup() != 0) {
        ret = 1;
    }
#endif

// 只有当 SPD=spmd 时，这段代码才会编译
#if defined(SPD_spmd)
    if (spmd_setup() != 0) {
        ret = 1;
    }
#endif
```

**含义**：
- `#if SPM_MM`：检查 `SPM_MM` 宏是否定义（需要 `SPM_MM=1`）
- `#if defined(SPD_spmd)`：检查 `SPD_spmd` 宏是否定义（自动定义，当 `SPD=spmd` 时）

## 如何查找需要的配置

### 方法 1：查看 SPD 的 Makefile

```bash
# 查看 SPMD 的 Makefile
cat services/std_svc/spmd/spmd.mk

# 查看 TSPD 的 Makefile
cat services/spd/tspd/tspd.mk
```

### 方法 2：查看代码中的条件编译

```bash
# 搜索条件编译
grep -r "#if.*SPM\|#ifdef.*SPM" services/

# 搜索 SPMD 相关
grep -r "#if.*SPMD\|#ifdef.*SPMD" services/
```

### 方法 3：查看 Makefile 中的配置列表

```bash
# 查看所有可配置的宏
grep -A 100 "add_defines" Makefile | grep -E "SPM|SPMD"
```

## 常见配置组合

### 配置 1：基本 SPMD

```bash
make PLAT=fvp SPD=spmd all
```

**宏定义**：
- ✅ `SPD_spmd`
- ❌ `SPM_MM`
- ❌ `SPMD_SPM_AT_SEL2`

**代码执行**：
- ✅ `spmd_setup()` 会被调用
- ❌ `spm_mm_setup()` **不会**被调用

### 配置 2：SPMD + SPM_MM

```bash
make PLAT=fvp SPD=spmd SPM_MM=1 all
```

**宏定义**：
- ✅ `SPD_spmd`
- ✅ `SPM_MM`
- ❌ `SPMD_SPM_AT_SEL2`

**代码执行**：
- ✅ `spmd_setup()` 会被调用
- ✅ `spm_mm_setup()` 会被调用

### 配置 3：完整 SPMD 配置

```bash
make PLAT=fvp SPD=spmd \
     SPM_MM=1 \
     SPMD_SPM_AT_SEL2=1 \
     SP_LAYOUT_FILE=sp_layout.json \
     ENABLE_SPMD_LP=1 \
     all
```

**宏定义**：
- ✅ `SPD_spmd`
- ✅ `SPM_MM`
- ✅ `SPMD_SPM_AT_SEL2`
- ✅ `ENABLE_SPMD_LP`

## 总结

### 关键点

1. **`SPD=xxx` 只定义 `SPD_xxx` 宏**
   - 不会自动定义其他相关宏
   - 每个功能需要单独配置

2. **其他配置是独立的**
   - `SPM_MM=1`：需要单独设置
   - `SPMD_SPM_AT_SEL2=1`：需要单独设置
   - `ENABLE_SPMD_LP=1`：需要单独设置

3. **代码中的条件编译**
   - `#if defined(SPD_spmd)`：检查 SPD 宏
   - `#if SPM_MM`：检查功能宏（需要显式设置）

### 配置检查清单

使用 SPMD 时，检查：

- [ ] `SPD=spmd` 已设置
- [ ] 如果需要 SPM_MM 功能，设置 `SPM_MM=1`
- [ ] 如果需要 S-EL2 SPM，设置 `SPMD_SPM_AT_SEL2=1` 和 `SP_LAYOUT_FILE`
- [ ] 如果需要 Logical Partition，设置 `ENABLE_SPMD_LP=1`
- [ ] `NEED_BL32 := yes` 已在 `spmd.mk` 中设置（自动）

### 验证配置

```bash
# 编译时查看定义的宏
make PLAT=fvp SPD=spmd SPM_MM=1 -n 2>&1 | grep "DEFINES" | grep -E "SPM|SPMD"

# 或者查看编译命令
make PLAT=fvp SPD=spmd SPM_MM=1 V=1 2>&1 | grep "gcc" | head -1
```

