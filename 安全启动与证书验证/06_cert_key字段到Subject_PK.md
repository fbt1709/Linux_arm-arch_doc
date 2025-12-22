# `.key` 字段与 `subject_pk` 的关系

## 答案

**是的**，`.key` 字段的公钥部分最终成为证书的 **Subject Public Key**，验证时通过 `subject_pk` 描述符提取。

---

## 证书生成时：`.key` → Subject Public Key

### 代码位置：`cert.c` 第 102 行和第 176 行

```c
int cert_new(...)
{
    // 1. 获取证书的密钥
    EVP_PKEY *pkey = keys[cert->key].key;
    //    对于 TRUSTED_KEY_CERT：pkey = keys[ROT_KEY].key
    
    // 2. 设置证书的 Subject Public Key
    X509_set_pubkey(x, pkey);
    // ← 证书的 Subject Public Key = cert->key 的公钥
    // ← 这就是 X.509 证书的 Subject Public Key Info 字段
}
```

**结果**：
- ✅ 证书的 **Subject Public Key Info** 字段 = `cert->key` 的公钥
- ✅ 这个公钥存储在证书的 `subjectPublicKeyInfo` 字段中

---

## 证书验证时：Subject Public Key → `subject_pk`

### `subject_pk` 描述符定义

**位置**：`tbbr_cot_common.c` 第 39-40 行

```c
auth_param_type_desc_t subject_pk = AUTH_PARAM_TYPE_DESC(
    AUTH_PARAM_PUB_KEY, 0);
// type = AUTH_PARAM_PUB_KEY（公钥类型）
// cookie = 0（表示提取 Subject Public Key，而不是扩展中的公钥）
```

### 使用 `subject_pk` 提取 Subject Public Key

**位置**：`auth_mod.c` 第 231-233 行（根证书验证）

```c
// 从证书中提取 Subject Public Key
rc = img_parser_get_auth_param(img_desc->img_type,
                               param->pk,  // ← param->pk = &subject_pk
                               img, img_len,
                               &pk_ptr, &pk_len);
// pk_ptr → 指向证书的 Subject Public Key
```

### 提取实现

**位置**：`mbedtls_x509_parser.c` 第 479-487 行

```c
case AUTH_PARAM_PUB_KEY:
    if (type_desc->cookie != NULL) {
        /* Get public key from extension */
        rc = get_ext(type_desc->cookie, param, param_len);
    } else {
        /* Get the subject public key */
        *param = (void *)pk.p;        // ← 指向 Subject Public Key
        *param_len = (unsigned int)pk.len;
    }
    break;
```

**关键点**：
- ✅ `subject_pk.cookie = 0`（NULL）
- ✅ 所以走 `else` 分支：提取 Subject Public Key
- ✅ `pk.p` 是在 `cert_parse()` 时解析的 Subject Public Key Info

---

## 完整流程

### 证书生成时

```
cert->key = ROT_KEY
    ↓
pkey = keys[ROT_KEY].key
    ↓
X509_set_pubkey(x, pkey)
    ↓
证书的 Subject Public Key Info = ROT_KEY 的公钥
```

### 证书验证时

```
证书的 Subject Public Key Info（在证书中）
    ↓ cert_parse() 解析
pk.p, pk.len（解析出的 Subject Public Key）
    ↓ img_parser_get_auth_param(..., &subject_pk, ...)
pk_ptr = pk.p（指向 Subject Public Key）
    ↓
用于验证证书签名
```

---

## 代码验证

### 1. 证书生成（`cert.c`）

```c
// 设置 Subject Public Key
X509_set_pubkey(x, keys[cert->key].key);
// ← cert->key 的公钥 → 证书的 Subject Public Key
```

### 2. 证书解析（`mbedtls_x509_parser.c`）

```c
// cert_parse() 解析证书时
// 提取 Subject Public Key Info
// 存储到 pk.p 和 pk.len
```

### 3. 证书验证（`mbedtls_x509_parser.c`）

```c
case AUTH_PARAM_PUB_KEY:
    if (type_desc->cookie == NULL) {  // ← subject_pk.cookie = 0
        *param = (void *)pk.p;        // ← 返回 Subject Public Key
        *param_len = (unsigned int)pk.len;
    }
```

### 4. 使用 `subject_pk`（`tbbr_cot_bl1.c`）

```c
static const auth_img_desc_t fwu_cert = {
    .img_auth_methods = {
        [0] = {
            .param.sig = {
                .pk = &subject_pk,  // ← 使用 subject_pk 提取 Subject PK
                ...
            }
        }
    }
};
```

---

## 总结

### `.key` 字段 → Subject Public Key

1. **证书生成时**：
   ```c
   cert->key = ROT_KEY
   → X509_set_pubkey(x, keys[ROT_KEY].key)
   → 证书的 Subject Public Key = ROT_KEY 的公钥
   ```

2. **证书验证时**：
   ```c
   param->pk = &subject_pk
   → img_parser_get_auth_param(..., &subject_pk, ...)
   → pk_ptr = 证书的 Subject Public Key
   → 使用 Subject Public Key 验证签名
   ```

### 关键对应关系

| 阶段 | 代码 | 结果 |
|------|------|------|
| **证书生成** | `X509_set_pubkey(x, keys[cert->key].key)` | 证书的 Subject Public Key = `cert->key` 的公钥 |
| **证书验证** | `img_parser_get_auth_param(..., &subject_pk, ...)` | `pk_ptr` = 证书的 Subject Public Key |

### 结论

- ✅ **`.key` 字段的公钥部分** → 成为证书的 **Subject Public Key**
- ✅ **`subject_pk` 描述符** → 用于提取证书的 **Subject Public Key**
- ✅ **两者对应**：`.key` 的公钥 = `subject_pk` 提取的内容

---

**参考代码位置**：
- 证书生成：`tools/cert_create/src/cert.c` 第 176 行
- Subject PK 定义：`drivers/auth/tbbr/tbbr_cot_common.c` 第 39-40 行
- Subject PK 提取：`drivers/auth/mbedtls/mbedtls_x509_parser.c` 第 484-486 行
- Subject PK 使用：`drivers/auth/tbbr/tbbr_cot_bl1.c` 第 55 行
