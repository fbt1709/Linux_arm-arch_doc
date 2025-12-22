# 如何将 Subject Public Key 添加到证书中

## 概述

`subject_pk`（Subject Public Key，证书主体公钥）是证书本身携带的公钥，用于验证证书的签名。**这个公钥是自动添加到证书中的，不需要手动操作**。

---

## Subject Public Key 的自动添加机制

### 1. 自动添加过程

当使用 `cert_create` 工具生成证书时，Subject Public Key 会**自动**添加到证书的 Subject Public Key Info 字段中。

**cert.c 第 176 行**：

```c
int cert_new(int md_alg, cert_t *cert, int days, int ca, STACK_OF(X509_EXTENSION) * sk)
{
    EVP_PKEY *pkey = keys[cert->key].key;  // ← 从证书定义中获取密钥
    
    // ... 创建证书结构 ...
    
    X509_set_pubkey(x, pkey);  // ← 自动设置 Subject Public Key
}
```

**关键点**：
- `X509_set_pubkey(x, pkey)` 会自动将公钥添加到证书的 Subject Public Key Info 字段
- 公钥来自 `keys[cert->key].key`，即证书定义中指定的密钥

### 2. 证书定义中的密钥指定

**tbb_cert.c 第 36-50 行** - Trusted Key Certificate：

```c
[TRUSTED_KEY_CERT] = {
    .id = TRUSTED_KEY_CERT,
    .key = ROT_KEY,              // ← 指定使用 ROT_KEY 作为 Subject Public Key
    .issuer = TRUSTED_KEY_CERT,  // ← 自签名证书
    ...
}
```

**tbb_cert.c 第 93-106 行** - SOC_FW_CONTENT_CERT：

```c
[SOC_FW_CONTENT_CERT] = {
    .id = SOC_FW_CONTENT_CERT,
    .key = SOC_FW_CONTENT_CERT_KEY,  // ← 指定使用 SOC_FW_CONTENT_CERT_KEY
    .issuer = SOC_FW_CONTENT_CERT,    // ← 自签名证书
    ...
}
```

### 3. 密钥的来源

**tbb_key.c 第 14-57 行** - 密钥定义：

```c
static cert_key_t tbb_keys[] = {
    [ROT_KEY] = {
        .id = ROT_KEY,
        .opt = "rot-key",  // ← 命令行选项
        .desc = "Root Of Trust key"
    },
    [SOC_FW_CONTENT_CERT_KEY] = {
        .id = SOC_FW_CONTENT_CERT_KEY,
        .opt = "soc-fw-key",  // ← 命令行选项
        .desc = "SoC Firmware Content Certificate key"
    },
    ...
};
```

**密钥加载**（main.c 第 430-471 行）：

```c
/* Load private keys from files (or generate new ones) */
for (i = 0; i < num_keys; i++) {
    if (keys[i].fn != NULL) {
        /* 从文件加载密钥 */
        if (!key_load(&keys[i])) {
            ERROR("Error loading '%s'\n", keys[i].fn);
            exit(1);
        }
    } else if (new_keys) {
        /* 自动生成新密钥 */
        if (!key_create(&keys[i], key_alg, key_size)) {
            ERROR("Error creating key '%s'\n", keys[i].desc);
            exit(1);
        }
    }
}
```

---

## 完整流程

### 步骤 1：指定密钥文件

```bash
cert_create \
    --rot-key rot_key.pem \                    # ← 指定 ROT_KEY
    --soc-fw-key soc_fw_content_key.pem \     # ← 指定 SOC_FW_CONTENT_CERT_KEY
    --trusted-key-cert trusted_key.crt \      # ← 生成 Trusted Key Cert
    --soc-fw-cert soc_fw_content.crt \       # ← 生成 Content Cert
    ...
```

### 步骤 2：cert_create 加载密钥

```c
// main.c 第 392-395 行
case CMD_OPT_KEY:
    cur_opt = cmd_opt_get_name(opt_idx);
    key = key_get_by_opt(cur_opt);  // ← 根据 --rot-key 找到密钥定义
    key->fn = strdup(optarg);       // ← 保存密钥文件路径

// main.c 第 254-282 行
unsigned int key_load(cert_key_t *key)
{
    // 从文件加载私钥
    key->key = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
    // 私钥中包含公钥信息
}
```

### 步骤 3：创建证书时自动设置 Subject Public Key

```c
// cert.c 第 95-176 行
int cert_new(...)
{
    // 1. 获取证书对应的密钥
    EVP_PKEY *pkey = keys[cert->key].key;  // ← 从证书定义获取
    
    // 2. 创建证书结构
    x = X509_new();
    
    // 3. 自动设置 Subject Public Key
    X509_set_pubkey(x, pkey);  // ← 这里自动添加 subject_pk
}
```

### 步骤 4：证书签名

```c
// cert.c 第 209-212 行
// 使用 issuer 的私钥签名证书
if (!X509_sign_ctx(x, mdCtx)) {
    ERR_print_errors_fp(stdout);
    goto END;
}
```

---

## 特殊情况处理

### 1. 自签名证书（Self-Signed Certificate）

**cert.c 第 120-124 行**：

```c
/* If we do not have a key, use the issuer key (the certificate will
 * become self signed). This happens in content certificates. */
if (!pkey) {
    pkey = ikey;  // ← 使用 issuer 的密钥作为 subject key
}
```

**示例**：Content Certificate 如果没有指定自己的密钥，会使用 issuer 的密钥，成为自签名证书。

### 2. 证书的 Subject Public Key 来源

| 证书类型 | Subject Public Key 来源 | 说明 |
|---------|----------------------|------|
| **Trusted Key Cert** | `ROT_KEY` | 使用 ROT_KEY 的公钥 |
| **SOC_FW_CONTENT_CERT** | `SOC_FW_CONTENT_CERT_KEY` | 使用 Content Cert Key 的公钥 |
| **SOC_FW_KEY_CERT** | `TRUSTED_WORLD_KEY` | 使用 Trusted World Key 的公钥 |

---

## 如何确保 Subject Public Key 正确设置

### 方法 1：通过命令行指定密钥文件

```bash
cert_create \
    --rot-key rot_key.pem \              # ← 确保密钥文件存在
    --trusted-key-cert trusted_key.crt \
    ...
```

### 方法 2：使用自动生成功能

```bash
cert_create \
    --new-keys \                         # ← 自动生成密钥
    --save-keys \                        # ← 保存生成的密钥
    --trusted-key-cert trusted_key.crt \
    ...
```

### 方法 3：验证证书中的 Subject Public Key

```bash
# 查看证书的 Subject Public Key
openssl x509 -in trusted_key.crt -pubkey -noout

# 或者查看完整证书信息
openssl x509 -in trusted_key.crt -text -noout
```

---

## Subject Public Key vs 扩展中的公钥

### 区别

| 位置 | 用途 | 如何添加 |
|------|------|---------|
| **Subject Public Key Info** | 证书本身的主体公钥，用于验证证书签名 | **自动添加**（`X509_set_pubkey()`） |
| **X.509 扩展中的公钥** | 用于分发下一级证书的公钥（如 Key Certificate） | **手动添加**（通过扩展） |

### 示例对比

**Trusted Key Certificate**：

```
证书结构：
├─ Subject Public Key Info: ROT_KEY 的公钥 ← 自动添加（subject_pk）
├─ Signature: 使用 ROT_KEY 的私钥签名
└─ X.509 Extensions:
    ├─ TRUSTED_WORLD_PK_EXT: Trusted World Key 的公钥 ← 手动添加（扩展）
    └─ NON_TRUSTED_WORLD_PK_EXT: Non-Trusted World Key 的公钥 ← 手动添加（扩展）
```

**SOC_FW_KEY_CERT**：

```
证书结构：
├─ Subject Public Key Info: TRUSTED_WORLD_KEY 的公钥 ← 自动添加（subject_pk）
├─ Signature: 使用 TRUSTED_WORLD_KEY 的私钥签名
└─ X.509 Extensions:
    └─ SOC_FW_CONTENT_CERT_PK_EXT: Content Cert Key 的公钥 ← 手动添加（扩展）
```

---

## 代码中的使用

### 1. 验证时提取 Subject Public Key

**mbedtls_x509_parser.c 第 479-487 行**：

```c
case AUTH_PARAM_PUB_KEY:
    if (type_desc->cookie != NULL) {
        /* 从扩展中获取公钥（通过 OID）*/
        rc = get_ext(type_desc->cookie, param, param_len);
    } else {
        /* 从证书的 Subject Public Key Info 获取公钥 */
        *param = (void *)pk.p;        // ← 这就是 subject_pk
        *param_len = (unsigned int)pk.len;
    }
    break;
```

**auth_mod.c 第 230-238 行** - 验证根证书时：

```c
/* Also retrieve the key from the image. */
rc = img_parser_get_auth_param(img_desc->img_type,
                               param->pk, img, img_len,
                               &pk_ptr, &pk_len);  // ← 提取 subject_pk

/* Validate the certificate's key against the platform ROTPK. */
// 比较证书的 subject_pk 与平台的 ROTPK
```

### 2. subject_pk 描述符

**tbbr_cot_common.c 第 39-40 行**：

```c
auth_param_type_desc_t subject_pk = AUTH_PARAM_TYPE_DESC(
    AUTH_PARAM_PUB_KEY, 0);  // ← cookie = 0 表示从 Subject Public Key Info 提取
```

**使用场景**（tbbr_cot_bl1.c 第 55 行）：

```c
static const auth_img_desc_t fwu_cert = {
    .img_auth_methods = {
        [0] = {
            .type = AUTH_METHOD_SIG,
            .param.sig = {
                .pk = &subject_pk,  // ← 使用 subject_pk 验证证书签名
                ...
            }
        }
    }
};
```

---

## 总结

### Subject Public Key 的添加方式

1. **自动添加**：
   - ✅ 使用 `X509_set_pubkey(x, pkey)` 自动设置
   - ✅ 公钥来自证书定义中 `.key` 字段指定的密钥
   - ✅ 不需要手动操作

2. **密钥来源**：
   - 从命令行指定的密钥文件加载（`--rot-key`, `--soc-fw-key` 等）
   - 或使用 `--new-keys` 自动生成

3. **验证使用**：
   - 使用 `subject_pk` 描述符（`cookie = 0`）从证书的 Subject Public Key Info 字段提取
   - 用于验证证书的签名

### 关键代码位置

- **设置 Subject Public Key**：`tools/cert_create/src/cert.c` 第 176 行
- **密钥加载**：`tools/cert_create/src/key.c` 第 254-282 行
- **提取 Subject Public Key**：`drivers/auth/mbedtls/mbedtls_x509_parser.c` 第 479-487 行
- **使用 Subject Public Key**：`drivers/auth/auth_mod.c` 第 230-280 行

### 重要提示

- ✅ **Subject Public Key 是自动添加的**，不需要手动操作
- ✅ **只需指定密钥文件**（通过 `--rot-key` 等选项）
- ✅ **cert_create 会自动处理**：加载密钥 → 提取公钥 → 添加到证书
- ✅ **验证时自动提取**：使用 `subject_pk` 描述符从证书中提取

---

**参考文档**：
- OpenSSL X.509 证书结构：https://www.openssl.org/docs/
- TF-A 证书工具：`tools/cert_create/src/cert.c`
- TF-A 认证框架：`drivers/auth/auth_mod.c`
