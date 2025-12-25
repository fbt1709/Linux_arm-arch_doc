### TF-A 顶层 `Makefile` 工作机制说明

本说明文档基于当前仓库下的 `arm-trusted-firmware/Makefile`，按执行顺序梳理其核心工作流程，帮助理解整个构建系统的结构和关键逻辑。

---

## 1. 顶层结构与默认目标

```makefile
# Trusted Firmware Version
VERSION_MAJOR := 2
VERSION_MINOR := 13
VERSION_PATCH := 0
VERSION       := ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}

# 默认目标是构建所有镜像
.DEFAULT_GOAL := all

MAKEOVERRIDES =
```

- **版本号**：组合成 `VERSION=2.13.0`，用于日志与版本字符串。
- **`.DEFAULT_GOAL := all`**：当只执行 `make` 而不带目标时，默认执行 `all` 目标。
- **`MAKEOVERRIDES =`**：清空命令行变量的隐式传播，避免如 `CFLAGS` 之类的命令行变量污染子 Makefile，仅保留 `-s` 等命令行选项。

---

## 2. 引入构建辅助模块（helpers）

```makefile
MAKE_HELPERS_DIRECTORY := make_helpers/
include ${MAKE_HELPERS_DIRECTORY}build_macros.mk
include ${MAKE_HELPERS_DIRECTORY}build-rules.mk
include ${MAKE_HELPERS_DIRECTORY}common.mk
```

- **`build_macros.mk`**：核心宏集合，负责：
  - 镜像构建宏 `MAKE_BL`（生成 BL1/BL2/BL31/BL32/RMM 等的规则）
  - FIP 打包宏 `TOOL_ADD_IMG`、`TOOL_ADD_PAYLOAD` 等
  - 各种检查宏：`assert_boolean(s)`、`assert_numeric(s)` 等
- **`build-rules.mk`**：通用规则，例如目录生成 `%/: mkdir -p ...`。
- **`common.mk`**：
  - 包含 `utilities.mk`（字符串、路径、布尔处理工具函数）
  - 设置静音/详细输出控制变量：
    - `s := @echo ...` 风格
    - `q := @` 控制是否打印命令本身

这些文件为随后的构建逻辑提供“底层积木”。

---

## 3. 默认配置与平台选取

```makefile
################################################################################
# 构建配置的默认值及其依赖关系
################################################################################

include ${MAKE_HELPERS_DIRECTORY}defaults.mk
PLAT := ${DEFAULT_PLAT}
include ${MAKE_HELPERS_DIRECTORY}plat_helpers.mk

# 为了能够设置平台特定的默认值
ifneq ($(PLAT_DEFAULTS_MAKEFILE_FULL),)
include ${PLAT_DEFAULTS_MAKEFILE_FULL}
endif
```

1. **`defaults.mk`**：
   - 定义所有构建选项的默认值，例如：
     - `ARCH := aarch64`
     - `DEFAULT_PLAT := fvp`
     - `DEBUG := 0`
     - `ENABLE_PMF := 0` 等。
   - 只给“全局缺省值”，不带平台差异。

2. **`PLAT := ${DEFAULT_PLAT}`**：
   - 如果命令行未指定 `PLAT`，默认为 `fvp` 等。

3. **`plat_helpers.mk`**：
   - 根据 `PLAT` 扫描 `plat/` 目录，定位：
     - 对应的 `platform.mk` → `PLAT_MAKEFILE_FULL`
     - 可选的 `platform_defaults.mk` → `PLAT_DEFAULTS_MAKEFILE_FULL`
   - 同时建立 `PLAT_DIR` 等变量。

4. **`platform_defaults.mk`（可选）**：
   - 某些平台在自身 `platform.mk` 之前，还需要设置一层“平台默认值”（例如 `ENABLE_LTO=1`），就会用到。
   - 大多数平台并不存在这个文件，只有极少数特殊平台需要。

整体顺序是：

> **全局默认值（defaults.mk） → 平台定位（plat_helpers.mk） → 平台默认值（platform_defaults.mk，可选）**

---

## 4. 工具链与调试级别

```makefile
include ${MAKE_HELPERS_DIRECTORY}toolchain.mk

# 默认情况下，DEBUG 构建启用断言
ENABLE_ASSERTIONS := ${DEBUG}
ENABLE_PMF        := ${ENABLE_RUNTIME_INSTRUMENTATION}
```

- **`toolchain.mk`**：
  - 定义并检测 host / aarch32 / aarch64 各类工具链：`$(ARCH)-cc`、`host-cc` 等。
  - 擦除某些 make 默认的 `CC/LD/AS` 设置，确保构建系统完全掌控工具链选择。

- **`ENABLE_ASSERTIONS := ${DEBUG}`**：
  - 当 `DEBUG=1` 时自动启用断言。
- **`ENABLE_PMF := ${ENABLE_RUNTIME_INSTRUMENTATION}`**：
  - 将 PMF（性能测量框架）的启用与运行时插桩挂钩。

---

## 5. 辅助工具路径与构建类型

```makefile
# 证书工具
CRTTOOLPATH ?= tools/cert_create
CRTTOOL     ?= ${BUILD_PLAT}/${CRTTOOLPATH}/cert_create$(.exe)

# 加密工具
ENCTOOLPATH ?= tools/encrypt_fw
ENCTOOL     ?= ${BUILD_PLAT}/${ENCTOOLPATH}/encrypt_fw$(.exe)

# FIP 工具
FIPTOOLPATH ?= tools/fiptool
FIPTOOL     ?= ${BUILD_PLAT}/${FIPTOOLPATH}/fiptool$(.exe)

# 调试/发布构建类型
ifneq (${DEBUG}, 0)
	BUILD_TYPE := debug
	LOG_LEVEL  := 40
else
	BUILD_TYPE := release
	LOG_LEVEL  := 20
endif

# 默认构建字符串（git 分支和提交）
ifeq (${BUILD_STRING},)
	BUILD_STRING := $(shell git describe --always --dirty --tags 2> /dev/null)
endif
VERSION_STRING := v${VERSION}(${BUILD_TYPE}):${BUILD_STRING}

DEFINES += -DBUILD_MESSAGE_TIMESTAMP='$(BUILD_MESSAGE_TIMESTAMP)'
DEFINES += -DBUILD_MESSAGE_VERSION_STRING='"$(VERSION_STRING)"'
DEFINES += -DBUILD_MESSAGE_VERSION='"$(VERSION)"'
```

- 定义各种工具在 **构建输出目录** 下的路径。
- 根据 `DEBUG` 区分 debug / release，并设置默认日志级别。
- 通过 `git describe` 自动生成 `BUILD_STRING`。
- 组合成 `VERSION_STRING`，并通过 `DEFINES` 传递给 C 代码（用于启动日志等）。

---

## 6. 公共源文件与包含路径

```makefile
BL_COMMON_SOURCES += \
	common/bl_common.c \
	common/tf_log.c \
	common/${ARCH}/debug.S \
	... \
	${COMPILER_RT_SRCS}

INCLUDES += \
	-Iinclude \
	-Iinclude/arch/${ARCH} \
	-Iinclude/lib/cpus/${ARCH} \
	-Iinclude/lib/el3_runtime/${ARCH} \
	${PLAT_INCLUDES} \
	${SPD_INCLUDES}
```

- `BL_COMMON_SOURCES`：所有 BL 阶段共享的一组通用源文件。
- `INCLUDES`：通用 include 路径 + 平台 include + SPD include。

后续各 BL（BL1/BL2/BL31/BL32 等）会在自己的 `blX.mk` 里基于这些公共源再追加自身专有源文件。

---

## 7. SPD（Secure Payload Dispatcher）处理

```makefile
ifneq (${SPD},none)
	ifeq (${SPD},spmd)
		SPD_DIR := std_svc
		...
	else
		SPD_DIR := spd
	endif

	SPD_MAKE := $(wildcard services/${SPD_DIR}/${SPD}/${SPD}.mk)
	ifeq (${SPD_MAKE},)
		$(error Error: No services/${SPD_DIR}/${SPD}/${SPD}.mk located)
	endif
	$(info Including ${SPD_MAKE})
	include ${SPD_MAKE}

	# SPD Makefile 中会决定是否需要 BL32、BL32_SOURCES/BL32 或其它
endif
```

- 根据 `SPD` 选择并包含相应的 SPD Makefile（如 `spmd`, `tspd`, `opteed` 等）。
- SPD 子 Makefile 会：
  - 提供 `SPD_SOURCES`
  - 可能设置 `NEED_BL32=yes` 等。

---

## 8. 包含平台 `platform.mk` 并解析架构特性

```makefile
include ${PLAT_MAKEFILE_FULL}

include ${MAKE_HELPERS_DIRECTORY}arch_features.mk
```

1. **`platform.mk`**：平台真正决定：
   - `PLAT_INCLUDES`：平台包含路径
   - 各 BL 的 `BL1_SOURCES/BL2_SOURCES/BL31_SOURCES/BL32_SOURCES` 等
   - 平台特定宏定义（通过 `add_define` 宏添加到 `DEFINES`）

2. **`arch_features.mk`**：
   - 根据 `ARM_ARCH_MAJOR.ARM_ARCH_MINOR` 决定默认启用哪些架构特性宏（`ENABLE_FEAT_*`）。

---

## 9. 分支保护与 RME 相关配置

```makefile
# 处理 BRANCH_PROTECTION 值并设置 BTI/PAUTH
ifeq (${BRANCH_PROTECTION},0)
	BP_OPTION := none
else ifeq (${BRANCH_PROTECTION},1)
	BP_OPTION := standard
	ENABLE_BTI := 1
	ENABLE_PAUTH := 1
...
endif

# RME 相关：当 ENABLE_RME=1 时，强制包含 EL2 上下文、PAUTH 等
ifeq (${ENABLE_RME},1)
	CTX_INCLUDE_EL2_REGS := 1
	CTX_INCLUDE_AARCH32_REGS := 0
	CTX_INCLUDE_PAUTH_REGS := 1
	...
endif

# 根据 RESET_TO_BL2 / ENABLE_RME 决定 BL2 是否在 EL3 运行
ifeq (${RESET_TO_BL2},1)
	BL2_RUNS_AT_EL3 := 1
	ifeq (${ENABLE_RME},1)
		$(error RESET_TO_BL2=1 and ENABLE_RME=1 configuration is not supported)
	endif
else ifeq (${ENABLE_RME},1)
	BL2_RUNS_AT_EL3 := 1
else
	BL2_RUNS_AT_EL3 := 0
endif
```

- 这些属于 **“全局行为开关”**，影响后续 C 代码的编译路径（通过 `DEFINES` 转为 `#define`）。

---

## 10. 是否需要各个 BL 镜像（NEED_BLx）

```makefile
ifdef BL1_SOURCES
	NEED_BL1 := yes
endif

ifdef BL2_SOURCES
	NEED_BL2 := yes

	# 使用 BL2 通常意味着需要 BL33（除非特殊配置）
	ifdef EL3_PAYLOAD_BASE
		NEED_BL33 := no
	else
		ifdef PRELOADED_BL33_BASE
			NEED_BL33 := no
		else
			NEED_BL33 ?= yes
		endif
	endif
endif

ifdef BL2U_SOURCES
	NEED_BL2U := yes
endif

ifneq (${ARCH},aarch32)
	ifdef BL31_SOURCES
		ifndef EL3_PAYLOAD_BASE
			NEED_BL31 := yes
		endif
	endif
endif
```

- 通过是否定义 `BLx_SOURCES` 和特殊配置（如 `EL3_PAYLOAD_BASE`、`PRELOADED_BL33_BASE`），决定：
  - 是否需要构建 BL1/BL2/BL2U/BL31/BL33 等。
  - 是否在 FIP 中包含 BL33。

---

## 11. TBB/TBBR & 证书生成/打包路径

```makefile
ifneq (${GENERATE_COT},0)
	# 通用 cert_create 选项（-n/-k 等）
	ifneq (${CREATE_KEYS},0)
		$(eval CRT_ARGS += -n)
		...
	endif
	# 包含 TBBR 工具 Makefile
	ifeq (${INCLUDE_TBBR_MK},1)
		include make_helpers/tbbr/tbbr_tools.mk
	endif
endif
```

`tbbr_tools.mk` 里：

- 定义要生成的 **各类证书**：
  - `tb_fw.crt`（BL2）
  - `soc_fw_content.crt` / `soc_fw_key.crt`（BL31）
  - `tos_fw_content.crt` / `tos_fw_key.crt`（BL32）
  - `nt_fw_content.crt` / `nt_fw_key.crt`（BL33）
- 用 `CERT_ADD_CMD_OPT` 填充 `CRT_ARGS`，用于 `cert_create`。
- 用 `TOOL_ADD_PAYLOAD(... *.crt ...)` 把这些证书通过 `FIP_ARGS += --xxx-cert file` 的形式注册到 FIP。

主 Makefile 中关于证书：

```makefile
certificates: ${CRT_DEPS} ${CRTTOOL} ${DTBS}
	${CRTTOOL} ${CRT_ARGS}
```

- 先保证 `cert_create` 工具本身构建好（`${CRTTOOL}` 目标）。
- 再用 `${CRT_ARGS}` 告诉 `cert_create` 生成哪些证书 `.crt`。

最终打包 FIP：

```makefile
${BUILD_PLAT}/${FIP_NAME}: ${FIP_DEPS} ${FIPTOOL}
	$(eval ${CHECK_FIP_CMD})
	${FIPTOOL} create ${FIP_ARGS} $@
	${FIPTOOL} info $@
```

- 所有 `TOOL_ADD_IMG`/`TOOL_ADD_PAYLOAD` 累积的 `FIP_ARGS` 决定 FIP 中包含的镜像和证书。

---

## 12. 选项合法性检查：布尔 & 数值

```makefile
# 布尔标志检查
$(eval $(call assert_booleans,
    $(sort \
        DEBUG \
        ENABLE_PMF \
        TRUSTED_BOARD_BOOT \
        ...
)))

# 数值标志检查
$(eval $(call assert_numerics,
    $(sort \
        ARM_ARCH_MAJOR \
        ARM_ARCH_MINOR \
        BRANCH_PROTECTION \
        ENABLE_RME \
        W \
        ...
)))
```

- `assert_boolean` / `assert_booleans`：确保变量是布尔型（0/1/yes/no 等）。
- `assert_numeric` / `assert_numerics`：确保变量只包含数字，不为空。

如果用户或平台 Makefile 给这些变量填了非法值，会在**构建一开始就报错**。

---

## 13. 生成编译宏：`DEFINES` 与 `add_define`

`build_macros.mk` 中：

```makefile
define add_define
    DEFINES += -D$(1)$(if $(value $(1)),=$(value $(1)),)
endef
```

含义：

- `$(call add_define,FOO)`：
  - 如果 `FOO` 未赋值或为空 → 追加 `-DFOO`
  - 如果 `FOO := 3` → 追加 `-DFOO=3`

主 Makefile 中：

```makefile
$(eval $(call add_defines,
    $(sort \
        ARM_ARCH_MAJOR \
        ARM_ARCH_MINOR \
        ENABLE_RME \
        TRUSTED_BOARD_BOOT \
        ...
)))
```

- `add_defines` 会遍历一长串变量名，调用 `add_define`，统一形成 `DEFINES`。
- 最终 `DEFINES` 会被拼到 `CFLAGS/CPPFLAGS` 里传给编译器，变成 C 里的 `#define`。

---

## 14. SP（Secure Partition）相关：SP_LAYOUT_FILE & sp_gen.mk

```makefile
ifeq (${SPD},spmd)
ifdef SP_LAYOUT_FILE
	-include $(BUILD_PLAT)/sp_gen.mk
	FIP_DEPS += sp
	CRT_DEPS += sp
	NEED_SP_PKG := yes
else
	ifeq (${SPMD_SPM_AT_SEL2},1)
		$(error "SPMD with SPM at S-EL2 require SP_LAYOUT_FILE")
	endif
endif
endif
```

- 当 `SPD=spmd` 且配置了 `SP_LAYOUT_FILE`（SP 布局 JSON）时：
  - 通过 `sp_mk_generator.py` 生成 `sp_gen.mk`
  - 里面填充：
    - `FDT_SOURCES += spX.dts`
    - `SPTOOL_ARGS += ...`
    - `FIP_ARGS += --blob uuid=... file=spX.pkg`
    - `CRT_ARGS += --sp-pkgX spX.pkg`

构建 SP 包与相关证书/FIP：

```makefile
ifeq (${NEED_SP_PKG},yes)
$(BUILD_PLAT)/sp_gen.mk: ${SP_MK_GEN} ${SP_LAYOUT_FILE} | $$(@D)/
	... 调用 sp_mk_generator.py ...
sp: $(DTBS) $(BUILD_PLAT)/sp_gen.mk $(SP_PKGS)
	echo "Built SP Images successfully"
endif
```

---

## 15. 编译/链接标志与 BL-specific Makefile

```makefile
include ${MAKE_HELPERS_DIRECTORY}cflags.mk

ifeq (${NEED_BL1},yes)
include bl1/bl1.mk
endif
ifeq (${NEED_BL2},yes)
include bl2/bl2.mk
endif
ifeq (${NEED_BL2U},yes)
include bl2u/bl2u.mk
endif
ifeq (${NEED_BL31},yes)
include bl31/bl31.mk
endif
...
```

- `cflags.mk`：
  - 根据 `ARCH`、`DEBUG`、`WARN` 等设置全局 `CFLAGS/LDFLAGS`。
- `blX.mk`：
  - 每个 BL 阶段自己的编译规则和源文件列表。
  - 借助 `MAKE_BL` 宏展开出真正的 `*.o → *.elf → *.bin` 规则，并将镜像加入 FIP。

---

## 16. FIP/FWU_FIP 的最终生成

```makefile
ifeq (${SEPARATE_BL2_FIP},1)
${BUILD_PLAT}/${BL2_FIP_NAME}: ${BL2_FIP_DEPS} ${FIPTOOL}
	$(eval ${CHECK_BL2_FIP_CMD})
	${FIPTOOL} create ${BL2_FIP_ARGS} $@
	${FIPTOOL} info $@
endif

${BUILD_PLAT}/${FIP_NAME}: ${FIP_DEPS} ${FIPTOOL}
	$(eval ${CHECK_FIP_CMD})
	${FIPTOOL} create ${FIP_ARGS} $@
	${FIPTOOL} info $@

${BUILD_PLAT}/${FWU_FIP_NAME}: ${FWU_FIP_DEPS} ${FIPTOOL}
	$(eval ${CHECK_FWU_FIP_CMD})
	${FIPTOOL} create ${FWU_FIP_ARGS} $@
	${FIPTOOL} info $@
```

- `FIP_ARGS`、`BL2_FIP_ARGS`、`FWU_FIP_ARGS` 全是前面各种 `MAKE_BL`、`TOOL_ADD_IMG`、`TOOL_ADD_PAYLOAD` 等宏一点点积累出来的。
- 这里是真正**调用 fiptool** 把所有镜像和证书封装成一个或多个 FIP 文件的地方。

---

## 17. 顶层构建目标（all / clean / fip 等）

```makefile
.PHONY: all msg_start clean realclean distclean cscope locate-checkpatch \
        checkcodebase checkpatch fiptool sptool fip sp tl fwu_fip certtool \
        dtbs memmap doc enctool

all: msg_start

msg_start:
	$(s)echo "Building ${PLAT}"

# 展开 c 语言静态库的构建规则
$(eval $(call MAKE_LIB,c))
```

- `.PHONY` 声明所有这些为伪目标，防止同名文件干扰（例如有个叫 `all` 的文件）。
- `all` 一般会通过 `MAKE_BL` 等宏间接触发所有 BL 镜像的构建和 FIP/FWU_FIP 生成。

常用目标：

- `make all`：构建所有 BL 镜像和 FIP。
- `make fip`：只生成 FIP 文件（前提是镜像已构建）。
- `make clean` / `realclean`：清理构建输出。
- `make certtool` / `fiptool` / `enctool`：只构建相应工具。
- `make checkcodebase` / `checkpatch`：风格检查。

---

## 18. 小结

顶层 `Makefile` 的核心职责可以概括为：

1. **配置收集与合并**：defaults → 平台 → SPD → RME 等特殊选项。
2. **合法性检查**：布尔/数值/约束（如 ENABLE_RME 与 ARCH/ SPD 的兼容性）。
3. **宏定义生成**：通过 `add_define(s)` 把 Make 变量映射到 C 预处理宏。
4. **源文件组织与规则展开**：通过 `MAKE_BL` 等宏自动生成 BL 各阶段的编译/链接/FIP 注册规则。
5. **证书生成与打包**：
   - `cert_create` + `CRT_ARGS` 生成各固件证书。
   - `TOOL_ADD_PAYLOAD(... *.crt ...)` 和 `FIP_ARGS` 把证书与镜像一并打入 FIP。
6. **工具与子工程调度**：`-C tools/...` 调用子目录 Makefile 构建工具。
7. **提供一致的构建入口**：`all` / `fip` / `clean` 等用户友好目标。

理解了这套顶层 Makefile 之后，再看平台的 `platform.mk`、各 BL 的 `blX.mk`，以及 `tbbr_tools.mk`/`build_macros.mk`，整体构建流程就比较清晰了。