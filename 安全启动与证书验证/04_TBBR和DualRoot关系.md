# TBBR 和 Dual Root 的关系

## 核心关系

**TBBR 和 Dual Root 是互斥的选择关系**，它们都是 TF-A 支持的 **Chain of Trust (CoT) 架构实现方式**，通过 `COT` 变量来选择使用哪一个。

---

## 关系图解

```
Chain of Trust (CoT) 架构
    │
    ├─ TBBR (Trusted Board Boot Root)
    │   └─ 单根信任链，默认架构
    │
    ├─ Dual Root (双根信任链)
    │   └─ TBBR 的变体，支持平台独立信任根
    │
    └─ CCA (Confidential Compute Architecture)
        └─ 机密计算架构

通过 COT 变量选择：
    COT=tbbr      → 使用 TBBR
    COT=dualroot  → 使用 Dual Root
    COT=cca       → 使用 CCA
```

---

## 选择机制

### 1. 编译时选择（`tools/cert_create/Makefile` 第 30-38 行）

```makefile
# Chain of trust.
ifeq (${COT},tbbr)
  include src/tbbr/tbbr.mk      # ← 使用 TBBR 定义
else ifeq (${COT},dualroot)
  include src/dualroot/cot.mk   # ← 使用 Dual Root 定义
else ifeq (${COT},cca)
  include src/cca/cot.mk        # ← 使用 CCA 定义
else
  $(error Unknown chain of trust ${COT})
endif
```

**关键点**：
- ✅ **互斥选择**：`ifeq...else ifeq` 结构确保只能选择一个
- ✅ **编译时决定**：在编译 `cert_create` 工具时决定使用哪个 CoT
- ✅ **默认是 TBBR**：`COT := tbbr`（`defaults.mk` 第 326 行）

### 2. 运行时影响（`tbbr_tools.mk`）

`tbbr_tools.mk` 会根据 `COT` 的值来决定包含哪些证书：

```makefile
# tbbr_tools.mk 第 52-63 行
ifeq (${COT},cca)
    $(eval $(call CERT_ADD_CMD_OPT,${CCAFW_NVCTR_VAL},--ccafw-nvctr))
endif

ifneq (${COT},cca)
    $(eval $(call TOOL_ADD_PAYLOAD,${TRUSTED_KEY_CERT},--trusted-key-cert))
else
    $(eval $(call TOOL_ADD_PAYLOAD,${BUILD_PLAT}/cca.crt,--cca-cert))
endif

# tbbr_tools.mk 第 138-142 行
ifneq (${COT},dualroot)
    ifneq (${COT},cca)
        $(eval $(call TOOL_ADD_PAYLOAD,${BUILD_PLAT}/nt_fw_key.crt,--nt-fw-key-cert))
    endif
endif
```

**关键点**：
- `tbbr_tools.mk` 同时支持 TBBR 和 Dual Root（通过 `ifneq (${COT},dualroot)` 判断）
- 但 `cert_create` 工具本身会根据 `COT` 值编译不同的 CoT 定义文件

---

## 它们的关系本质

### 1. **都是 Chain of Trust 的实现**

| 特性 | TBBR | Dual Root |
|------|------|-----------|
| **本质** | Chain of Trust 架构 | Chain of Trust 架构 |
| **关系** | 标准实现 | TBBR 的变体/扩展 |
| **默认** | ✅ 是（`COT=tbbr`） | ❌ 否 |

### 2. **互斥的选择关系**

```
编译时：
    COT=tbbr     → 编译 tbb_cert.c + tbb_ext.c + tbb_key.c
    COT=dualroot → 编译 dualroot/cot.c
    COT=cca      → 编译 cca/cot.c

运行时：
    只能使用编译时选择的 CoT 定义
```

### 3. **Dual Root 是 TBBR 的变体**

**相同点**：
- ✅ 都遵循 TBBR 文档的基本架构
- ✅ 都使用 Key Certificate + Content Certificate 两层结构
- ✅ 都使用相同的 Makefile 工具（`tbbr_tools.mk`）

**不同点**：
- ✅ **信任根数量**：TBBR 单根，Dual Root 双根
- ✅ **Non-Trusted FW 证书结构**：TBBR 两层，Dual Root 单层
- ✅ **Platform SP 支持**：TBBR 无，Dual Root 有

---

## 代码层面的关系

### 1. 定义文件位置

| CoT | 证书定义 | 扩展定义 | 密钥定义 |
|-----|---------|---------|---------|
| **TBBR** | `tools/cert_create/src/tbbr/tbb_cert.c` | `tools/cert_create/src/tbbr/tbb_ext.c` | `tools/cert_create/src/tbbr/tbb_key.c` |
| **Dual Root** | `tools/cert_create/src/dualroot/cot.c` | `tools/cert_create/src/dualroot/cot.c` | `tools/cert_create/src/dualroot/cot.c` |

**注意**：
- TBBR：三个独立文件
- Dual Root：一个文件包含所有定义

### 2. Makefile 包含关系

```
顶层 Makefile
    │
    ├─ include make_helpers/tbbr/tbbr_tools.mk  (总是包含，如果 INCLUDE_TBBR_MK=1)
    │   └─ 根据 COT 值决定包含哪些证书
    │
    └─ 编译 cert_create 工具
        └─ tools/cert_create/Makefile
            └─ 根据 COT 值包含不同的 CoT 定义
                ├─ COT=tbbr     → src/tbbr/tbbr.mk
                ├─ COT=dualroot → src/dualroot/cot.mk
                └─ COT=cca      → src/cca/cot.mk
```

---

## 使用场景对比

### TBBR（默认）

**适用场景**：
- ✅ 标准 TBBR 架构
- ✅ 单一信任根管理所有固件
- ✅ 大多数平台的标准配置

**编译命令**：
```bash
make PLAT=fvp all fip
# 或显式指定
make PLAT=fvp COT=tbbr all fip
```

### Dual Root

**适用场景**：
- ✅ 需要平台独立的信任根
- ✅ 平台厂商需要独立管理 Non-Trusted Firmware
- ✅ 需要 Platform Secure Partition 支持
- ✅ 符合某些安全标准要求

**编译命令**：
```bash
make PLAT=fvp COT=dualroot all fip
```

---

## 关键区别总结

### 1. 信任根结构

```
TBBR:
    ROT_KEY (单一根)
        ↓
    所有固件（Trusted + Non-Trusted）

Dual Root:
    ROT_KEY (SiP 根)          PROT_KEY (平台根)
        ↓                          ↓
    Trusted FW              Non-Trusted FW + Platform SP
```

### 2. Trusted Key Certificate

```
TBBR:
    TRUSTED_KEY_CERT
        ├── TRUSTED_WORLD_PK_EXT
        └── NON_TRUSTED_WORLD_PK_EXT  ← 存在

Dual Root:
    TRUSTED_KEY_CERT
        └── TRUSTED_WORLD_PK_EXT      ← 只有这个
```

### 3. Non-Trusted Firmware 证书

```
TBBR:
    NON_TRUSTED_FW_KEY_CERT (由 NON_TRUSTED_WORLD_KEY 签名)
        ↓
    NON_TRUSTED_FW_CONTENT_CERT

Dual Root:
    NON_TRUSTED_FW_CONTENT_CERT (由 PROT_KEY 签名，直接签名)
```

---

## 总结

### 关系本质

1. **互斥的选择关系**：
   - 通过 `COT` 变量选择使用哪一个
   - 编译时决定，运行时不可更改

2. **都是 CoT 实现**：
   - TBBR：标准实现（默认）
   - Dual Root：TBBR 的变体/扩展

3. **代码层面**：
   - TBBR：三个独立文件（`tbb_cert.c`, `tbb_ext.c`, `tbb_key.c`）
   - Dual Root：一个文件（`dualroot/cot.c`）

4. **Makefile 层面**：
   - 都使用 `tbbr_tools.mk`（如果 `INCLUDE_TBBR_MK=1`）
   - 但 `cert_create` 工具根据 `COT` 值编译不同的 CoT 定义

### 选择建议

- **默认使用 TBBR**：`make PLAT=fvp all fip`（`COT=tbbr` 是默认值）
- **需要平台独立信任根时使用 Dual Root**：`make PLAT=fvp COT=dualroot all fip`

---

**参考代码位置**：
- 选择机制：`tools/cert_create/Makefile` 第 30-38 行
- 默认值：`make_helpers/defaults.mk` 第 326 行
- TBBR 定义：`tools/cert_create/src/tbbr/`
- Dual Root 定义：`tools/cert_create/src/dualroot/cot.c`
