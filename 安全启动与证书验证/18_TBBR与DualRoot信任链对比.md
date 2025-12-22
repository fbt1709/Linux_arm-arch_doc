# TBBR vs Dual Root Chain of Trust 对比

## 概述

TF-A 支持三种 Chain of Trust (CoT) 架构：
1. **TBBR** (Trusted Board Boot Root) - 单根信任链
2. **Dual Root** - 双根信任链
3. **CCA** (Confidential Compute Architecture) - 机密计算架构

本文档重点对比 **TBBR** 和 **Dual Root** 两种架构的差异。

---

## 文件位置

| CoT 类型 | 证书定义文件 | 扩展定义文件 | 密钥定义文件 |
|---------|------------|------------|------------|
| **TBBR** | `tools/cert_create/src/tbbr/tbb_cert.c` | `tools/cert_create/src/tbbr/tbb_ext.c` | `tools/cert_create/src/tbbr/tbb_key.c` |
| **Dual Root** | `tools/cert_create/src/dualroot/cot.c` | `tools/cert_create/src/dualroot/cot.c` | `tools/cert_create/src/dualroot/cot.c` |

**注意**：Dual Root 将所有定义（证书、扩展、密钥）都放在同一个文件 `cot.c` 中。

---

## 选择机制

**Makefile 选择逻辑**（`tools/cert_create/Makefile` 第 30-38 行）：

```makefile
ifeq (${COT},tbbr)
  include src/tbbr/tbbr.mk
else ifeq (${COT},dualroot)
  include src/dualroot/cot.mk
else ifeq (${COT},cca)
  include src/cca/cot.mk
else
  $(error Unknown chain of trust ${COT})
endif
```

**编译时指定**：
```bash
# 使用 TBBR CoT（默认）
make PLAT=fvp all fip

# 使用 Dual Root CoT
make PLAT=fvp COT=dualroot all fip
```

---

## 核心差异对比

### 1. 信任根（Root of Trust）数量

| CoT 类型 | 信任根数量 | 信任根名称 |
|---------|----------|-----------|
| **TBBR** | **单根** | `ROT_KEY` (Root of Trust Key) |
| **Dual Root** | **双根** | `ROT_KEY` + `PROT_KEY` (Platform Root of Trust Key) |

### 2. Trusted Key Certificate 的差异

#### TBBR (`tbb_cert.c` 第 36-50 行)
```c
[TRUSTED_KEY_CERT] = {
    .key = ROT_KEY,
    .ext = {
        TRUSTED_FW_NVCOUNTER_EXT,
        TRUSTED_WORLD_PK_EXT,        // ← Trusted World 公钥
        NON_TRUSTED_WORLD_PK_EXT     // ← Non-Trusted World 公钥（存在）
    }
}
```

#### Dual Root (`cot.c` 第 39-51 行)
```c
[TRUSTED_KEY_CERT] = {
    .key = ROT_KEY,
    .ext = {
        TRUSTED_FW_NVCOUNTER_EXT,
        TRUSTED_WORLD_PK_EXT,        // ← Trusted World 公钥
        // ← 没有 NON_TRUSTED_WORLD_PK_EXT
    }
}
```

**差异**：
- ✅ **TBBR**：Trusted Key Certificate 包含 **两个公钥**（Trusted World 和 Non-Trusted World）
- ✅ **Dual Root**：Trusted Key Certificate 只包含 **一个公钥**（Trusted World）

### 3. Non-Trusted Firmware 证书的签名密钥

#### TBBR (`tbb_cert.c` 第 139-152 行)
```c
[NON_TRUSTED_FW_KEY_CERT] = {
    .key = NON_TRUSTED_WORLD_KEY,    // ← 使用 NON_TRUSTED_WORLD_KEY
    .issuer = NON_TRUSTED_FW_KEY_CERT,
    .ext = {
        NON_TRUSTED_FW_NVCOUNTER_EXT,
        NON_TRUSTED_FW_CONTENT_CERT_PK_EXT
    }
}

[NON_TRUSTED_FW_CONTENT_CERT] = {
    .key = NON_TRUSTED_FW_CONTENT_CERT_KEY,
    .issuer = NON_TRUSTED_FW_CONTENT_CERT,
    ...
}
```

#### Dual Root (`cot.c` 第 193-207 行)
```c
// ← 没有 NON_TRUSTED_FW_KEY_CERT

[NON_TRUSTED_FW_CONTENT_CERT] = {
    .key = PROT_KEY,                 // ← 使用 PROT_KEY（Platform Root）
    .issuer = NON_TRUSTED_FW_CONTENT_CERT,
    .ext = {
        NON_TRUSTED_FW_NVCOUNTER_EXT,
        NON_TRUSTED_WORLD_BOOTLOADER_HASH_EXT,
        NON_TRUSTED_FW_CONFIG_HASH_EXT,
        PROT_PK_EXT,                 // ← 包含 Platform Root 公钥
    }
}
```

**差异**：
- ✅ **TBBR**：Non-Trusted Firmware 使用 **两层证书结构**（Key Cert + Content Cert），由 `NON_TRUSTED_WORLD_KEY` 签名
- ✅ **Dual Root**：Non-Trusted Firmware 使用 **单层证书结构**（只有 Content Cert），由 `PROT_KEY` 签名

### 4. Platform Secure Partition 证书

#### TBBR
```c
// ← 没有 PLAT_SECURE_PARTITION_CONTENT_CERT
```

#### Dual Root (`cot.c` 第 159-176 行)
```c
[PLAT_SECURE_PARTITION_CONTENT_CERT] = {
    .id = PLAT_SECURE_PARTITION_CONTENT_CERT,
    .opt = "plat-sp-cert",
    .key = PROT_KEY,                 // ← 使用 PROT_KEY
    .issuer = PLAT_SECURE_PARTITION_CONTENT_CERT,
    .ext = {
        NON_TRUSTED_FW_NVCOUNTER_EXT,
        SP_PKG5_HASH_EXT,
        SP_PKG6_HASH_EXT,
        SP_PKG7_HASH_EXT,
        SP_PKG8_HASH_EXT,
        PROT_PK_EXT,                 // ← 包含 Platform Root 公钥
    }
}
```

**差异**：
- ✅ **TBBR**：没有 Platform Secure Partition 证书
- ✅ **Dual Root**：有 `PLAT_SECURE_PARTITION_CONTENT_CERT`，用于验证平台拥有的 Secure Partition

### 5. 密钥定义差异

#### TBBR (`tbb_key.c`)
```c
[ROT_KEY] = { ... }
[TRUSTED_WORLD_KEY] = { ... }
[NON_TRUSTED_WORLD_KEY] = { ... }   // ← 存在
[SCP_FW_CONTENT_CERT_KEY] = { ... }
[SOC_FW_CONTENT_CERT_KEY] = { ... }
[TRUSTED_OS_FW_CONTENT_CERT_KEY] = { ... }
[NON_TRUSTED_FW_CONTENT_CERT_KEY] = { ... }
```

#### Dual Root (`cot.c` 第 539-581 行)
```c
[ROT_KEY] = { ... }
[TRUSTED_WORLD_KEY] = { ... }
// ← 没有 NON_TRUSTED_WORLD_KEY
[SCP_FW_CONTENT_CERT_KEY] = { ... }
[SOC_FW_CONTENT_CERT_KEY] = { ... }
[TRUSTED_OS_FW_CONTENT_CERT_KEY] = { ... }
[PROT_KEY] = { ... }                // ← 存在 PROT_KEY
```

**差异**：
- ✅ **TBBR**：使用 `NON_TRUSTED_WORLD_KEY`
- ✅ **Dual Root**：使用 `PROT_KEY`（Platform Root of Trust Key）

---

## 证书结构对比

### TBBR 证书链

```
ROT_KEY (Root of Trust)
    ↓
TRUSTED_KEY_CERT
    ├── TRUSTED_WORLD_PK_EXT
    └── NON_TRUSTED_WORLD_PK_EXT
        ↓
    TRUSTED_WORLD_KEY              NON_TRUSTED_WORLD_KEY
        ↓                              ↓
    SOC_FW_KEY_CERT              NON_TRUSTED_FW_KEY_CERT
        ↓                              ↓
    SOC_FW_CONTENT_CERT          NON_TRUSTED_FW_CONTENT_CERT
        ↓                              ↓
    BL31                          BL33
```

### Dual Root 证书链

```
ROT_KEY (Root of Trust)           PROT_KEY (Platform Root)
    ↓                                  ↓
TRUSTED_KEY_CERT                  (独立根)
    ├── TRUSTED_WORLD_PK_EXT
    └── (没有 NON_TRUSTED_WORLD_PK_EXT)
        ↓
    TRUSTED_WORLD_KEY              PROT_KEY
        ↓                              ↓
    SOC_FW_KEY_CERT              NON_TRUSTED_FW_CONTENT_CERT
        ↓                              ↓
    SOC_FW_CONTENT_CERT          BL33
        ↓                              ↓
    BL31                          PLAT_SECURE_PARTITION_CONTENT_CERT
                                      ↓
                                  SP_PKG5-8
```

---

## 使用场景

### TBBR（单根信任链）

**适用场景**：
- ✅ 标准 TBBR 架构
- ✅ 单一信任根管理所有固件
- ✅ 需要 Key Certificate 和 Content Certificate 分离

**特点**：
- 所有固件（Trusted 和 Non-Trusted）都由同一个 ROT_KEY 派生
- Non-Trusted World 使用独立的密钥体系（NON_TRUSTED_WORLD_KEY）

### Dual Root（双根信任链）

**适用场景**：
- ✅ 需要平台独立的信任根
- ✅ 平台厂商需要独立管理 Non-Trusted Firmware
- ✅ 需要 Platform Secure Partition 支持
- ✅ 符合某些安全标准要求（如某些行业标准）

**特点**：
- **两个独立的信任根**：
  - `ROT_KEY`：管理 Trusted World 固件（BL31, BL32, SCP_BL2）
  - `PROT_KEY`：管理 Non-Trusted World 固件（BL33）和 Platform Secure Partition
- Non-Trusted Firmware 直接由 `PROT_KEY` 签名，不需要 Key Certificate
- 平台厂商可以独立管理 `PROT_KEY`，不影响 `ROT_KEY`

---

## 证书数量对比

| 证书类型 | TBBR | Dual Root |
|---------|------|-----------|
| TRUSTED_BOOT_FW_CERT | ✅ | ✅ |
| TRUSTED_KEY_CERT | ✅ | ✅ |
| SCP_FW_KEY_CERT | ✅ | ✅ |
| SCP_FW_CONTENT_CERT | ✅ | ✅ |
| SOC_FW_KEY_CERT | ✅ | ✅ |
| SOC_FW_CONTENT_CERT | ✅ | ✅ |
| TRUSTED_OS_FW_KEY_CERT | ✅ | ✅ |
| TRUSTED_OS_FW_CONTENT_CERT | ✅ | ✅ |
| **NON_TRUSTED_FW_KEY_CERT** | ✅ | ❌ |
| NON_TRUSTED_FW_CONTENT_CERT | ✅ | ✅ |
| SIP_SECURE_PARTITION_CONTENT_CERT | ✅ | ✅ |
| **PLAT_SECURE_PARTITION_CONTENT_CERT** | ❌ | ✅ |
| FWU_CERT | ✅ | ✅ |

**总计**：
- **TBBR**：11 个证书
- **Dual Root**：11 个证书（但结构不同）

---

## 扩展字段差异

### TBBR 特有扩展
- `NON_TRUSTED_WORLD_PK_EXT`：在 Trusted Key Certificate 中
- `NON_TRUSTED_FW_CONTENT_CERT_PK_EXT`：在 NON_TRUSTED_FW_KEY_CERT 中

### Dual Root 特有扩展
- `PROT_PK_EXT`：Platform Root of Trust 公钥扩展
  - 在 `PLAT_SECURE_PARTITION_CONTENT_CERT` 中
  - 在 `NON_TRUSTED_FW_CONTENT_CERT` 中

---

## 代码位置总结

### TBBR
- **证书定义**：`tools/cert_create/src/tbbr/tbb_cert.c`
- **扩展定义**：`tools/cert_create/src/tbbr/tbb_ext.c`
- **密钥定义**：`tools/cert_create/src/tbbr/tbb_key.c`
- **Makefile**：`tools/cert_create/src/tbbr/tbbr.mk`

### Dual Root
- **所有定义**：`tools/cert_create/src/dualroot/cot.c`
- **Makefile**：`tools/cert_create/src/dualroot/cot.mk`

---

## 总结

| 特性 | TBBR | Dual Root |
|------|------|-----------|
| **信任根数量** | 1 (ROT_KEY) | 2 (ROT_KEY + PROT_KEY) |
| **Trusted Key Cert 公钥** | 2 个 | 1 个 |
| **Non-Trusted FW 证书结构** | Key Cert + Content Cert | 只有 Content Cert |
| **Non-Trusted FW 签名密钥** | NON_TRUSTED_WORLD_KEY | PROT_KEY |
| **Platform SP 证书** | ❌ | ✅ |
| **适用场景** | 标准 TBBR | 平台独立管理 |

**选择建议**：
- 如果使用标准 TBBR 架构，使用 `COT=tbbr`（默认）
- 如果需要平台独立的信任根管理，使用 `COT=dualroot`
