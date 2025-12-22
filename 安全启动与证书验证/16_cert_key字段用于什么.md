# 证书生成时 `.key` 字段的作用

## 概述

在证书生成时，`.key` 字段有两个关键作用：
1. **作为证书的 Subject Public Key**（证书自己的公钥）
2. **对于自签名证书，也作为签名密钥**（因为签发者就是自己）

---

## 代码分析

### `cert_new()` 函数（`cert.c` 第 95-176 行）

```c
int cert_new(...)
{
    // 1. 获取证书的密钥（用于 Subject Public Key）
    EVP_PKEY *pkey = keys[cert->key].key;
    //    对于 TRUSTED_KEY_CERT：pkey = keys[ROT_KEY].key
    
    // 2. 获取签发者的密钥（用于签名）
    cert_t *issuer_cert = &certs[cert->issuer];
    EVP_PKEY *ikey = keys[issuer_cert->key].key;
    //    对于 TRUSTED_KEY_CERT：ikey = keys[certs[TRUSTED_KEY_CERT].key].key
    //    → ikey = keys[ROT_KEY].key
    //    → ikey == pkey（同一个密钥！）
    
    // 3. 设置证书的 Subject Public Key
    X509_set_pubkey(x, pkey);  // ← 使用 cert->key 的公钥
    //    证书的 Subject Public Key = ROT_KEY 的公钥
    
    // 4. 使用签发者的密钥签名证书
    EVP_DigestSignInit(mdCtx, &pKeyCtx, get_digest(md_alg), NULL, ikey);
    //    使用 ikey（ROT_KEY 的私钥）签名
    X509_sign_ctx(x, mdCtx);
}
```

---

## `.key` 字段的两个作用

### 作用 1：作为证书的 Subject Public Key

**代码位置**：`cert.c` 第 102 行和第 176 行

```c
// 获取密钥
EVP_PKEY *pkey = keys[cert->key].key;
// 对于 TRUSTED_KEY_CERT：pkey = keys[ROT_KEY].key

// 设置证书的 Subject Public Key
X509_set_pubkey(x, pkey);
// ← 证书的 Subject Public Key = ROT_KEY 的公钥
```

**作用**：
- ✅ 证书的 **Subject Public Key Info** 字段 = `cert->key` 的公钥
- ✅ 这个公钥用于验证证书的签名
- ✅ 这个公钥也会被提取到 `authenticated_data`（如果证书有扩展）

### 作用 2：对于自签名证书，也作为签名密钥

**代码位置**：`cert.c` 第 103-104 行和第 139 行

```c
// 获取签发者的密钥
cert_t *issuer_cert = &certs[cert->issuer];
EVP_PKEY *ikey = keys[issuer_cert->key].key;
// 对于 TRUSTED_KEY_CERT（自签名）：
// issuer_cert = &certs[TRUSTED_KEY_CERT]
// ikey = keys[certs[TRUSTED_KEY_CERT].key].key
// ikey = keys[ROT_KEY].key
// ikey == pkey（同一个密钥！）

// 使用签发者的密钥签名
EVP_DigestSignInit(mdCtx, &pKeyCtx, get_digest(md_alg), NULL, ikey);
X509_sign_ctx(x, mdCtx);
// ← 使用 ikey（ROT_KEY 的私钥）签名证书
```

**对于自签名证书**：
- ✅ `cert->issuer == cert->id` → `issuer_cert == cert`
- ✅ `issuer_cert->key == cert->key` → `ikey == pkey`
- ✅ **签名密钥 = 证书的密钥**（使用自己的私钥签名）

---

## 完整示例：生成 TRUSTED_KEY_CERT

### 证书定义

```c
[TRUSTED_KEY_CERT] = {
    .id = TRUSTED_KEY_CERT,
    .key = ROT_KEY,              // ← 证书的密钥
    .issuer = TRUSTED_KEY_CERT,  // ← 自签名
    ...
}
```

### 证书生成过程

```c
1. 获取证书的密钥
   pkey = keys[cert->key].key
        = keys[ROT_KEY].key
   → pkey = ROT_KEY 的公钥/私钥对

2. 获取签发者的密钥
   issuer_cert = &certs[cert->issuer]
                = &certs[TRUSTED_KEY_CERT]
                = cert（自己）
   
   ikey = keys[issuer_cert->key].key
        = keys[certs[TRUSTED_KEY_CERT].key].key
        = keys[ROT_KEY].key
        = pkey（同一个密钥！）

3. 设置证书的 Subject Public Key
   X509_set_pubkey(x, pkey)
   → 证书的 Subject Public Key = ROT_KEY 的公钥

4. 签名证书
   X509_sign_ctx(x, mdCtx)
   → 使用 ikey（ROT_KEY 的私钥）签名
   → 因为 ikey == pkey，所以是用自己的私钥签名自己
```

---

## `.key` 字段的用途总结

### 用途 1：Subject Public Key（证书自己的公钥）

```c
X509_set_pubkey(x, pkey);
// pkey = keys[cert->key].key
// → 证书的 Subject Public Key = cert->key 的公钥
```

**作用**：
- ✅ 证书的 Subject Public Key Info 字段
- ✅ 用于验证证书的签名
- ✅ 可以被提取到 `authenticated_data`（如果证书有扩展）

### 用途 2：签名密钥（对于自签名证书）

```c
EVP_DigestSignInit(mdCtx, &pKeyCtx, get_digest(md_alg), NULL, ikey);
// ikey = keys[issuer_cert->key].key
// 对于自签名：ikey = keys[cert->key].key = pkey
```

**作用**：
- ✅ 对于自签名证书，签名密钥 = 证书的密钥
- ✅ 使用 `cert->key` 的私钥签名证书
- ✅ 使用 `cert->key` 的公钥验证签名

---

## 关键区别

### `pkey` vs `ikey`

| 变量 | 来源 | 用途 | 对于自签名证书 |
|------|------|------|---------------|
| **`pkey`** | `keys[cert->key].key` | Subject Public Key | ROT_KEY 的公钥 |
| **`ikey`** | `keys[issuer_cert->key].key` | 签名密钥 | ROT_KEY 的私钥 |

**对于自签名证书**：
- `pkey` = ROT_KEY 的公钥（用于 Subject PK）
- `ikey` = ROT_KEY 的私钥（用于签名）
- `pkey` 和 `ikey` 是**同一个密钥对**的公钥和私钥部分

---

## 实际例子

### 例子 1：TRUSTED_KEY_CERT（自签名）

```c
[TRUSTED_KEY_CERT] = {
    .key = ROT_KEY,
    .issuer = TRUSTED_KEY_CERT,  // ← 自签名
}
```

**生成过程**：

```c
pkey = keys[ROT_KEY].key        // ← 证书的密钥
ikey = keys[ROT_KEY].key        // ← 签发者的密钥（自己）

X509_set_pubkey(x, pkey);       // ← Subject PK = ROT_KEY 的公钥
X509_sign_ctx(x, mdCtx);        // ← 使用 ROT_KEY 的私钥签名
```

**结果**：
- ✅ 证书的 Subject Public Key = ROT_KEY 的公钥
- ✅ 证书的签名 = ROT_KEY 的私钥签名
- ✅ 验证时：使用 ROT_KEY 的公钥验证签名

### 例子 2：如果 SOC_FW_KEY_CERT 不是自签名（假设）

```c
[SOC_FW_KEY_CERT] = {
    .key = TRUSTED_WORLD_KEY,        // ← 证书的密钥
    .issuer = TRUSTED_KEY_CERT,      // ← 签发者是父证书
}
```

**生成过程**：

```c
pkey = keys[TRUSTED_WORLD_KEY].key  // ← 证书的密钥
ikey = keys[TRUSTED_KEY_CERT.key].key
     = keys[ROT_KEY].key             // ← 签发者的密钥（父证书）

X509_set_pubkey(x, pkey);            // ← Subject PK = TRUSTED_WORLD_KEY 的公钥
X509_sign_ctx(x, mdCtx);             // ← 使用 ROT_KEY 的私钥签名
```

**结果**：
- ✅ 证书的 Subject Public Key = TRUSTED_WORLD_KEY 的公钥
- ✅ 证书的签名 = ROT_KEY 的私钥签名
- ✅ 验证时：使用 ROT_KEY 的公钥验证签名（从父证书获取）

**注意**：在 TF-A 中，所有证书都是自签名的，所以这个例子只是说明区别。

---

## 总结

### `.key` 字段在证书生成时的作用

1. **作为 Subject Public Key**：
   ```c
   X509_set_pubkey(x, keys[cert->key].key);
   ```
   - 证书的 Subject Public Key = `cert->key` 的公钥

2. **对于自签名证书，也作为签名密钥**：
   ```c
   ikey = keys[issuer_cert->key].key;
   // 对于自签名：ikey = keys[cert->key].key（同一个密钥）
   X509_sign_ctx(x, mdCtx);  // 使用 ikey 的私钥签名
   ```
   - 签名密钥 = `cert->key` 的私钥

### 关键点

- ✅ **`cert->key` 指定证书的密钥对**（公钥和私钥）
- ✅ **公钥部分** → 成为证书的 Subject Public Key
- ✅ **私钥部分** → 用于签名证书（对于自签名证书）
- ✅ **验证时**：使用证书的 Subject Public Key（公钥）验证签名

### 为什么需要 `.key` 字段？

1. **指定证书的公钥**：每个证书都需要一个公钥（Subject Public Key）
2. **指定签名密钥**：对于自签名证书，签名密钥就是证书的密钥
3. **形成信任链**：证书的公钥可以被提取到 `authenticated_data`，用于验证下一级证书

---

**参考代码位置**：
- 证书密钥获取：`tools/cert_create/src/cert.c` 第 102 行
- Subject PK 设置：`tools/cert_create/src/cert.c` 第 176 行
- 签名密钥获取：`tools/cert_create/src/cert.c` 第 104 行
- 签名过程：`tools/cert_create/src/cert.c` 第 139 行
