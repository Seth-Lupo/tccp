# Building TCCP

Complete guide to building TCCP from source on Windows, macOS, and Linux.

## Prerequisites

### All Platforms
- CMake 3.20 or later
- C++17 compatible compiler
- Git

### Platform-Specific

**macOS:**
- Xcode Command Line Tools
  ```
  xcode-select --install
  ```

**Linux (Debian/Ubuntu):**
```bash
sudo apt-get update
sudo apt-get install build-essential cmake git
```

**Windows:**
- Visual Studio 2019 or later with C++ support
- Or MinGW-w64

## Quick Setup

### Step 1: Clone and Setup vcpkg

```bash
git clone https://github.com/sethlupo/tccp.git
cd tccp

git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh  # macOS/Linux
# or
.\vcpkg\bootstrap-vcpkg.bat  # Windows
```

### Step 2: Configure CMake

Choose the command for your platform:

**macOS (ARM):**
```bash
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=arm64-osx \
  -DTCCP_STATIC_BUILD=ON \
  -DCMAKE_BUILD_TYPE=Release
```

**macOS (Intel):**
```bash
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-osx \
  -DTCCP_STATIC_BUILD=ON \
  -DCMAKE_BUILD_TYPE=Release
```

**Linux:**
```bash
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-linux \
  -DTCCP_STATIC_BUILD=ON \
  -DCMAKE_BUILD_TYPE=Release
```

**Windows:**
```powershell
cmake -B build -S . `
  -DCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DTCCP_STATIC_BUILD=ON `
  -DCMAKE_BUILD_TYPE=Release
```

### Step 3: Build

**macOS/Linux:**
```bash
make -C build
```

**Windows (with MSVC):**
```bash
cmake --build build --config Release
```

### Step 4: Test

```bash
./build/tccp --version
./build/tccp help
```

## Build Options

### CMake Variables

- `TCCP_STATIC_BUILD` (ON/OFF, default ON)
  - Static linking of all dependencies
  - Larger binary size, no runtime dependencies

- `CMAKE_BUILD_TYPE` (Debug/Release, default Release)
  - Debug: Includes symbols, larger binary, easier debugging
  - Release: Optimized, smaller binary, suitable for distribution

### Dynamic Linking (Optional)

For smaller binary size with dynamic linking:

```bash
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=arm64-osx \
  -DTCCP_STATIC_BUILD=OFF \
  -DCMAKE_BUILD_TYPE=Release
```

**Note:** Dynamic linking requires runtime libraries on deployment machine.

## Vcpkg Triplets

Common triplets for different platforms:

| Platform | Triplet | Notes |
|----------|---------|-------|
| macOS ARM64 (native) | arm64-osx | M1/M2/M3 Macs |
| macOS Intel | x64-osx | Intel Macs (older) |
| Linux x64 | x64-linux | Most Linux systems |
| Linux ARM64 | arm64-linux | Raspberry Pi, ARM servers |
| Windows x64 (static) | x64-windows-static | Recommended |
| Windows x64 (dynamic) | x64-windows | With DLL dependencies |

## Clean Build

To rebuild from scratch:

```bash
rm -rf build
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=arm64-osx \
  -DTCCP_STATIC_BUILD=ON \
  -DCMAKE_BUILD_TYPE=Release
make -C build
```

## Development Workflow

### Local Testing

```bash
# Build
make -C build

# Run directly from build directory
./build/tccp --help
./build/tccp --version

# Run interactive mode
./build/tccp
```

### With Ccache (Linux/macOS)

For faster incremental builds:

```bash
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=arm64-osx \
  -DTCCP_STATIC_BUILD=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
```

### Verbose Output

For debugging build issues:

```bash
make -C build VERBOSE=1
# or
cmake --build build --verbose
```

## Troubleshooting

### CMake Not Found

Make sure CMake is in your PATH. On macOS with Homebrew:
```bash
brew install cmake
```

### Vcpkg Bootstrap Fails

On macOS/Linux, ensure execute permissions:
```bash
chmod +x ./vcpkg/bootstrap-vcpkg.sh
./vcpkg/bootstrap-vcpkg.sh
```

### Missing Dependencies During Build

Delete build directory and reconfigure:
```bash
rm -rf build
cmake -B build -S . ...
make -C build
```

### Binary Won't Run

Check for missing libraries (dynamic build only):
```bash
ldd ./build/tccp       # Linux
otool -L ./build/tccp  # macOS
```

For static builds, verify no dynamic dependencies:
```bash
file ./build/tccp
# Should show: statically linked
```

## GitHub Actions CI/CD

The project uses GitHub Actions to automatically build binaries for:
- Linux x64
- macOS ARM64
- Windows x64

Binaries are published as release artifacts and available for download.

## Manual Release Build

To create a release build for distribution:

```bash
# Clean build with optimizations
rm -rf build
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=arm64-osx \
  -DTCCP_STATIC_BUILD=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG"

make -C build -j$(nproc)

# Strip symbols to reduce size
strip build/tccp

# Verify
ls -lh build/tccp
file build/tccp
./build/tccp --version
```

## Support

For build issues:
1. Check CMake output for error messages
2. Verify vcpkg bootstrap completed successfully
3. Ensure correct triplet for your platform
4. See [GitHub Issues](https://github.com/sethlupo/tccp/issues) for known issues
