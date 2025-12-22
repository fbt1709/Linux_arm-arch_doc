# CERT_ADD_CMD_OPT 宏详解

## 概述

`CERT_ADD_CMD_OPT` 是一个 **Makefile 宏**，用于向 `cert_create` 工具的命令行参数列表（`CRT_ARGS`）中添加参数。

---

## 宏定义

**位置**：`make_helpers/build_macros.mk` 第 207-213 行

```makefile
# CERT_ADD_CMD_OPT adds a new command line option to the cert_create invocation
#   $(1) = parameter filename/value
#   $(2) = cert_create command line option for the specified parameter
#   $(3) = FIP prefix (optional) (if FWU_, target is fwu_fip instead of fip)
define CERT_ADD_CMD_OPT
    $(3)CRT_ARGS += $(2) $(1)
endef
```

**参数说明**：
- **`$(1)`**：参数值（文件名或值）
- **`$(2)`**：`cert_create` 的命令行选项（如 `--rot-key`, `--key-alg`）
- **`$(3)`**：可选的前缀（`FWU_` 或 `BL2_`），用于区分不同的证书生成目标

---

## 工作原理

### 1. 宏展开

当调用 `$(eval $(call CERT_ADD_CMD_OPT,${ROT_KEY},--rot-key))` 时：

```makefile
# 展开后相当于：
CRT_ARGS += --rot-key ${ROT_KEY}
```

### 2. 变量累积

多次调用会累积到 `CRT_ARGS` 变量中：

```makefile
# 第一次调用
$(eval $(call CERT_ADD_CMD_OPT,${ROT_KEY},--rot-key))
# CRT_ARGS = --rot-key build/fvp/rot_key.pem

# 第二次调用
$(eval $(call CERT_ADD_CMD_OPT,${TRUSTED_WORLD_KEY},--trusted-world-key))
# CRT_ARGS = --rot-key build/fvp/rot_key.pem --trusted-world-key build/fvp/trusted_world_key.pem

# 第三次调用
$(eval $(call CERT_ADD_CMD_OPT,${KEY_ALG},--key-alg))
# CRT_ARGS = --rot-key build/fvp/rot_key.pem --trusted-world-key build/fvp/trusted_world_key.pem --key-alg rsa
```

### 3. 最终使用

**Makefile 第 1156-1157 行**：

```makefile
certificates: ${CRT_DEPS} ${CRTTOOL} ${DTBS}
	$(q)${CRTTOOL} ${CRT_ARGS}
```

**展开后**：

```bash
cert_create --rot-key build/fvp/rot_key.pem \
            --trusted-world-key build/fvp/trusted_world_key.pem \
            --key-alg rsa \
            --key-size 2048 \
            --hash-alg sha256 \
            --trusted-key-cert build/fvp/trusted_key.crt \
            --soc-fw-key-cert build/fvp/soc_fw_key.crt \
            ...
```

---

## 使用示例

### 示例 1：添加 NV Counter

**`tbbr_tools.mk` 第 49 行**：

```makefile
$(eval $(call CERT_ADD_CMD_OPT,${TFW_NVCTR_VAL},--tfw-nvctr))
```

**效果**：
- 如果 `TFW_NVCTR_VAL=0`，则 `CRT_ARGS += --tfw-nvctr 0`
- 最终 `cert_create` 命令包含 `--tfw-nvctr 0`

### 示例 2：添加密钥文件

**`tbbr_tools.mk` 第 82 行**：

```makefile
$(if ${ROT_KEY},$(eval $(call CERT_ADD_CMD_OPT,${ROT_KEY},--rot-key)))
```

**效果**：
- 如果 `ROT_KEY=build/fvp/rot_key.pem`，则 `CRT_ARGS += --rot-key build/fvp/rot_key.pem`
- 最终 `cert_create` 命令包含 `--rot-key build/fvp/rot_key.pem`

**注意**：使用 `$(if ${ROT_KEY},...)` 确保只有当 `ROT_KEY` 变量有值时才添加参数。

### 示例 3：添加算法参数

**`tbbr_tools.mk` 第 73 行**：

```makefile
$(if ${KEY_ALG},$(eval $(call CERT_ADD_CMD_OPT,${KEY_ALG},--key-alg)))
```

**效果**：
- 如果 `KEY_ALG=rsa`，则 `CRT_ARGS += --key-alg rsa`
- 最终 `cert_create` 命令包含 `--key-alg rsa`

### 示例 4：使用前缀（FWU_）

**`tbbr_tools.mk` 第 74 行**：

```makefile
$(if ${KEY_ALG},$(eval $(call CERT_ADD_CMD_OPT,${KEY_ALG},--key-alg,FWU_)))
```

**效果**：
- 添加到 `FWU_CRT_ARGS` 而不是 `CRT_ARGS`
- 用于生成 FWU（Firmware Update）证书
- 最终 `cert_create` 命令（用于 FWU）包含 `--key-alg rsa`

### 示例 5：使用前缀（BL2_）

**`tbbr_tools.mk` 第 50 行**：

```makefile
$(eval $(call CERT_ADD_CMD_OPT,${TFW_NVCTR_VAL},--tfw-nvctr, BL2_))
```

**效果**：
- 添加到 `BL2_CRT_ARGS` 而不是 `CRT_ARGS`
- 用于生成 BL2 证书（当 `SEPARATE_BL2_FIP=1` 时）

---

## 三种 CRT_ARGS 变量

| 变量名 | 用途 | 使用位置 |
|--------|------|---------|
| `CRT_ARGS` | 标准证书生成参数 | `certificates` 目标 |
| `FWU_CRT_ARGS` | FWU 证书生成参数 | `fwu_certificates` 目标 |
| `BL2_CRT_ARGS` | BL2 证书生成参数 | `bl2_certificates` 目标 |

### 使用位置

**Makefile 第 1156-1157 行**（标准证书）：
```makefile
certificates: ${CRT_DEPS} ${CRTTOOL} ${DTBS}
	$(q)${CRTTOOL} ${CRT_ARGS}
```

**Makefile 第 1183-1184 行**（FWU 证书）：
```makefile
fwu_certificates: ${FWU_CRT_DEPS} ${CRTTOOL}
	$(q)${CRTTOOL} ${FWU_CRT_ARGS}
```

**Makefile 第 1192-1193 行**（BL2 证书）：
```makefile
bl2_certificates: ${BUILD_PLAT}/${FIP_NAME} ${BL2_CRT_DEPS} ${CRTTOOL}
	$(q)${CRTTOOL} ${BL2_CRT_ARGS}
```

---

## 完整流程示例

### 步骤 1：在 `tbbr_tools.mk` 中累积参数

```makefile
# 添加 NV Counter
$(eval $(call CERT_ADD_CMD_OPT,${TFW_NVCTR_VAL},--tfw-nvctr))
# CRT_ARGS = --tfw-nvctr 0

# 添加密钥算法
$(if ${KEY_ALG},$(eval $(call CERT_ADD_CMD_OPT,${KEY_ALG},--key-alg)))
# CRT_ARGS = --tfw-nvctr 0 --key-alg rsa

# 添加密钥大小
$(if ${KEY_SIZE},$(eval $(call CERT_ADD_CMD_OPT,${KEY_SIZE},--key-size)))
# CRT_ARGS = --tfw-nvctr 0 --key-alg rsa --key-size 2048

# 添加 ROT 密钥
$(if ${ROT_KEY},$(eval $(call CERT_ADD_CMD_OPT,${ROT_KEY},--rot-key)))
# CRT_ARGS = --tfw-nvctr 0 --key-alg rsa --key-size 2048 --rot-key build/fvp/rot_key.pem

# 添加 BL31 密钥
$(if ${BL31_KEY},$(eval $(call CERT_ADD_CMD_OPT,${BL31_KEY},--soc-fw-key)))
# CRT_ARGS = ... --soc-fw-key build/fvp/soc_fw_content_key.pem
```

### 步骤 2：`TOOL_ADD_PAYLOAD` 添加证书和镜像

```makefile
# 添加证书输出文件
$(eval $(call TOOL_ADD_PAYLOAD,${BUILD_PLAT}/soc_fw_key.crt,--soc-fw-key-cert))
# CRT_ARGS += --soc-fw-key-cert build/fvp/soc_fw_key.crt

# 添加镜像文件（用于计算 hash）
$(if ${BL31}, $(eval $(call TOOL_ADD_IMG,bl31,--soc-fw)))
# CRT_ARGS += --soc-fw build/fvp/bl31.bin
```

**注意**：`TOOL_ADD_PAYLOAD` 也会向 `CRT_ARGS` 添加参数，但它同时还会：
- 向 `FIP_ARGS` 添加参数（用于 `fiptool`）
- 添加依赖关系（`CRT_DEPS`）

### 步骤 3：执行 `cert_create`

**Makefile 第 1157 行**：

```bash
cert_create ${CRT_ARGS}
```

**实际执行的命令**：

```bash
cert_create \
    --tfw-nvctr 0 \
    --key-alg rsa \
    --key-size 2048 \
    --hash-alg sha256 \
    --rot-key build/fvp/rot_key.pem \
    --trusted-world-key build/fvp/trusted_world_key.pem \
    --soc-fw-key build/fvp/soc_fw_content_key.pem \
    --trusted-key-cert build/fvp/trusted_key.crt \
    --soc-fw-key-cert build/fvp/soc_fw_key.crt \
    --soc-fw-cert build/fvp/soc_fw_content.crt \
    --soc-fw build/fvp/bl31.bin \
    ...
```

---

## 与其他宏的区别

### `CERT_ADD_CMD_OPT` vs `TOOL_ADD_PAYLOAD`

| 宏 | 用途 | 添加到 | 同时添加依赖 |
|----|------|--------|------------|
| `CERT_ADD_CMD_OPT` | 添加 `cert_create` 的命令行参数 | `CRT_ARGS` | ❌ |
| `TOOL_ADD_PAYLOAD` | 添加证书/镜像文件（用于 `cert_create` 和 `fiptool`） | `CRT_ARGS` + `FIP_ARGS` | ✅ |

**`TOOL_ADD_PAYLOAD` 定义**（`build_macros.mk` 第 167-177 行）：

```makefile
define TOOL_ADD_PAYLOAD
ifneq ($(5),)
    $(4)FIP_ARGS += $(2) $(5)
    $(if $(3),$(4)CRT_DEPS += $(1))
else
    $(4)FIP_ARGS += $(2) $(1)
    $(if $(3),$(4)CRT_DEPS += $(3))
endif
    $(if $(3),$(4)FIP_DEPS += $(3))
    $(4)CRT_ARGS += $(2) $(1)  # ← 也会添加到 CRT_ARGS
endef
```

**使用场景**：
- **`CERT_ADD_CMD_OPT`**：用于添加**参数值**（密钥文件路径、算法名称、NV Counter 值等）
- **`TOOL_ADD_PAYLOAD`**：用于添加**证书输出文件**和**镜像文件**（需要依赖关系）

---

## 实际使用示例

### 示例：完整的 BL31 证书生成流程

**`tbbr_tools.mk` 第 114-121 行**：

```makefile
# Add the BL31 CoT (key cert + img cert)
$(if ${BL31_KEY},$(eval $(call CERT_ADD_CMD_OPT,${BL31_KEY},--soc-fw-key)))
ifneq (${COT},cca)
$(eval $(call TOOL_ADD_PAYLOAD,${BUILD_PLAT}/soc_fw_content.crt,--soc-fw-cert))
$(eval $(call TOOL_ADD_PAYLOAD,${BUILD_PLAT}/soc_fw_key.crt,--soc-fw-key-cert))
endif
$(if ${BL31}, $(eval $(call TOOL_ADD_IMG,bl31,--soc-fw)))
```

**展开后的效果**：

```makefile
# 1. 添加密钥文件参数
CRT_ARGS += --soc-fw-key build/fvp/soc_fw_content_key.pem

# 2. 添加证书输出文件（TOOL_ADD_PAYLOAD）
CRT_ARGS += --soc-fw-cert build/fvp/soc_fw_content.crt
CRT_ARGS += --soc-fw-key-cert build/fvp/soc_fw_key.crt
CRT_DEPS += build/fvp/soc_fw_content.crt build/fvp/soc_fw_key.crt

# 3. 添加镜像文件（TOOL_ADD_IMG → TOOL_ADD_PAYLOAD）
CRT_ARGS += --soc-fw build/fvp/bl31.bin
CRT_DEPS += build/fvp/bl31.bin
FIP_ARGS += --soc-fw build/fvp/bl31.bin
```

---

## 总结

### `CERT_ADD_CMD_OPT` 的作用

1. **向 `CRT_ARGS` 添加参数**：累积 `cert_create` 工具的命令行参数
2. **支持前缀**：可以添加到 `FWU_CRT_ARGS` 或 `BL2_CRT_ARGS`
3. **条件添加**：通常配合 `$(if ${VAR},...)` 使用，只在变量有值时添加

### 典型用法

```makefile
# 添加参数值
$(eval $(call CERT_ADD_CMD_OPT,${VALUE},--option))

# 条件添加
$(if ${VAR},$(eval $(call CERT_ADD_CMD_OPT,${VAR},--option)))

# 添加到 FWU 证书参数
$(eval $(call CERT_ADD_CMD_OPT,${VALUE},--option,FWU_))
```

### 最终效果

所有通过 `CERT_ADD_CMD_OPT` 添加的参数都会累积到 `CRT_ARGS` 变量中，最终在 `certificates` 目标执行时传递给 `cert_create` 工具：

```bash
cert_create ${CRT_ARGS}
```

---

**参考代码位置**：
- 宏定义：`make_helpers/build_macros.mk` 第 207-213 行
- 使用示例：`make_helpers/tbbr/tbbr_tools.mk` 第 49-90 行
- 最终使用：`Makefile` 第 1156-1157 行
