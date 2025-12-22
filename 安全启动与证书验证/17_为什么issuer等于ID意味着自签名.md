# 为什么 `.issuer = FWU_CERT` 代表自签名

## 核心原理

**自签名的判断**：当证书的 `issuer` 字段等于证书的 `id` 字段时，表示证书是**自签名**的。

---

## 代码逻辑分析

### FWU_CERT 的定义

**`tbb_cert.c` 第 189-203 行**：

```c
[FWU_CERT] = {
    .id = FWU_CERT,           // ← 证书的 ID
    .issuer = FWU_CERT,       // ← 签发者 ID = 自己的 ID
    .key = ROT_KEY,
    ...
}
```

**关键点**：
- `cert->id = FWU_CERT`
- `cert->issuer = FWU_CERT`
- **`cert->issuer == cert->id`** → 自签名 ✅

---

## 证书生成时的处理逻辑

### `cert_new()` 函数（`cert.c` 第 95-176 行）

```c
int cert_new(...)
{
    // 1. 获取当前证书的密钥（用于 Subject Public Key）
    EVP_PKEY *pkey = keys[cert->key].key;
    //   对于 FWU_CERT：pkey = keys[ROT_KEY].key
    
    // 2. 获取签发者证书
    cert_t *issuer_cert = &certs[cert->issuer];
    //   对于 FWU_CERT：issuer_cert = &certs[FWU_CERT]
    //   → issuer_cert 就是 FWU_CERT 自己！
    
    // 3. 获取签发者的密钥（用于签名）
    EVP_PKEY *ikey = keys[issuer_cert->key].key;
    //   对于 FWU_CERT：ikey = keys[certs[FWU_CERT].key].key
    //   → ikey = keys[ROT_KEY].key
    //   → ikey == pkey（都是 ROT_KEY 的密钥）
    
    // 4. 获取签发者证书对象
    X509 *issuer = issuer_cert->x;
    //   对于 FWU_CERT：issuer = certs[FWU_CERT].x
    //   如果证书还没创建，issuer 可能是 NULL
    
    // 5. 如果签发者证书对象不存在，使用自己的证书对象
    if (!issuer) {
        issuer = x;  // ← 使用自己的证书对象作为签发者
    }
    
    // 6. 设置证书的 Subject Public Key
    X509_set_pubkey(x, pkey);  // ← 使用自己的密钥
    
    // 7. 设置证书的 Issuer 和 Subject（都是自己）
    X509_set_issuer_name(x, name);  // ← Issuer = 自己
    X509_set_subject_name(x, name); // ← Subject = 自己
    
    // 8. 使用签发者的密钥签名（对于自签名，就是自己的密钥）
    X509_sign_ctx(x, mdCtx);  // ← 使用 ikey（自己的密钥）签名
}
```

---

## 自签名的判断过程

### 步骤 1：获取签发者证书

```c
// cert->issuer = FWU_CERT
cert_t *issuer_cert = &certs[cert->issuer];
// → issuer_cert = &certs[FWU_CERT]
// → issuer_cert 指向的就是 FWU_CERT 自己
```

**关键**：`issuer_cert` 和 `cert` 指向同一个证书定义！

### 步骤 2：获取签名密钥

```c
// 当前证书的密钥
EVP_PKEY *pkey = keys[cert->key].key;
// → pkey = keys[ROT_KEY].key

// 签发者的密钥（用于签名）
EVP_PKEY *ikey = keys[issuer_cert->key].key;
// → ikey = keys[certs[FWU_CERT].key].key
// → ikey = keys[ROT_KEY].key
// → ikey == pkey（同一个密钥！）
```

**关键**：签名密钥 `ikey` 和证书的密钥 `pkey` 是**同一个密钥**！

### 步骤 3：设置签发者和主体

```c
// 设置 Subject（证书的主体）
X509_set_subject_name(x, name);
// → Subject = "Firmware Update Certificate"

// 设置 Issuer（证书的签发者）
X509_set_issuer_name(x, name);
// → Issuer = "Firmware Update Certificate"
// → Issuer == Subject（都是自己！）
```

**关键**：证书的 Issuer 和 Subject 是**同一个实体**！

### 步骤 4：签名证书

```c
// 使用签发者的密钥签名
X509_sign_ctx(x, mdCtx);
// → 使用 ikey（ROT_KEY 的私钥）签名
// → 因为 ikey == pkey，所以是用自己的私钥签名
```

**关键**：证书使用**自己的私钥**签名！

---

## 自签名的完整含义

### 1. 签发者 = 自己

```
cert->issuer = FWU_CERT
cert->id = FWU_CERT

→ issuer_cert = &certs[FWU_CERT] = cert（自己）
```

### 2. 签名密钥 = 自己的密钥

```
ikey = keys[issuer_cert->key].key
     = keys[certs[FWU_CERT].key].key
     = keys[ROT_KEY].key

pkey = keys[cert->key].key
     = keys[ROT_KEY].key

→ ikey == pkey（同一个密钥）
```

### 3. Issuer DN = Subject DN

```
X509_set_issuer_name(x, name);   // Issuer = "Firmware Update Certificate"
X509_set_subject_name(x, name);  // Subject = "Firmware Update Certificate"

→ Issuer DN == Subject DN
```

### 4. 签名 = 用自己的私钥签名

```
签名密钥 = ROT_KEY 的私钥
Subject Public Key = ROT_KEY 的公钥

→ 使用自己的私钥签名，用自己的公钥验证
```

---

## 对比：自签名 vs 非自签名

### 自签名证书（FWU_CERT）

```c
[FWU_CERT] = {
    .id = FWU_CERT,
    .issuer = FWU_CERT,  // ← 签发者 = 自己
    .key = ROT_KEY,
}
```

**生成过程**：
```
1. issuer_cert = &certs[FWU_CERT]  // ← 签发者就是自己
2. ikey = keys[ROT_KEY].key         // ← 签名密钥
3. pkey = keys[ROT_KEY].key        // ← Subject PK（同一个密钥）
4. Issuer DN = Subject DN          // ← 都是 "Firmware Update Certificate"
5. 使用 ROT_KEY 的私钥签名
```

### 非自签名证书（SOC_FW_KEY_CERT）

```c
[SOC_FW_KEY_CERT] = {
    .id = SOC_FW_KEY_CERT,
    .issuer = SOC_FW_KEY_CERT,  // ← 虽然也是自签名，但让我们看一个例子
    .key = TRUSTED_WORLD_KEY,
}
```

**实际上，TF-A 中所有证书都是自签名的**。但如果是非自签名的情况：

```c
[SOC_FW_KEY_CERT] = {
    .id = SOC_FW_KEY_CERT,
    .issuer = TRUSTED_KEY_CERT,  // ← 签发者是父证书
    .key = TRUSTED_WORLD_KEY,
}
```

**生成过程**：
```
1. issuer_cert = &certs[TRUSTED_KEY_CERT]  // ← 签发者是父证书
2. ikey = keys[TRUSTED_KEY_CERT.key].key   // ← 父证书的密钥（ROT_KEY）
3. pkey = keys[TRUSTED_WORLD_KEY].key      // ← 自己的密钥（不同！）
4. Issuer DN = "Trusted Key Certificate"   // ← 父证书的名称
5. Subject DN = "SoC Firmware Key Certificate"  // ← 自己的名称
6. 使用 ROT_KEY 的私钥签名（父证书的密钥）
```

---

## 为什么 TF-A 使用自签名？

### 1. 简化设计

- ✅ **不需要证书颁发机构（CA）**：每个证书自己签名自己
- ✅ **减少依赖**：不需要外部 CA 证书
- ✅ **简化验证**：只需要验证证书的签名，不需要验证 CA 链

### 2. 信任根在平台

- ✅ **ROTPK 是信任根**：平台存储的 ROTPK 是最终的信任锚点
- ✅ **根证书由 ROTPK 验证**：根证书的 Subject PK 必须匹配平台 ROTPK
- ✅ **后续证书由根证书的公钥验证**：形成信任链

### 3. 信任链结构

```
平台 ROTPK（硬件/固件存储）
    ↓ 验证
根证书（TRUSTED_KEY_CERT, TRUSTED_BOOT_FW_CERT, FWU_CERT）
    ├─ Subject PK = ROTPK
    └─ Signature = ROT_KEY 私钥签名（自签名）
    ↓ 提取公钥
后续证书（SOC_FW_KEY_CERT, etc.）
    ├─ Subject PK = 自己的公钥
    └─ Signature = 父证书的公钥对应的私钥签名（自签名）
```

---

## 代码验证

### 验证自签名的逻辑

**`cert.c` 第 103-104 行**：

```c
cert_t *issuer_cert = &certs[cert->issuer];
EVP_PKEY *ikey = keys[issuer_cert->key].key;
```

**对于 FWU_CERT**：

```c
cert->issuer = FWU_CERT
→ issuer_cert = &certs[FWU_CERT]
→ issuer_cert == cert（指向同一个证书）

cert->key = ROT_KEY
issuer_cert->key = ROT_KEY
→ ikey = keys[ROT_KEY].key
→ pkey = keys[ROT_KEY].key
→ ikey == pkey（同一个密钥）
```

**结论**：
- ✅ 签发者证书 = 自己
- ✅ 签名密钥 = 自己的密钥
- ✅ **自签名** ✅

---

## 总结

### `.issuer = FWU_CERT` 代表自签名的原因

1. **签发者 ID = 证书 ID**：
   ```c
   cert->issuer = FWU_CERT
   cert->id = FWU_CERT
   → issuer_cert = &certs[FWU_CERT] = cert（自己）
   ```

2. **签名密钥 = 自己的密钥**：
   ```c
   ikey = keys[issuer_cert->key].key = keys[ROT_KEY].key
   pkey = keys[cert->key].key = keys[ROT_KEY].key
   → ikey == pkey（同一个密钥）
   ```

3. **Issuer DN = Subject DN**：
   ```c
   X509_set_issuer_name(x, name);   // Issuer = "Firmware Update Certificate"
   X509_set_subject_name(x, name);  // Subject = "Firmware Update Certificate"
   ```

4. **使用自己的私钥签名**：
   ```c
   X509_sign_ctx(x, mdCtx);  // 使用 ikey（自己的私钥）签名
   ```

### 自签名的判断标准

**简单判断**：
```c
if (cert->issuer == cert->id) {
    // 自签名 ✅
}
```

**对于 FWU_CERT**：
```c
cert->issuer = FWU_CERT
cert->id = FWU_CERT
→ cert->issuer == cert->id → 自签名 ✅
```

---

**参考代码位置**：
- 证书定义：`tools/cert_create/src/tbbr/tbb_cert.c` 第 189-203 行
- 证书生成：`tools/cert_create/src/cert.c` 第 103-104 行、128-130 行
- 注释说明：`tools/cert_create/src/tbbr/tbb_cert.c` 第 15-16 行
