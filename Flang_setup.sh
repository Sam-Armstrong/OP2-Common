# Run everything from your WSL home, NOT from /mnt/c — /mnt/c is a 5-10x build slowdown.
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build python3 clang lld git

# 1. Get and build LLVM + MLIR + Flang (this takes ~30-60 min, ~15 GB disk)
cd ~
git clone --depth 1 https://github.com/llvm/llvm-project.git
cd llvm-project
cmake -S llvm -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_ENABLE_PROJECTS="mlir;flang" \
    -DLLVM_TARGETS_TO_BUILD=host \
    -DCMAKE_INSTALL_PREFIX=$HOME/.local/llvm \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DLLVM_USE_LINKER=lld \
    -DLLVM_ENABLE_ASSERTIONS=OFF \
    -DLLVM_ENABLE_RTTI=ON
cmake --build build --target install -j"$(nproc)"

# 2. Build op2-flang-scan against that install
cd /mnt/c/repos/OP2-Common/translator-v2/flang-scan
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH=$HOME/.local/llvm
cmake --build build

# 3. Verify
ls -l build/op2-flang-scan
echo 'program p; end program' | build/op2-flang-scan --stdin --path demo.f90
