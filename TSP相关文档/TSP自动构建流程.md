# TSP 自动编译流程详解

## 一、完整编译流程

```
设置 SPD=tspd
    │
    ▼
Makefile:250
include services/spd/tspd/tspd.mk
    │
    ├─> tspd.mk:30
    │   include bl32/tsp/tsp.mk
    │       │
    │       ├─> tsp.mk:13-24
    │       │   BL32_SOURCES += bl32/tsp/tsp_main.c ...
    │       │   (添加所有 TSP 源文件)
    │       │
    │       └─> tsp.mk:49
    │           include plat/.../tsp/tsp-${PLAT}.mk
    │           (添加平台特定的源文件)
    │
    └─> tspd.mk:33
        NEED_BL32 := yes
        (标记需要编译 BL32)
    │
    ▼
Makefile:1017-1029
ifeq (${NEED_BL32},yes)
    BL32_SOURCES := $(sort ${BL32_SOURCES})
    BUILD_BL32 := $(if $(BL32),,$(if $(BL32_SOURCES),1))
    (检查是否有 BL32_SOURCES)
    │
    └─> Makefile:1026
        $(eval $(call MAKE_BL,bl32,tos-fw))
        (调用 MAKE_BL 宏创建 bl32 目标)
            │
            ▼
        build_macros.mk:546-622
        define MAKE_BL
            (定义 bl32 的编译规则)
            │
            └─> build_macros.mk:622
                all: $(1)  # all: bl32
                (将 bl32 添加到 all 目标的依赖中)
```

## 二、关键代码位置

### 1. Makefile:250 - 包含 SPD Makefile

```makefile
# Makefile:244-250
SPD_MAKE := $(wildcard services/${SPD_DIR}/${SPD}/${SPD}.mk)
# 当 SPD=tspd 时，SPD_MAKE = services/spd/tspd/tspd.mk

ifeq (${SPD_MAKE},)
    $(error Error: No services/${SPD_DIR}/${SPD}/${SPD}.mk located)
endif
$(info Including ${SPD_MAKE})
include ${SPD_MAKE}  # ← 这里包含 tspd.mk
```

**作用**：包含 `tspd.mk`，这是触发 TSP 编译的入口。

### 2. tspd.mk:30 - 包含 TSP Makefile

```makefile
# services/spd/tspd/tspd.mk:25-30
BL32_ROOT := bl32/tsp

# Include SP's Makefile. The assumption is that the TSP's build system is
# compatible with that of Trusted Firmware, and it'll add and populate necessary
# build targets and variables
include ${BL32_ROOT}/tsp.mk  # ← 这里包含 bl32/tsp/tsp.mk
```

**作用**：包含 TSP 的 Makefile，这会添加 TSP 源文件到 `BL32_SOURCES`。

### 3. tsp.mk:13-24 - 添加 TSP 源文件

```makefile
# bl32/tsp/tsp.mk:13-24
ifeq (${SPMC_AT_EL3},1)
   BL32_SOURCES += bl32/tsp/tsp_ffa_main.c \
                   bl32/tsp/ffa_helpers.c
else
   BL32_SOURCES += bl32/tsp/tsp_main.c  # ← 添加 TSP 主文件
endif

BL32_SOURCES += bl32/tsp/aarch64/tsp_entrypoint.S \
                bl32/tsp/aarch64/tsp_exceptions.S \
                bl32/tsp/aarch64/tsp_request.S \
                bl32/tsp/tsp_interrupt.c \
                bl32/tsp/tsp_timer.c \
                bl32/tsp/tsp_common.c \
                bl32/tsp/tsp_context.c \
                common/aarch64/early_exceptions.S \
                lib/locks/exclusive/aarch64/spinlock.S
```

**作用**：将所有 TSP 源文件添加到 `BL32_SOURCES` 变量中。

### 4. tspd.mk:33 - 设置 NEED_BL32

```makefile
# services/spd/tspd/tspd.mk:32-33
# Let the top-level Makefile know that we intend to build the SP from source
NEED_BL32 := yes  # ← 关键：标记需要编译 BL32
```

**作用**：设置 `NEED_BL32 := yes`，告诉主 Makefile 需要编译 BL32。

### 5. Makefile:1017-1029 - 检查并创建 bl32 目标

```makefile
# Makefile:1017-1029
# If a BL32 image is needed but neither BL32 nor BL32_SOURCES is defined, the
# build system will call TOOL_ADD_IMG to print a warning message and abort the
# process. Note that the dependency on BL32 applies to the FIP only.
ifeq (${NEED_BL32},yes)  # ← 检查 NEED_BL32
# Sort BL32 source files to remove duplicates
BL32_SOURCES := $(sort ${BL32_SOURCES})  # ← 整理源文件列表
BUILD_BL32 := $(if $(BL32),,$(if $(BL32_SOURCES),1))  # ← 判断是否需要编译

ifneq (${DECRYPTION_SUPPORT},none)
$(if ${BUILD_BL32}, $(eval $(call MAKE_BL,bl32,tos-fw,,$(ENCRYPT_BL32))),\
    $(eval $(call TOOL_ADD_IMG,bl32,--tos-fw,,$(ENCRYPT_BL32))))
else
$(if ${BUILD_BL32}, $(eval $(call MAKE_BL,bl32,tos-fw)),\  # ← 调用 MAKE_BL 宏
    $(eval $(call TOOL_ADD_IMG,bl32,--tos-fw)))
endif #(DECRYPTION_SUPPORT)
endif #(NEED_BL32)
```

**作用**：
1. 检查 `NEED_BL32` 是否为 `yes`
2. 检查 `BL32_SOURCES` 是否有内容
3. 如果有，调用 `MAKE_BL(bl32,tos-fw)` 创建 bl32 编译目标

### 6. build_macros.mk:622 - 添加到 all 目标

```makefile
# make_helpers/build_macros.mk:546-622
define MAKE_BL
    # ... 定义编译规则 ...
    
    .PHONY: $(1)  # .PHONY: bl32
    ifeq ($(DISABLE_BIN_GENERATION),1)
    $(1): $(ELF) $(DUMP)  # bl32: bl32.elf bl32.dump
    else
    $(1): $(BIN) $(DUMP)  # bl32: bl32.bin bl32.dump
    endif
    
    all: $(1)  # ← 关键：将 bl32 添加到 all 目标的依赖中
    # 这意味着执行 'make all' 时会自动编译 bl32
endef
```

**作用**：`MAKE_BL` 宏会：
1. 定义 `bl32` 目标的编译规则
2. **将 `bl32` 添加到 `all` 目标的依赖中**
3. 这样执行 `make all` 时，会自动编译 `bl32`

## 三、关键变量传递链

```
SPD=tspd
    │
    ▼
Makefile 包含 services/spd/tspd/tspd.mk
    │
    ├─> 设置 NEED_BL32 := yes
    │
    └─> 包含 bl32/tsp/tsp.mk
            │
            └─> 添加文件到 BL32_SOURCES
                    │
                    ▼
Makefile 检查 NEED_BL32 == yes
    │
    └─> 检查 BL32_SOURCES 有内容
            │
            └─> BUILD_BL32 := 1
                    │
                    └─> 调用 MAKE_BL(bl32,...)
                            │
                            └─> 创建 bl32 目标
                                    │
                                    └─> all: bl32
```

## 四、验证流程

### 步骤 1：检查变量设置

```bash
cd /home/fbt1709/arm-trusted-firmware

# 检查 NEED_BL32 是否被设置
make PLAT=fvp SPD=tspd -n 2>&1 | grep NEED_BL32

# 检查 BL32_SOURCES 是否有内容
make PLAT=fvp SPD=tspd -n 2>&1 | grep "BL32_SOURCES" | head -5
```

### 步骤 2：检查目标依赖

```bash
# 查看 all 目标的依赖
make PLAT=fvp SPD=tspd -n 2>&1 | grep "^all:"
```

### 步骤 3：查看 bl32 目标定义

```bash
# 查看 bl32 目标的定义
make PLAT=fvp SPD=tspd -n 2>&1 | grep "^bl32:"
```

## 五、关键点总结

### 1. 触发点

**位置**：`Makefile:250`
```makefile
include ${SPD_MAKE}  # 包含 services/spd/tspd/tspd.mk
```

### 2. 源文件添加

**位置**：`bl32/tsp/tsp.mk:13-24`
```makefile
BL32_SOURCES += bl32/tsp/tsp_main.c ...
```

### 3. 编译标志设置

**位置**：`services/spd/tspd/tspd.mk:33`
```makefile
NEED_BL32 := yes
```

### 4. 目标创建

**位置**：`Makefile:1026`
```makefile
$(eval $(call MAKE_BL,bl32,tos-fw))
```

### 5. 添加到 all 目标

**位置**：`make_helpers/build_macros.mk:622`
```makefile
all: $(1)  # all: bl32
```

## 六、为什么只编译 bl31 时 TSP 不会编译？

```bash
make PLAT=fvp SPD=tspd bl31
```

**原因**：
- `bl31` 目标只依赖 `bl31.bin`
- `bl32` 目标没有被显式指定
- 虽然 `all: bl32` 存在，但 `bl31` 目标不依赖 `all`

**解决方案**：
```bash
# 方法 1：使用 all 目标（推荐）
make PLAT=fvp SPD=tspd all

# 方法 2：显式指定 bl32
make PLAT=fvp SPD=tspd bl32

# 方法 3：同时指定 bl31 和 bl32
make PLAT=fvp SPD=tspd bl31 bl32
```

## 七、总结

**TSP 自动编译的关键路径**：

1. ✅ `SPD=tspd` → 包含 `tspd.mk`
2. ✅ `tspd.mk` → 设置 `NEED_BL32 := yes` + 包含 `tsp.mk`
3. ✅ `tsp.mk` → 添加文件到 `BL32_SOURCES`
4. ✅ `Makefile` → 检查 `NEED_BL32` 和 `BL32_SOURCES`
5. ✅ `MAKE_BL(bl32,...)` → 创建 `bl32` 目标
6. ✅ `all: bl32` → 将 `bl32` 添加到 `all` 依赖

**因此**：当执行 `make PLAT=fvp SPD=tspd all` 时，TSP 会自动编译！

