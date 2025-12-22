# ROT_KEY 的作用和启动过程中的角色

## 概述

**ROT_KEY**（Root of Trust Key，信任根密钥）是整个 Chain of Trust 的**密码学锚点**，是安全启动的基础。它有两个关键角色：

1. **证书生成时**：用于签名根证书（Root Certificates）
2. **启动验证时**：ROTPK（ROT_KEY 的公钥）用于验证根证书的签名

---

## ROT_KEY 的定义

**位置**：`tools/cert_create/src/tbbr/tbb_key.c` 第 15-20 行

```c
[ROT_KEY] = {
    .id = ROT_KEY,
    .opt = "rot-key",
    .help_msg = "Root Of Trust key file or PKCS11 URI",
    .desc = "Root Of Trust key"
}
```

**命令行参数**：`--rot-key rot_key.pem`

---

## ROT_KEY 的两个角色

### 角色 1：证书生成时 - 签名根证书

#### 1.1 使用 ROT_KEY 签名的证书

**`tbb_cert.c` 中定义的根证书**：

```c
// 1. TRUSTED_BOOT_FW_CERT（BL2 证书）
[TRUSTED_BOOT_FW_CERT] = {
    .key = ROT_KEY,              // ← 使用 ROT_KEY
    .issuer = TRUSTED_BOOT_FW_CERT,  // ← 自签名
    ...
}

// 2. TRUSTED_KEY_CERT（信任密钥证书）
[TRUSTED_KEY_CERT] = {
    .key = ROT_KEY,              // ← 使用 ROT_KEY
    .issuer = TRUSTED_KEY_CERT,  // ← 自签名
    ...
}

// 3. FWU_CERT（固件更新证书）
[FWU_CERT] = {
    .key = ROT_KEY,              // ← 使用 ROT_KEY
    .issuer = FWU_CERT,          // ← 自签名
    ...
}
```

#### 1.2 签名过程（`cert.c` 第 95-176 行）

```c
int cert_new(...)
{
    // 1. 获取证书的密钥（用于 Subject Public Key）
    EVP_PKEY *pkey = keys[cert->key].key;  // ← keys[ROT_KEY].key
    
    // 2. 获取签发者的密钥（用于签名）
    cert_t *issuer_cert = &certs[cert->issuer];
    EVP_PKEY *ikey = keys[issuer_cert->key].key;  // ← 对于自签名，也是 keys[ROT_KEY].key
    
    // 3. 设置证书的 Subject Public Key
    X509_set_pubkey(x, pkey);  // ← ROT_KEY 的公钥成为证书的 Subject PK
    
    // 4. 使用 ROT_KEY 的私钥签名证书
    if (!EVP_DigestSignInit(mdCtx, &pKeyCtx, get_digest(md_alg), NULL, ikey)) {
        // ← 使用 ROT_KEY 的私钥签名
    }
    
    if (!X509_sign_ctx(x, mdCtx)) {  // ← 签名证书
        // ...
    }
}
```

**结果**：
- 证书的 **Subject Public Key** = ROT_KEY 的公钥
- 证书的 **签名** = 使用 ROT_KEY 的私钥签名

---

### 角色 2：启动验证时 - 验证根证书

#### 2.1 根证书的验证流程

**`auth_mod.c` 第 215-292 行**：

```c
static int auth_signature(...)
{
    // 判断是否为根证书（没有父证书）
    if (img_desc->parent != NULL) {
        // 非根证书：从父证书获取公钥
        rc = auth_get_param(param->pk, img_desc->parent, &pk_ptr, &pk_len);
    } else {
        // ← 根证书：从平台获取 ROTPK
        rc = plat_get_rotpk_info(param->pk->cookie, &pk_plat_ptr,
                                 &pk_plat_len, &flags);
        
        // 从证书中提取 Subject Public Key
        rc = img_parser_get_auth_param(img_desc->img_type,
                                       param->pk, img, img_len,
                                       &pk_ptr, &pk_len);
        
        // 验证证书的 Subject PK 是否与平台 ROTPK 匹配
        if ((flags & ROTPK_IS_HASH) != 0U) {
            // 比较 hash
            rc = crypto_mod_verify_hash(cnv_pk_ptr, cnv_pk_len,
                                        pk_plat_ptr, pk_plat_len);
        } else {
            // 直接比较
            if (memcmp(pk_plat_ptr, pk_ptr, pk_len) != 0) {
                return -1;  // ← 不匹配，验证失败
            }
        }
    }
    
    // 使用公钥验证签名
    rc = crypto_mod_verify_signature(data_ptr, data_len,
                                     sig_ptr, sig_len,
                                     sig_alg_ptr, sig_alg_len,
                                     pk_ptr, pk_len);
}
```

#### 2.2 平台 ROTPK 的获取

**平台函数**：`plat_get_rotpk_info()` （平台特定实现）

**存储方式**（`auth_mod.c` 第 243-246 行注释）：

1. **ROTPK Hash**：存储 ROTPK 的 hash 值
2. **Modified ROTPK Hash**：存储修改后的 ROTPK 的 hash（前缀/后缀）
3. **Full ROTPK**：存储完整的 ROTPK

**示例：SynQuacer 平台**（`plat/socionext/synquacer/sq_rotpk.S`）：

```assembly
sq_rotpk_hash:
    /* DER header */
    .byte 0x30, 0x31, 0x30, ...
    /* SHA256 */
    .incbin ROTPK_HASH  // ← 嵌入 ROTPK hash
sq_rotpk_hash_end:
```

**C 代码访问**（`plat/rpi/common/rpi3_trusted_boot.c`）：

```c
extern char rpi3_rotpk_hash[], rpi3_rotpk_hash_end[];

int plat_get_rotpk_info(...) {
    *key_ptr = rpi3_rotpk_hash;  // ← 返回嵌入的 hash
    *key_len = rpi3_rotpk_hash_end - rpi3_rotpk_hash;
    *flags = ROTPK_IS_HASH;
    return 0;
}
```

---

## 启动过程中的作用

### 完整启动流程

```
1. BL1 启动
   ↓
2. BL1 加载并验证 BL2
   ├─ 加载 TRUSTED_BOOT_FW_CERT（BL2 的证书）
   ├─ 验证证书签名（使用平台 ROTPK）
   │   ├─ 提取证书的 Subject Public Key
   │   ├─ 与平台 ROTPK 比较（hash 或直接比较）
   │   └─ 使用 ROTPK 验证证书签名
   ├─ 提取 BL2 的 hash（从证书扩展）
   └─ 验证 BL2 镜像的 hash
   ↓
3. BL2 启动
   ↓
4. BL2 加载并验证后续固件
   ├─ 加载 TRUSTED_KEY_CERT（根证书）
   │   ├─ 验证证书签名（使用平台 ROTPK）
   │   └─ 提取 Trusted World PK 和 Non-Trusted World PK
   ├─ 加载 SOC_FW_KEY_CERT（BL31 的 Key Cert）
   │   ├─ 验证签名（使用 Trusted World PK）
   │   └─ 提取 Content Cert PK
   ├─ 加载 SOC_FW_CONTENT_CERT（BL31 的 Content Cert）
   │   ├─ 验证签名（使用 Content Cert PK）
   │   └─ 提取 BL31 的 hash
   └─ 验证 BL31 镜像的 hash
   ↓
5. BL31 启动
   ↓
6. 继续验证 BL32、BL33...
```

### 关键验证点

#### 验证点 1：BL1 验证 BL2 证书

**证书**：`TRUSTED_BOOT_FW_CERT`

**验证过程**（`tbbr_cot_bl1.c` 第 59-85 行）：

```c
static const auth_img_desc_t trusted_boot_fw_cert = {
    .img_id = TRUSTED_BOOT_FW_CERT_ID,
    .parent = NULL,  // ← 根证书，没有父证书
    .img_auth_methods = {
        [0] = {
            .type = AUTH_METHOD_SIG,
            .param.sig = {
                .pk = &subject_pk,  // ← 使用 Subject PK（ROTPK）
                .sig = &sig,
                .alg = &sig_alg,
                .data = &raw_data
            }
        }
    }
};
```

**验证步骤**：

1. **提取证书的 Subject Public Key**（ROTPK）
2. **与平台 ROTPK 比较**：
   ```c
   if (ROTPK_IS_HASH) {
       hash(cert_subject_pk) == platform_rotpk_hash
   } else {
       cert_subject_pk == platform_rotpk
   }
   ```
3. **验证证书签名**：
   ```c
   verify_signature(TBSCertificate, signature, ROTPK)
   ```

#### 验证点 2：BL2 验证 Trusted Key Certificate

**证书**：`TRUSTED_KEY_CERT`

**验证过程**（`tbbr_cot_bl2.c` 第 60-90 行）：

```c
static const auth_img_desc_t trusted_key_cert = {
    .img_id = TRUSTED_KEY_CERT_ID,
    .parent = NULL,  // ← 根证书
    .img_auth_methods = {
        [0] = {
            .type = AUTH_METHOD_SIG,
            .param.sig = {
                .pk = &subject_pk,  // ← 使用 Subject PK（ROTPK）
                ...
            }
        }
    },
    .authenticated_data = {
        [0] = { .type_desc = &trusted_world_pk, ... },  // ← 提取 Trusted World PK
        [1] = { .type_desc = &non_trusted_world_pk, ... }  // ← 提取 Non-Trusted World PK
    }
};
```

**验证步骤**：

1. **验证证书签名**（使用平台 ROTPK）
2. **提取 Trusted World PK** 和 **Non-Trusted World PK**（存储到缓冲区）
3. **后续证书使用这些公钥验证**

---

## ROT_KEY 的关键特性

### 1. 信任链的起点

```
ROT_KEY (信任根)
    ↓ 签名
TRUSTED_BOOT_FW_CERT / TRUSTED_KEY_CERT / FWU_CERT（根证书）
    ↓ 验证（使用 ROTPK）
后续证书链
    ↓
固件镜像
```

### 2. 平台级别的安全锚点

- **ROTPK 必须安全存储**：通常嵌入到固件中或存储在硬件中
- **ROTPK 不可更改**：一旦部署，很难修改（除非使用硬件机制）
- **ROTPK 验证失败 = 启动失败**：如果 ROTPK 不匹配，整个信任链验证失败

### 3. 自签名证书

所有使用 ROT_KEY 的证书都是**自签名**的：

```c
[TRUSTED_BOOT_FW_CERT] = {
    .key = ROT_KEY,                    // ← 证书的密钥
    .issuer = TRUSTED_BOOT_FW_CERT,    // ← 签发者是自己
    ...
}
```

**含义**：
- 证书的 Subject Public Key = ROT_KEY 的公钥
- 证书的签名 = 使用 ROT_KEY 的私钥签名
- 验证时使用 ROTPK（ROT_KEY 的公钥）验证签名

---

## ROT_KEY 在证书生成中的使用

### 示例：生成 TRUSTED_BOOT_FW_CERT

**命令行**：

```bash
cert_create \
    --rot-key rot_key.pem \              # ← ROT_KEY 私钥文件
    --tb-fw-cert tb_fw.crt \             # ← 输出证书
    --tb-fw bl2.bin \                    # ← BL2 镜像（用于计算 hash）
    --tfw-nvctr 0 \
    ...
```

**生成过程**（`cert.c`）：

```c
1. 加载 ROT_KEY
   keys[ROT_KEY].key = EVP_PKEY* (从 rot_key.pem 加载)

2. 创建证书
   cert = &certs[TRUSTED_BOOT_FW_CERT]
   cert->key = ROT_KEY
   cert->issuer = TRUSTED_BOOT_FW_CERT  // 自签名

3. 设置 Subject Public Key
   X509_set_pubkey(x, keys[ROT_KEY].key)
   // ← 证书的 Subject PK = ROT_KEY 的公钥

4. 签名证书
   ikey = keys[TRUSTED_BOOT_FW_CERT].key  // = keys[ROT_KEY].key
   X509_sign_ctx(x, mdCtx)  // ← 使用 ROT_KEY 的私钥签名
```

**生成的证书**：
- Subject Public Key = ROT_KEY 的公钥
- Signature = 使用 ROT_KEY 的私钥签名
- 包含 BL2 的 hash（在扩展中）

---

## ROT_KEY 在启动验证中的使用

### 示例：BL1 验证 BL2 证书

**验证流程**（`auth_mod.c` + `mbedtls_x509_parser.c`）：

```c
1. 加载 TRUSTED_BOOT_FW_CERT 到内存
   ↓
2. 解析证书结构
   cert_parse(cert_ptr, cert_len)
   → 提取 tbs（TBSCertificate）
   → 提取 signature（签名值）
   → 提取 sig_alg（签名算法）
   → 提取 pk（Subject Public Key）
   ↓
3. 验证签名（auth_signature）
   ↓
   3.1 获取平台 ROTPK
       plat_get_rotpk_info(..., &pk_plat_ptr, &pk_plat_len, &flags)
       → pk_plat_ptr = 平台存储的 ROTPK（hash 或完整公钥）
   ↓
   3.2 提取证书的 Subject Public Key
       img_parser_get_auth_param(..., AUTH_PARAM_PUB_KEY, ...)
       → pk_ptr = 证书中的 Subject PK（ROTPK）
   ↓
   3.3 验证 Subject PK 是否与平台 ROTPK 匹配
       if (ROTPK_IS_HASH) {
           hash(cert_subject_pk) == platform_rotpk_hash
       } else {
           cert_subject_pk == platform_rotpk
       }
   ↓
   3.4 提取被签名的数据（TBSCertificate）
       img_parser_get_auth_param(..., AUTH_PARAM_RAW_DATA, ...)
       → data_ptr = tbs.p, data_len = tbs.len
   ↓
   3.5 提取签名值
       img_parser_get_auth_param(..., AUTH_PARAM_SIG, ...)
       → sig_ptr = signature.p, sig_len = signature.len
   ↓
   3.6 验证签名
       crypto_mod_verify_signature(data_ptr, data_len,
                                    sig_ptr, sig_len,
                                    sig_alg_ptr, sig_alg_len,
                                    pk_ptr, pk_len)
       → 使用 ROTPK 验证签名
   ↓
4. 验证成功 → 继续启动
   验证失败 → 启动失败
```

---

## ROT_KEY 的安全重要性

### 1. 信任链的基础

- ✅ **所有信任都基于 ROT_KEY**：如果 ROTPK 泄露或被替换，整个信任链失效
- ✅ **ROTPK 必须安全存储**：通常嵌入到固件或存储在硬件中
- ✅ **ROTPK 验证是第一个验证点**：如果失败，后续验证不会进行

### 2. 防篡改机制

```
攻击场景：攻击者修改了证书
    ↓
验证时：ROTPK 不匹配 → 验证失败 → 启动失败 ✅

攻击场景：攻击者替换了 ROTPK
    ↓
验证时：新的 ROTPK 无法验证原始证书 → 验证失败 → 启动失败 ✅
```

### 3. 平台绑定

- **ROTPK 与平台绑定**：每个平台有自己的 ROTPK
- **ROTPK 在制造时部署**：通常在生产时写入硬件或固件
- **ROTPK 难以更改**：一旦部署，除非使用特殊硬件机制，否则无法更改

---

## ROT_KEY 的使用位置总结

### 证书生成时（`cert_create`）

| 证书 | ROT_KEY 的作用 |
|------|---------------|
| `TRUSTED_BOOT_FW_CERT` | ✅ Subject Public Key<br>✅ 签名密钥 |
| `TRUSTED_KEY_CERT` | ✅ Subject Public Key<br>✅ 签名密钥 |
| `FWU_CERT` | ✅ Subject Public Key<br>✅ 签名密钥 |

### 启动验证时（TF-A 运行时）

| 验证阶段 | ROTPK 的作用 |
|---------|-------------|
| **BL1 验证 BL2** | ✅ 验证 `TRUSTED_BOOT_FW_CERT` 的签名<br>✅ 验证证书的 Subject PK 是否匹配平台 ROTPK |
| **BL2 验证根证书** | ✅ 验证 `TRUSTED_KEY_CERT` 的签名<br>✅ 验证证书的 Subject PK 是否匹配平台 ROTPK<br>✅ 验证 `FWU_CERT` 的签名 |

---

## 完整示例：BL1 验证 BL2

### 1. 证书生成（编译时）

```bash
cert_create \
    --rot-key rot_key.pem \          # ← ROT_KEY 私钥
    --tb-fw-cert tb_fw.crt \         # ← BL2 证书
    --tb-fw bl2.bin                  # ← BL2 镜像
```

**生成的证书**：
- Subject Public Key = ROT_KEY 的公钥
- Signature = ROT_KEY 的私钥签名（TBSCertificate）

### 2. 启动验证（运行时）

```
BL1 启动
    ↓
加载 TRUSTED_BOOT_FW_CERT
    ↓
验证流程：
    1. 提取证书的 Subject Public Key（ROTPK）
    2. 从平台获取 ROTPK（plat_get_rotpk_info）
    3. 比较：cert_subject_pk == platform_rotpk？
       → 如果匹配，继续
       → 如果不匹配，启动失败 ❌
    4. 提取 TBSCertificate（被签名的数据）
    5. 提取签名值
    6. 使用 ROTPK 验证签名
       verify_signature(TBSCertificate, signature, ROTPK)
       → 如果验证成功，继续
       → 如果验证失败，启动失败 ❌
    ↓
提取 BL2 的 hash（从证书扩展）
    ↓
验证 BL2 镜像的 hash
    ↓
启动 BL2
```

---

## 总结

### ROT_KEY 的核心作用

1. **证书生成时**：
   - ✅ 作为根证书的 Subject Public Key
   - ✅ 作为根证书的签名密钥（私钥）

2. **启动验证时**：
   - ✅ ROTPK（公钥）用于验证根证书的签名
   - ✅ ROTPK 与平台存储的 ROTPK 比较，确保证书来自可信源

### 安全意义

- 🔒 **信任链的起点**：所有信任都基于 ROT_KEY
- 🔒 **防篡改**：如果证书被修改，ROTPK 验证会失败
- 🔒 **平台绑定**：ROTPK 与平台绑定，防止跨平台攻击

### 关键代码位置

- **证书生成**：`tools/cert_create/src/cert.c` 第 102-104 行、138-139 行
- **启动验证**：`drivers/auth/auth_mod.c` 第 215-292 行
- **平台 ROTPK**：`plat/*/trusted_boot.c`（平台特定实现）

---

**参考代码位置**：
- ROT_KEY 定义：`tools/cert_create/src/tbbr/tbb_key.c` 第 15-20 行
- 证书签名：`tools/cert_create/src/cert.c` 第 102-104 行、138-139 行
- 签名验证：`drivers/auth/auth_mod.c` 第 215-292 行
- 平台 ROTPK：`plat/*/trusted_boot.c`（平台特定）
