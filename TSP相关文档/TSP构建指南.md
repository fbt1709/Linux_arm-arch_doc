# TSP 编译指南

## 一、TSP 编译概述

TSP（Test Secure Payload）是 ATF 内置的测试安全负载，**不需要单独编译**。当你编译 ATF 并指定 `SPD=tspd` 时，TSP 会自动编译为 BL32。

## 二、快速编译 TSP

### 最简单的编译命令

```bash
cd /home/fbt1709/arm-trusted-firmware

# 编译 TSP（使用 FVP 平台）
make PLAT=fvp SPD=tspd all
```

这个命令会：
1. 编译 TSPD（在 BL31 中）
2. **自动编译 TSP**（作为 BL32）
3. 生成 `build/fvp/release/bl32.bin`（这就是 TSP）

## 三、详细编译步骤

### 步骤 1：选择支持 TSP 的平台

支持 TSP 的平台包括：
- `fvp` - Fixed Virtual Platform（推荐，最常用）
- `juno` - Juno 开发板
- `zynqmp` - Xilinx ZynqMP
- `versal` - Xilinx Versal
- 等等...

### 步骤 2：编译命令

```bash
cd /home/fbt1709/arm-trusted-firmware

# 清理之前的编译（可选）
make PLAT=fvp clean

# 编译 TSP
make PLAT=fvp SPD=tspd all
```

### 步骤 3：验证编译结果

```bash
# 检查 TSP 二进制文件
ls -lh build/fvp/release/bl32.bin

# 检查文件信息
file build/fvp/release/bl32.bin

# 查看编译输出目录
ls -lh build/fvp/release/
```

## 四、编译选项说明

### 基本选项

```bash
PLAT=<platform>    # 平台名称（必需）
SPD=tspd           # 启用 TSPD，这会自动编译 TSP（必需）
all                # 编译所有镜像（包括 bl32.bin）
```

### 可选选项

```bash
# 启用调试信息
make PLAT=fvp SPD=tspd DEBUG=1 all

# 指定工具链
make PLAT=fvp SPD=tspd CROSS_COMPILE=aarch64-linux-gnu- all

# 启用异步初始化（可选）
make PLAT=fvp SPD=tspd TSP_INIT_ASYNC=1 all

# 启用非安全中断异步抢占（可选）
make PLAT=fvp SPD=tspd TSP_NS_INTR_ASYNC_PREEMPT=1 all
```

## 五、编译流程说明

```
设置 SPD=tspd
    │
    ▼
Makefile 包含 services/spd/tspd/tspd.mk
    │
    ├─> 设置 NEED_BL32 := yes
    │
    └─> 包含 bl32/tsp/tsp.mk
            │
            ├─> 添加 TSP 源文件到 BL32_SOURCES
            │
            └─> 查找平台特定的 tsp-${PLAT}.mk
                    │
                    ├─> 如果找到：包含平台特定配置
                    └─> 如果没找到：报错 "TSP is not supported on platform"
```

## 六、完整编译示例

### 示例 1：FVP 平台

```bash
cd /home/fbt1709/arm-trusted-firmware

# 编译
make PLAT=fvp SPD=tspd all

# 验证
ls -lh build/fvp/release/bl32.bin
```

### 示例 2：Juno 平台

```bash
cd /home/fbt1709/arm-trusted-firmware

# 编译
make PLAT=juno SPD=tspd all

# 验证
ls -lh build/juno/release/bl32.bin
```

### 示例 3：生成 FIP（包含 TSP）

```bash
# 编译并生成 FIP（需要提供 BL33，如 U-Boot）
make PLAT=fvp SPD=tspd fip BL33=<path-to-u-boot.bin>

# 查看 FIP 内容
tools/fiptool/fiptool info build/fvp/release/fip.bin
```

## 七、编译产物

编译成功后，会生成以下文件：

```
build/fvp/release/
├── bl1.bin          # Boot Loader Stage 1
├── bl2.bin          # Boot Loader Stage 2
├── bl31.bin         # Boot Loader Stage 3（包含 TSPD）
├── bl32.bin         # Boot Loader Stage 32（这就是 TSP！）
├── bl31.elf         # BL31 ELF 文件
├── bl32.elf         # BL32 ELF 文件（TSP）
└── fip.bin          # FIP 镜像（如果生成了）
```

**关键文件：`bl32.bin` 就是编译好的 TSP！**

## 八、常见问题

### 问题 1：平台不支持 TSP

```
Error: TSP is not supported on platform xxx
```

**解决方法：**
```bash
# 检查平台是否有 tsp-${PLAT}.mk 文件
ls plat/*/tsp/tsp-*.mk

# 使用支持 TSP 的平台（如 fvp）
make PLAT=fvp SPD=tspd all
```

### 问题 2：找不到 bl32.bin

**检查步骤：**
```bash
# 1. 确认是否正确设置了 SPD=tspd
make PLAT=fvp SPD=tspd all

# 2. 检查编译输出
ls -lh build/fvp/release/bl32.bin

# 3. 查看编译日志，确认 TSP 是否被编译
make PLAT=fvp SPD=tspd all 2>&1 | grep -i "tsp\|bl32"
```

### 问题 3：编译错误

**解决方法：**
```bash
# 1. 清理后重新编译
make PLAT=fvp clean
make PLAT=fvp SPD=tspd all

# 2. 查看详细错误信息
make PLAT=fvp SPD=tspd all V=1

# 3. 检查平台特定文件是否存在
ls plat/arm/board/fvp/tsp/tsp-fvp.mk
```

## 九、编译脚本示例

创建 `build_tsp.sh`：

```bash
#!/bin/bash

# 配置
PLATFORM=${1:-fvp}  # 默认使用 fvp
CROSS_COMPILE=${CROSS_COMPILE:-aarch64-none-elf-}
ATF_DIR=${ATF_DIR:-/home/fbt1709/arm-trusted-firmware}

echo "Building TSP for platform: $PLATFORM"

cd $ATF_DIR || exit 1

# 清理
make PLAT=$PLATFORM clean

# 编译
make PLAT=$PLATFORM \
     SPD=tspd \
     CROSS_COMPILE=$CROSS_COMPILE \
     all

# 检查结果
if [ -f "build/$PLATFORM/release/bl32.bin" ]; then
    echo "✓ TSP compiled successfully!"
    ls -lh build/$PLATFORM/release/bl32.bin
    echo ""
    echo "TSP binary: build/$PLATFORM/release/bl32.bin"
    echo "TSP ELF:    build/$PLATFORM/release/bl32.elf"
else
    echo "✗ TSP compilation failed!"
    exit 1
fi
```

使用方法：
```bash
chmod +x build_tsp.sh
./build_tsp.sh fvp
```

## 十、验证 TSP 编译结果

### 方法 1：检查文件

```bash
# 检查文件大小（应该有几 KB 到几十 KB）
ls -lh build/fvp/release/bl32.bin

# 检查文件类型
file build/fvp/release/bl32.bin

# 查看 ELF 信息
readelf -h build/fvp/release/bl32.elf
```

### 方法 2：查看符号表

```bash
# 查看 TSP 的符号
nm build/fvp/release/bl32.elf | grep -i tsp

# 查看入口点
readelf -h build/fvp/release/bl32.elf | grep Entry
```

### 方法 3：反汇编（可选）

```bash
# 查看反汇编代码
aarch64-none-elf-objdump -d build/fvp/release/bl32.elf | head -50
```

## 十一、与 TFTF 一起使用

编译好 TSP 后，可以在 TFTF 中使用：

```bash
# 1. 编译 ATF（包含 TSP）
cd /home/fbt1709/arm-trusted-firmware
make PLAT=fvp SPD=tspd all

# 2. 编译 TFTF
cd /home/fbt1709/tf-a-tests
make PLAT=fvp all

# 3. 运行 TFTF 测试（会自动检测 TSP）
# TFTF 会使用编译好的 bl32.bin
```

## 十二、总结

### 最简单的编译命令

```bash
cd /home/fbt1709/arm-trusted-firmware
make PLAT=fvp SPD=tspd all
```

### 关键点

1. **TSP 不需要单独编译**：设置 `SPD=tspd` 会自动编译 TSP
2. **生成 `bl32.bin`**：这就是编译好的 TSP
3. **需要支持 TSP 的平台**：如 `fvp`、`juno` 等
4. **TSP 在 ATF 源码中**：不需要额外下载源码

### 下一步

编译好 TSP 后，可以：
1. 在 TFTF 中测试 TSP 功能
2. 查看 TSP 源码：`bl32/tsp/`
3. 查看 TSPD 源码：`services/spd/tspd/`
4. 运行 TSP 测试用例

