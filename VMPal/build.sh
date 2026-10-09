#!/bin/zsh
# Builds DXMT as VMPal ships it: LLVM 15.0.7 first, without CPU backends, as DXMT's shader compiler links it, then
# DXMT's native library with Meson. Usage: VMPal/build.sh [folder for the builds, build-vmpal by default]
set -e
source=${0:A:h:h}
work=${1:-$source/build-vmpal}
llvm_version=15.0.7
llvm_sha256=8b5fcb24b4128cf04df1b0b9410ce8b1a729cb3c544e6da885d234280dedeac6
export MACOSX_DEPLOYMENT_TARGET=26.0
mkdir -p $work

if [[ ! -f $work/prefix/lib/libLLVMCore.a ]]; then
    archive=$work/llvm-project-$llvm_version.src.tar.xz
    [[ -f $archive ]] || curl -sfL -o $archive https://github.com/llvm/llvm-project/releases/download/llvmorg-$llvm_version/llvm-project-$llvm_version.src.tar.xz
    [[ $(shasum -a 256 $archive | awk '{print $1}') == $llvm_sha256 ]] || { echo "LLVM's archive doesn't match its SHA-256"; exit 1 }
    rm -rf $work/llvm-project-$llvm_version.src
    tar -xf $archive -C $work llvm-project-$llvm_version.src/llvm llvm-project-$llvm_version.src/cmake llvm-project-$llvm_version.src/third-party
    cmake -S $work/llvm-project-$llvm_version.src/llvm -B $work/llvm -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=$work/prefix -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET \
        -DLLVM_TARGETS_TO_BUILD="" -DLLVM_BUILD_TOOLS=Off -DLLVM_INCLUDE_TESTS=Off -DLLVM_INCLUDE_BENCHMARKS=Off \
        -DLLVM_INCLUDE_EXAMPLES=Off -DLLVM_ENABLE_ZSTD=Off -DLLVM_ENABLE_ZLIB=Off -DLLVM_ENABLE_LIBXML2=Off \
        -DLLVM_ENABLE_TERMINFO=Off -DLLVM_VERSION_PRINTER_SHOW_HOST_TARGET_INFO=Off > $work/llvm.log
    ninja -C $work/llvm install >> $work/llvm.log
fi

rm -rf $work/dxmt
meson setup $work/dxmt $source --buildtype=release -Dnative_llvm_path=$work/prefix -Dwine_builtin_dll=false > $work/dxmt.log
ninja -C $work/dxmt >> $work/dxmt.log
echo $work/dxmt/src/dxmt-native/libdxmt-native.dylib
