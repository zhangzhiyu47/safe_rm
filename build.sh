#!/bin/bash
set -e

echo "Building SafeRm (Rust version)..."

# 检查Rust是否安装
if ! command -v cargo &> /dev/null; then
    echo "Error: Rust/Cargo not found. Please install Rust first:"
    echo "  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"
    exit 1
fi

# 构建Release版本
echo "Building release binaries..."
cargo build --release

# 创建安装目录
mkdir -p bin

# 复制二进制文件
cp target/release/safe_rm bin/
cp target/release/restore bin/
cp target/release/zrestore bin/

echo "Build complete! Binaries are in the 'bin/' directory."
echo ""
echo "To install system-wide, run:"
echo "  sudo cp bin/* /usr/local/bin/"
echo ""
echo "Or install to user directory:"
echo "  mkdir -p ~/.local/bin && cp bin/* ~/.local/bin/"
