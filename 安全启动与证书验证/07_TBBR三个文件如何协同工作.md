# TBBR 三个文件如何配合运行

## 概述

TBBR 目录下的三个文件（`tbb_cert.c`, `tbb_ext.c`, `tbb_key.c`）通过**注册机制**和**引用关系**协同工作，共同定义了完整的证书链结构。

---

## 三个文件的职责

### 1. `tbb_key.c` - 密钥定义
**职责**：定义所有需要的密钥类型

```c
static cert_key_t tbb_keys[] = {
    [ROT_KEY] = {
        .id = ROT_KEY,
        .opt = "rot-key",              // ← 命令行选项
        .help_msg = "Root Of Trust key file or PKCS11 URI",
        .desc = "Root Of Trust key"
    },
    [TRUSTED_WORLD_KEY] = { ... },
    [NON_TRUSTED_WORLD_KEY] = { ... },
    [SCP_FW_CONTENT_CERT_KEY] = { ... },
    [SOC_FW_CONTENT_CERT_KEY] = { ... },
    [TRUSTED_OS_FW_CONTENT_CERT_KEY] = { ... },
    [NON_TRUSTED_FW_CONTENT_CERT_KEY] = { ... }
};

REGISTER_KEYS(tbb_keys);  // ← 注册到全局变量
```

**作用**：
- 定义密钥的 ID、命令行选项、描述信息
- 通过 `REGISTER_KEYS` 宏注册到全局变量 `def_keys`

### 2. `tbb_ext.c` - 扩展定义
**职责**：定义所有证书扩展类型

```c
static ext_t tbb_ext[] = {
    [TRUSTED_WORLD_PK_EXT] = {
        .oid = TRUSTED_WORLD_PK_OID,
        .sn = "TrustedWorldPublicKey",
        .ln = "Trusted World Public Key",
        .type = EXT_TYPE_PKEY,
        .attr.key = TRUSTED_WORLD_KEY  // ← 引用密钥 ID
    },
    [SOC_FW_CONTENT_CERT_PK_EXT] = {
        .oid = SOC_FW_CONTENT_CERT_PK_OID,
        .type = EXT_TYPE_PKEY,
        .attr.key = SOC_FW_CONTENT_CERT_KEY  // ← 引用密钥 ID
    },
    [SOC_AP_FW_HASH_EXT] = {
        .oid = SOC_AP_FW_HASH_OID,
        .opt = "soc-fw",                // ← 命令行选项（用于指定镜像文件）
        .type = EXT_TYPE_HASH
    },
    ...
};

REGISTER_EXTENSIONS(tbb_ext);  // ← 注册到全局变量
```

**作用**：
- 定义扩展的 OID、类型（HASH/PKEY/NVCOUNTER）、命令行选项
- **通过 `.attr.key` 引用密钥 ID**（对于公钥扩展）
- 通过 `REGISTER_EXTENSIONS` 宏注册到全局变量 `def_extensions`

### 3. `tbb_cert.c` - 证书定义
**职责**：定义所有证书及其包含的扩展

```c
static cert_t tbb_certs[] = {
    [SOC_FW_KEY_CERT] = {
        .id = SOC_FW_KEY_CERT,
        .opt = "soc-fw-key-cert",
        .key = TRUSTED_WORLD_KEY,      // ← 引用密钥 ID（用于签名和 Subject PK）
        .issuer = SOC_FW_KEY_CERT,     // ← 自签名
        .ext = {
            TRUSTED_FW_NVCOUNTER_EXT,  // ← 引用扩展 ID
            SOC_FW_CONTENT_CERT_PK_EXT // ← 引用扩展 ID
        },
        .num_ext = 2
    },
    [SOC_FW_CONTENT_CERT] = {
        .id = SOC_FW_CONTENT_CERT,
        .opt = "soc-fw-cert",
        .key = SOC_FW_CONTENT_CERT_KEY,  // ← 引用密钥 ID
        .issuer = SOC_FW_CONTENT_CERT,
        .ext = {
            TRUSTED_FW_NVCOUNTER_EXT,
            SOC_AP_FW_HASH_EXT,         // ← 引用扩展 ID
            SOC_FW_CONFIG_HASH_EXT
        },
        .num_ext = 3
    },
    ...
};

REGISTER_COT(tbb_certs);  // ← 注册到全局变量
```

**作用**：
- 定义证书的 ID、命令行选项、签名密钥、签发者
- **通过 `.ext[]` 数组引用扩展 ID**
- **通过 `.key` 引用密钥 ID**（用于签名和 Subject Public Key）
- 通过 `REGISTER_COT` 宏注册到全局变量 `def_certs`

---

## 注册机制

### 宏定义（在头文件中）

**`include/key.h` 第 82-84 行**：
```c
#define REGISTER_KEYS(_keys) \
    cert_key_t *def_keys = &_keys[0]; \
    const unsigned int num_def_keys = sizeof(_keys)/sizeof(_keys[0])
```

**`include/ext.h` 第 79-81 行**：
```c
#define REGISTER_EXTENSIONS(_ext) \
    ext_t *def_extensions = &_ext[0]; \
    const unsigned int num_def_extensions = sizeof(_ext)/sizeof(_ext[0])
```

**`include/cert.h` 第 60-62 行**：
```c
#define REGISTER_COT(_certs) \
    cert_t *def_certs = &_certs[0]; \
    const unsigned int num_def_certs = sizeof(_certs)/sizeof(_certs[0])
```

**作用**：
- 将静态数组注册为全局变量（`def_keys`, `def_extensions`, `def_certs`）
- 计算数组大小并保存到对应的 `num_*` 变量中

---

## 初始化流程

### 1. 编译时注册（静态）

```c
// tbb_key.c
REGISTER_KEYS(tbb_keys);  // → def_keys = &tbb_keys[0], num_def_keys = 7

// tbb_ext.c
REGISTER_EXTENSIONS(tbb_ext);  // → def_extensions = &tbb_ext[0], num_def_extensions = ...

// tbb_cert.c
REGISTER_COT(tbb_certs);  // → def_certs = &tbb_certs[0], num_def_certs = 11
```

### 2. 运行时初始化（`main.c` 第 323-339 行）

```c
/* Initialize the certificates */
cert_init();  // ← 步骤 1：初始化证书

/* Initialize the keys */
key_init();   // ← 步骤 2：初始化密钥

/* Initialize the extensions */
ext_init();   // ← 步骤 3：初始化扩展
```

### 3. 初始化函数详解

#### `cert_init()` (`cert.c` 第 223-270 行)

```c
int cert_init(void)
{
    // 1. 分配内存
    certs = malloc((num_def_certs * sizeof(def_certs[0])));
    
    // 2. 复制注册的证书数组到全局变量
    memcpy(&certs[0], &def_certs[0], (num_def_certs * sizeof(def_certs[0])));
    num_certs = num_def_certs;
    
    // 3. 为每个证书注册命令行选项
    for (i = 0; i < num_certs; i++) {
        cert = &certs[i];
        cmd_opt.long_opt.name = cert->opt;  // 例如 "--soc-fw-key-cert"
        cmd_opt_add(&cmd_opt);
    }
    
    return 0;
}
```

**结果**：
- 全局变量 `certs[]` 包含所有证书定义
- 命令行选项已注册（如 `--soc-fw-key-cert`）

#### `key_init()` (`key.c` 第 309-350 行)

```c
int key_init(void)
{
    // 1. 分配内存
    keys = malloc((num_def_keys * sizeof(def_keys[0])));
    
    // 2. 复制注册的密钥数组到全局变量
    memcpy(&keys[0], &def_keys[0], (num_def_keys * sizeof(def_keys[0])));
    num_keys = num_def_keys;
    
    // 3. 为每个密钥注册命令行选项
    for (i = 0; i < num_keys; i++) {
        key = &keys[i];
        if (key->opt != NULL) {
            cmd_opt.long_opt.name = key->opt;  // 例如 "--soc-fw-key"
            cmd_opt_add(&cmd_opt);
        }
    }
    
    return 0;
}
```

**结果**：
- 全局变量 `keys[]` 包含所有密钥定义
- 命令行选项已注册（如 `--soc-fw-key`）

#### `ext_init()` (`ext.c` 第 50-150 行)

```c
int ext_init(void)
{
    // 1. 分配内存
    extensions = malloc((num_def_extensions * sizeof(def_extensions[0])));
    
    // 2. 复制注册的扩展数组到全局变量
    memcpy(&extensions[0], &def_extensions[0], ...);
    num_extensions = num_def_extensions;
    
    // 3. 为每个扩展注册命令行选项和 OID
    for (i = 0; i < num_extensions; i++) {
        ext = &extensions[i];
        
        // 注册命令行选项（如果有）
        if (ext->opt) {
            cmd_opt.long_opt.name = ext->opt;  // 例如 "--soc-fw"
            cmd_opt_add(&cmd_opt);
        }
        
        // 在 OpenSSL 中注册 OID
        nid = OBJ_create(ext->oid, ext->sn, ext->ln);
        ...
    }
    
    return 0;
}
```

**结果**：
- 全局变量 `extensions[]` 包含所有扩展定义
- 命令行选项已注册（如 `--soc-fw`）
- OID 已在 OpenSSL 中注册

---

## 证书生成流程

### 步骤 1：解析命令行参数（`main.c` 第 344-407 行）

```c
while (1) {
    c = getopt_long(argc, argv, "a:b:hknps:", cmd_opt, &opt_idx);
    
    switch (c) {
    case CMD_OPT_CERT:  // 例如 "--soc-fw-key-cert"
        cert = cert_get_by_opt(cur_opt);  // ← 从 certs[] 中找到证书
        cert->fn = strdup(optarg);        // ← 保存输出文件名
        break;
        
    case CMD_OPT_KEY:  // 例如 "--soc-fw-key"
        key = key_get_by_opt(cur_opt);    // ← 从 keys[] 中找到密钥
        key->fn = strdup(optarg);         // ← 保存密钥文件路径
        break;
        
    case CMD_OPT_EXT:  // 例如 "--soc-fw"
        ext = ext_get_by_opt(cur_opt);    // ← 从 extensions[] 中找到扩展
        ext->arg = strdup(optarg);        // ← 保存镜像文件路径
        break;
    }
}
```

### 步骤 2：加载密钥（`main.c` 第 430-471 行）

```c
for (i = 0; i < num_keys; i++) {
    // 从文件加载私钥
    err_code = key_load(&keys[i]);
    if (err_code == KEY_ERR_NONE) {
        continue;  // 加载成功
    }
    
    // 如果文件不存在且 --new-keys 选项，生成新密钥
    if (new_keys) {
        key_create(&keys[i], key_alg, key_size);
    }
}
```

**结果**：
- `keys[i].key` 包含已加载的 `EVP_PKEY*` 对象

### 步骤 3：生成证书（`main.c` 第 473-560 行）

```c
for (i = 0; i < num_certs; i++) {
    cert = &certs[i];
    
    if (cert->fn == NULL) {
        continue;  // 证书未请求，跳过
    }
    
    // 创建扩展栈
    sk = sk_X509_EXTENSION_new_null();
    
    // 遍历证书的所有扩展
    for (j = 0; j < cert->num_ext; j++) {
        ext = &extensions[cert->ext[j]];  // ← 从全局 extensions[] 获取扩展
        
        switch (ext->type) {
        case EXT_TYPE_PKEY:
            // 从 keys[] 中获取密钥，提取公钥创建扩展
            cert_ext = ext_new_key(ext_nid, EXT_CRIT, 
                                   keys[ext->attr.key].key);  // ← 引用密钥
            break;
            
        case EXT_TYPE_HASH:
            // 计算镜像文件的 hash，创建扩展
            sha_file(hash_alg, ext->arg, md);
            cert_ext = ext_new_hash(ext_nid, EXT_CRIT, md_info, md, md_len);
            break;
            
        case EXT_TYPE_NVCOUNTER:
            // 创建 NV Counter 扩展
            cert_ext = ext_new_nvcounter(ext_nid, EXT_CRIT, nvctr);
            break;
        }
        
        sk_X509_EXTENSION_push(sk, cert_ext);
    }
    
    // 创建并签名证书
    cert_new(hash_alg, cert, VAL_DAYS, 0, sk);
}
```

**关键引用关系**：
1. `cert->ext[j]` → `extensions[cert->ext[j]]`（证书引用扩展）
2. `ext->attr.key` → `keys[ext->attr.key]`（扩展引用密钥）
3. `cert->key` → `keys[cert->key]`（证书引用密钥，用于签名和 Subject PK）

---

## 引用关系图

```
tbb_key.c
    ↓ REGISTER_KEYS
def_keys[] (全局变量)
    ↑
    │ 引用（通过 ID）
    │
tbb_ext.c
    ↓ REGISTER_EXTENSIONS
def_extensions[]
    │
    │ .attr.key = KEY_ID
    │
    ↓
tbb_cert.c
    ↓ REGISTER_COT
def_certs[]
    │
    │ .ext[] = { EXT_ID, ... }
    │ .key = KEY_ID
    │
    ↓
main.c (证书生成)
    │
    ├─ certs[i].ext[j] → extensions[cert->ext[j]]
    ├─ extensions[k].attr.key → keys[ext->attr.key]
    └─ certs[i].key → keys[cert->key]
```

---

## 完整示例：生成 SOC_FW_KEY_CERT

### 1. 证书定义（`tbb_cert.c`）

```c
[SOC_FW_KEY_CERT] = {
    .key = TRUSTED_WORLD_KEY,           // ← 引用密钥 ID
    .ext = {
        TRUSTED_FW_NVCOUNTER_EXT,       // ← 引用扩展 ID
        SOC_FW_CONTENT_CERT_PK_EXT      // ← 引用扩展 ID
    }
}
```

### 2. 扩展定义（`tbb_ext.c`）

```c
[SOC_FW_CONTENT_CERT_PK_EXT] = {
    .oid = SOC_FW_CONTENT_CERT_PK_OID,
    .type = EXT_TYPE_PKEY,
    .attr.key = SOC_FW_CONTENT_CERT_KEY  // ← 引用密钥 ID
}
```

### 3. 密钥定义（`tbb_key.c`）

```c
[SOC_FW_CONTENT_CERT_KEY] = {
    .id = SOC_FW_CONTENT_CERT_KEY,
    .opt = "soc-fw-key"
}
```

### 4. 命令行调用

```bash
cert_create \
    --soc-fw-key-cert soc_fw_key.crt \      # ← 匹配 cert->opt
    --trusted-world-key trusted_world.pem \ # ← 匹配 key->opt (用于签名)
    --soc-fw-key soc_fw_content.pem         # ← 匹配 key->opt (用于扩展)
```

### 5. 执行流程

```
1. cert_init()
   → certs[SOC_FW_KEY_CERT] = { ... }

2. key_init()
   → keys[TRUSTED_WORLD_KEY] = { ... }
   → keys[SOC_FW_CONTENT_CERT_KEY] = { ... }

3. ext_init()
   → extensions[SOC_FW_CONTENT_CERT_PK_EXT] = { .attr.key = SOC_FW_CONTENT_CERT_KEY }

4. 解析命令行
   → certs[SOC_FW_KEY_CERT].fn = "soc_fw_key.crt"
   → keys[TRUSTED_WORLD_KEY].fn = "trusted_world.pem"
   → keys[SOC_FW_CONTENT_CERT_KEY].fn = "soc_fw_content.pem"

5. 加载密钥
   → keys[TRUSTED_WORLD_KEY].key = EVP_PKEY* (从文件加载)
   → keys[SOC_FW_CONTENT_CERT_KEY].key = EVP_PKEY* (从文件加载)

6. 生成证书
   → cert = &certs[SOC_FW_KEY_CERT]
   → ext = &extensions[SOC_FW_CONTENT_CERT_PK_EXT]
   → key = &keys[ext->attr.key]  // = keys[SOC_FW_CONTENT_CERT_KEY]
   → cert_ext = ext_new_key(..., key->key)  // 提取公钥创建扩展
   → cert_new(..., cert->key)  // = keys[TRUSTED_WORLD_KEY].key (用于签名)
```

---

## 总结

### 三个文件的配合关系

1. **`tbb_key.c`**：
   - 定义所有密钥类型
   - 提供密钥 ID 供其他文件引用

2. **`tbb_ext.c`**：
   - 定义所有扩展类型
   - **通过 `.attr.key` 引用密钥 ID**（对于公钥扩展）
   - 提供扩展 ID 供证书文件引用

3. **`tbb_cert.c`**：
   - 定义所有证书
   - **通过 `.ext[]` 引用扩展 ID**
   - **通过 `.key` 引用密钥 ID**（用于签名和 Subject PK）

### 关键机制

1. **注册机制**：通过 `REGISTER_*` 宏将静态数组注册为全局变量
2. **ID 引用**：通过枚举 ID 在不同数组间建立引用关系
3. **运行时解析**：`main.c` 通过 ID 索引访问对应的密钥、扩展、证书

### 优势

- ✅ **模块化**：三个文件各司其职，职责清晰
- ✅ **可扩展**：可以轻松添加新的密钥、扩展、证书
- ✅ **类型安全**：通过枚举 ID 避免字符串匹配错误
- ✅ **灵活性**：支持平台自定义扩展（通过 `PLAT_REGISTER_*` 宏）

---

**参考代码位置**：
- 注册宏：`tools/cert_create/include/*.h`
- 初始化函数：`tools/cert_create/src/cert.c`, `key.c`, `ext.c`
- 证书生成：`tools/cert_create/src/main.c` 第 473-560 行
