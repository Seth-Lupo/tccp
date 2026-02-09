# TCCP - Tufts Cluster Command Prompt

A high-performance C++ CLI tool for managing SLURM jobs on HPC clusters. Works with any project/repo - just register it with `tccp register .`

**Features:**
- Interactive CLI with multi-factor authentication support
- Secure credential management (platform keyring integration)
- File synchronization respecting .gitignore patterns
- SLURM job submission and monitoring
- Cross-platform (Windows, macOS, Linux)
- Statically linked, zero runtime dependencies

## Quick Start

### Installation

Download the binary for your platform from [releases](https://github.com/sethlupo/tccp/releases):
- `tccp-linux-x64` - Linux x64
- `tccp-macos-arm64` - macOS ARM64
- `tccp-windows-x64.exe` - Windows

Or build from source (see below).

### Setup

1. Initialize global config:
   ```
   tccp init
   ```
   Edit `~/.tccp/config.yaml` with your cluster details.

2. Register a project:
   ```
   cd /path/to/your/project
   tccp register .
   ```
   This creates `tccp.yaml` in your project directory.

3. Start using TCCP:
   ```
   tccp connect          # Connect to cluster
   tccp sync             # Sync files
   tccp run job_name     # Submit a job
   tccp jobs             # List running jobs
   ```

## Usage

### Interactive Mode
```
tccp
```

### Single Command Mode
```
tccp connect
tccp sync
tccp run training --epochs 10
tccp jobs
tccp cancel JOB_ID
```

### Common Commands

**Connection:**
- `connect` - Establish cluster connection
- `disconnect` - Close connection
- `status` - Show connection status

**Files:**
- `sync` - Upload project files to cluster
- `download <path>` - Download file from cluster
- `ls [path]` - List remote files

**Jobs:**
- `run <job>` - Submit a SLURM job
- `jobs` - List running SLURM jobs
- `logs <job_id>` - View job logs
- `cancel <job_id>` - Cancel a job

**Shell:**
- `shell` - Interactive shell on cluster
- `exec <command>` - Run single command on cluster

## Configuration

### Global Config (~/.tccp/config.yaml)

```yaml
cluster:
  host: login.example.edu
  user: username
  password: password
  email: user@example.edu
  timeout: 30
  control_persist: 600

proxy:
  enabled: false
  host: proxy.example.edu
  user: username
  password: null

slurm:
  partition: cpu
  time: 00:30:00
  nodes: 1
  cpus_per_task: 4
  memory: 8G
  gpu_type: ""
  gpu_count: 0
  mail_type: NONE

modules: []
duo_auto: "1"
```

### Project Config (tccp.yaml)

```yaml
name: myproject
description: My research project

remote_dir: ~/tccp-projects/myproject

environment:
  PYTHON_VERSION: "3.12"

sync:
  - "*.py"
  - "src/"
  - "!__pycache__/"
  - "!*.pyc"

jobs:
  train:
    script: train.py
    description: Train the model
  inference:
    script: inference.py
    description: Run inference
```

## Building from Source

### Requirements
- CMake 3.20+
- C++17 compiler (clang, gcc, or MSVC)
- vcpkg

### Local Development (macOS/Linux)

```bash
git clone https://github.com/sethlupo/tccp.git
cd tccp

# Setup vcpkg
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh

# Configure (ARM Mac)
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=arm64-osx \
  -DTCCP_STATIC_BUILD=ON \
  -DCMAKE_BUILD_TYPE=Release

# Build
make -C build

# Test
./build/tccp --version
```

### Windows

```bash
# Setup vcpkg
git clone https://github.com/microsoft/vcpkg.git
.\vcpkg\bootstrap-vcpkg.bat

# Configure
cmake -B build -S . `
  -DCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DTCCP_STATIC_BUILD=ON `
  -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build --config Release
```

## Project Structure

```
tccp/
├── src/
│   ├── core/           # Configuration, credentials, types
│   ├── ssh/            # SSH connection & authentication
│   ├── managers/       # File sync, jobs, batch operations
│   ├── cli/            # Command-line interface
│   └── util/           # Utilities
├── CMakeLists.txt      # Build configuration
├── vcpkg.json          # Dependencies
└── .github/workflows/  # CI/CD
```

## Architecture

**Core Components:**

1. **SSH Layer** (`src/ssh/`)
   - ExpectMatcher: Pattern matching on SSH channels (pexpect replacement)
   - Authenticators: Password, SSH key, and Duo 2FA support
   - SessionManager: Multi-hop proxy connections
   - SSHConnection: High-level SSH API

2. **Managers** (`src/managers/`)
   - GitignoreParser: .gitignore pattern matching
   - SyncManager: File synchronization
   - JobManager: SLURM job management
   - BatchManager: Batch operations

3. **CLI** (`src/cli/`)
   - BaseCLI: Command registration and state management
   - TCCPCLI: Main CLI with command modules
   - Commands: Modular command implementations

4. **Core** (`src/core/`)
   - Config: YAML configuration loading
   - CredentialManager: Secure credential storage (Keychain/CredentialManager/libsecret)
   - Types: Data structures and result types

## Performance

- **Binary size:** 5.1 MB (static, no dependencies)
- **Startup:** ~30-50ms (vs ~200ms Python version)
- **Memory:** ~5-10MB baseline
- **File sync:** Parallel uploads with thread pool

## Security

- Passwords stored in system keyring (not config files)
- SSH key authentication support
- Support for Duo 2FA on clusters
- Secure file transfer via SSH
- No plain-text credentials on disk

## Testing

Run tests:
```bash
make -C build test
```

Cross-platform testing via GitHub Actions on:
- Ubuntu (x64)
- Windows (x64)
- macOS (ARM64)

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Submit a pull request

## License

MIT License - see LICENSE file

## Support

For issues and feature requests, visit the [GitHub issues page](https://github.com/sethlupo/tccp/issues).

---

**Development Progress:**
- Phase 1: Build system & core - COMPLETE
- Phase 2: SSH layer - COMPLETE
- Phase 3: Managers - COMPLETE
- Phase 4: CLI & commands - COMPLETE
- Phase 5: CI/CD & testing - IN PROGRESS
