# OP-TEE 单独编译指南

## 一、OP-TEE 项目结构

OP-TEE 是一个**独立的项目**，有自己的源码仓库和编译系统。它不包含在 ATF 中。

```
OP-TEE 项目结构：
├── optee_os/          # OP-TEE OS 核心（需要编译）
├── optee_client/     # OP-TEE Client 库
├── optee_test/       # OP-TEE 测试套件
└── build/            # 编译脚本（可选）
```

## 二、获取 OP-TEE 源码

### 方法 1：克隆官方仓库

```bash
# 创建 OP-TEE 工作目录
mkdir -p ~/optee
cd ~/optee

# 克隆 OP-TEE OS 核心
git clone https://github.com/OP-TEE/optee_os.git
cd optee_os
```

### 方法 2：使用 manifest（推荐）

```bash
# 安装 repo 工具
mkdir -p ~/bin
curl https://storage.googleapis.com/git-repo-downloads/repo > ~/bin/repo
chmod a+x ~/bin/repo
export PATH=~/bin:$PATH

# 初始化 OP-TEE 项目
mkdir -p ~/optee
cd ~/optee
repo init -u https://github.com/OP-TEE/manifest.git -m default.xml
repo sync
```

## 三、编译 OP-TEE OS

### 基本编译命令

```bash
cd ~/optee/optee_os

# 基本编译（需要指定平台）
make PLATFORM=<platform> CROSS_COMPILE=<toolchain-prefix>
```

### 常用平台和工具链

| 平台 | PLATFORM 值 | 工具链示例 |
|------|------------|-----------|
| FVP | `fvp` | `aarch64-none-elf-` |
| QEMU | `qemu_v8` | `aarch64-linux-gnu-` |
| Raspberry Pi 3 | `rpi3` | `aarch64-linux-gnu-` |
| HiKey | `hikey` | `aarch64-linux-gnu-` |
| STM32MP1 | `stm32mp1` | `arm-linux-gnueabihf-` |

### 编译示例

#### 示例 1：编译 FVP 平台

```bash
cd ~/optee/optee_os

# 清理
make PLATFORM=fvp clean

# 编译
make PLATFORM=fvp \
     CROSS_COMPILE=aarch64-none-elf- \
     CFG_TEE_CORE_LOG_LEVEL=3

# 编译产物
ls -lh out/arm-plat-fvp/core/tee.bin
ls -lh out/arm-plat-fvp/core/tee.elf
```

#### 示例 2：编译 QEMU 平台

```bash
cd ~/optee/optee_os

make PLATFORM=qemu_v8 \
     CROSS_COMPILE=aarch64-linux-gnu- \
     CFG_TEE_CORE_LOG_LEVEL=3
```

### 编译选项说明

```bash
# 基本选项
PLATFORM=<platform>          # 目标平台（必需）
CROSS_COMPILE=<prefix>        # 交叉编译工具链前缀（必需）

# 配置选项
CFG_TEE_CORE_LOG_LEVEL=3     # 日志级别（0-4，默认 1）
CFG_TEE_CORE_DEBUG=y         # 启用调试
CFG_ARM64_core=y             # 使用 AArch64（默认）
CFG_ARM32_core=y             # 使用 AArch32

# 编译选项
-jN                          # 并行编译（N 为线程数）
O=<build-dir>                # 指定输出目录（默认 out/）
```

## 四、编译产物

编译成功后，生成以下文件：

```
out/arm-plat-<platform>/
├── core/
│   ├── tee.bin              # OP-TEE 二进制镜像（用于 BL32）
│   ├── tee.elf              # OP-TEE ELF 文件
│   ├── tee.dmp              # 反汇编文件
│   └── tee.map              # 链接映射文件
├── ta/
│   └── <uuid>/              # 可信应用（TA）
└── export-ta_arm64/         # TA 导出目录
```

### 关键文件说明

- **`tee.bin`**：这是用于 ATF 的 BL32 镜像
- **`tee.elf`**：ELF 格式，用于调试
- **`tee.map`**：链接映射，用于分析内存布局

## 五、与 ATF 集成

### 方法 1：使用预编译的 OP-TEE 镜像

```bash
cd ~/arm-trusted-firmware

# 编译 ATF，指定 OP-TEE 镜像路径
make PLAT=fvp \
     SPD=opteed \
     BL32=~/optee/optee_os/out/arm-plat-fvp/core/tee.bin \
     all
```

### 方法 2：在 ATF 中自动编译（如果支持）

某些平台支持在 ATF 编译时自动编译 OP-TEE：

```bash
# 需要设置 OP-TEE 源码路径
make PLAT=fvp \
     SPD=opteed \
     OP_TEE_PATH=~/optee/optee_os \
     all
```

## 六、完整编译流程示例

### FVP 平台完整流程

```bash
# 1. 编译 OP-TEE
cd ~/optee/optee_os
make PLATFORM=fvp \
     CROSS_COMPILE=aarch64-none-elf- \
     CFG_TEE_CORE_LOG_LEVEL=3

# 2. 编译 ATF（使用编译好的 OP-TEE）
cd ~/arm-trusted-firmware
make PLAT=fvp \
     SPD=opteed \
     BL32=~/optee/optee_os/out/arm-plat-fvp/core/tee.bin \
     all

# 3. 生成 FIP
make PLAT=fvp \
     SPD=opteed \
     BL32=~/optee/optee_os/out/arm-plat-fvp/core/tee.bin \
     BL33=<path-to-u-boot.bin> \
     fip
```

## 七、常见问题

### 问题 1：找不到工具链

```bash
# 安装 ARM 工具链（Ubuntu/Debian）
sudo apt-get install gcc-aarch64-linux-gnu

# 或使用 Linaro 工具链
wget https://releases.linaro.org/components/toolchain/binaries/latest/aarch64-linux-gnu/gcc-linaro-aarch64-linux-gnu.tar.xz
tar xf gcc-linaro-aarch64-linux-gnu.tar.xz
export PATH=$PWD/gcc-linaro-aarch64-linux-gnu/bin:$PATH
```

### 问题 2：平台不支持

```bash
# 查看支持的平台
cd ~/optee/optee_os
ls -d core/arch/arm/plat-*/

# 或查看 Makefile
grep -r "PLATFORM" core/arch/arm/plat-*/
```

### 问题 3：编译错误

```bash
# 启用详细输出
make PLATFORM=fvp V=1

# 清理后重新编译
make PLATFORM=fvp clean
make PLATFORM=fvp
```

## 八、编译脚本示例

创建 `build_optee.sh`：

```bash
#!/bin/bash

# 配置
PLATFORM=${1:-fvp}
CROSS_COMPILE=${CROSS_COMPILE:-aarch64-none-elf-}
OP_TEE_DIR=${OP_TEE_DIR:-~/optee/optee_os}

echo "Building OP-TEE for platform: $PLATFORM"

cd $OP_TEE_DIR || exit 1

# 清理
make PLATFORM=$PLATFORM clean

# 编译
make PLATFORM=$PLATFORM \
     CROSS_COMPILE=$CROSS_COMPILE \
     CFG_TEE_CORE_LOG_LEVEL=3 \
     -j$(nproc)

# 检查结果
if [ -f "out/arm-plat-$PLATFORM/core/tee.bin" ]; then
    echo "✓ OP-TEE compiled successfully!"
    ls -lh out/arm-plat-$PLATFORM/core/tee.bin
    echo ""
    echo "To use with ATF:"
    echo "  make PLAT=$PLATFORM SPD=opteed BL32=$OP_TEE_DIR/out/arm-plat-$PLATFORM/core/tee.bin all"
else
    echo "✗ OP-TEE compilation failed!"
    exit 1
fi
```

使用方法：
```bash
chmod +x build_optee.sh
./build_optee.sh fvp
```

## 九、验证编译结果

```bash
# 检查文件大小（应该有几 MB）
ls -lh out/arm-plat-fvp/core/tee.bin

# 检查文件类型
file out/arm-plat-fvp/core/tee.bin

# 查看链接信息
readelf -h out/arm-plat-fvp/core/tee.elf

# 查看符号表
nm out/arm-plat-fvp/core/tee.elf | head -20
```

## 十、总结

### 快速开始

```bash
# 1. 克隆源码
git clone https://github.com/OP-TEE/optee_os.git
cd optee_os

# 2. 编译
make PLATFORM=fvp CROSS_COMPILE=aarch64-none-elf-

# 3. 使用编译产物
# tee.bin 就是用于 ATF 的 BL32 镜像
```

### 关键点

1. **OP-TEE 是独立项目**：需要单独编译
2. **生成 `tee.bin`**：这是用于 ATF 的 BL32 镜像
3. **需要指定平台**：`PLATFORM=<platform>`
4. **需要交叉编译工具链**：`CROSS_COMPILE=<prefix>`
5. **与 ATF 集成**：通过 `BL32=tee.bin` 参数

### 下一步

编译好 OP-TEE 后，可以：
1. 在 ATF 中使用：`make PLAT=fvp SPD=opteed BL32=tee.bin all`
2. 运行测试：使用 `optee_test` 项目
3. 开发 TA：使用 `optee_examples` 项目

