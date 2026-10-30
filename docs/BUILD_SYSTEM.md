> **⚠️ Historical design document.** This predates the implementation and describes original *intent*, not the shipped system. For the as-built architecture see the [Architecture Decision Records](adr/) and the [CHANGELOG](../CHANGELOG.md); where they disagree with this file, they are correct.

# QuantumOS Build System

## Cross-Compiler Requirements

### Supported Architectures
- **x86_64** - Primary development target
- **ARM64** - Mobile/embedded systems
- **RISC-V** - Open hardware platforms

### Toolchain Setup
```bash
# Install cross-compilers (Ubuntu/Debian)
sudo apt-get install gcc-x86-64-elf gcc-aarch64-elf gcc-riscv64-elf

# Or use pre-built toolchains
wget https://developer.arm.com/-/media/Files/downloads/gnu-a/11.2-2022.02/binrel/gcc-arm-11.2-2022.02-x86_64-aarch64-elf.tar.xz
```

## Makefile Structure

### Root Makefile
```makefile
# QuantumOS Root Makefile
ARCH ?= x86_64
BUILD_DIR = build/$(ARCH)
KERNEL_DIR = kernel
MSI_DIR = msi
SERVICES_DIR = services

# Default target
.PHONY: all clean kernel msi services

all: kernel msi services

kernel:
	$(MAKE) -C $(KERNEL_DIR) ARCH=$(ARCH) BUILD_DIR=$(BUILD_DIR)/kernel

msi:
	$(MAKE) -C $(MSI_DIR) ARCH=$(ARCH) BUILD_DIR=$(BUILD_DIR)/msi

services:
	$(MAKE) -C $(SERVICES_DIR) ARCH=$(ARCH) BUILD_DIR=$(BUILD_DIR)/services

clean:
	rm -rf build/
	$(MAKE) -C $(KERNEL_DIR) clean
	$(MAKE) -C $(MSI_DIR) clean
	$(MAKE) -C $(SERVICES_DIR) clean

# Development targets
.PHONY: run qemu debug

run: kernel
	qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel/kernel.elf

debug: kernel
	qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel/kernel.elf -s -S

# CI targets
.PHONY: test lint format

test:
	$(MAKE) -C $(KERNEL_DIR) test
	$(MAKE) -C $(MSI_DIR) test

lint:
	clang-format -i $(shell find . -name "*.c" -o -name "*.h")

format:
	clang-format -i $(shell find . -name "*.c" -o -name "*.h")
```

### Kernel Makefile
```makefile
# kernel/Makefile
CC = $(ARCH)-elf-gcc
LD = $(ARCH)-elf-ld
OBJCOPY = $(ARCH)-elf-objcopy
CFLAGS = -Wall -Wextra -Werror -nostdlib -ffreestanding -mno-red-zone -mno-mmx -mno-sse -mno-sse2
CFLAGS += -fno-omit-frame-pointer -fno-stack-protector
CFLAGS += -Iinclude -I../msi/include
LDFLAGS = -nostdlib -z max-page-size=0x1000

# Source files
KERNEL_SOURCES = $(shell find src -name "*.c")
ASSEMBLY_SOURCES = $(shell find src -name "*.S")
OBJECTS = $(KERNEL_SOURCES:src/%.c=$(BUILD_DIR)/%.o) $(ASSEMBLY_SOURCES:src/%.S=$(BUILD_DIR)/%.o)

.PHONY: all clean test

all: $(BUILD_DIR)/kernel.elf

$(BUILD_DIR)/kernel.elf: $(OBJECTS) link.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -T link.ld -o $@ $(OBJECTS)

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build/

test:
	# Add kernel tests here
```

## Linker Script

### x86_64 Link Script
```ld
/* kernel/link.ld */
ENTRY(_start)

SECTIONS
{
    . = 1M;  /* Kernel starts at 1MB */
    
    .text : {
        *(.text .text.*)
    }
    
    .rodata : {
        *(.rodata .rodata.*)
    }
    
    .data : {
        *(.data .data.*)
    }
    
    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }
    
    . = ALIGN(4K);
    __end = .;
}
```

## CI/CD Pipeline

### GitHub Actions
```yaml
# .github/workflows/build.yml
name: Build QuantumOS

on: [push, pull_request]

jobs:
  build:
    strategy:
      matrix:
        arch: [x86_64, arm64, riscv64]
    
    runs-on: ubuntu-latest
    
    steps:
    - uses: actions/checkout@v3
    
    - name: Install dependencies
      run: |
        sudo apt-get update
        sudo apt-get install gcc-x86-64-elf gcc-aarch64-elf gcc-riscv64-elf qemu-system-x86 qemu-system-arm
    
    - name: Build kernel
      run: make kernel ARCH=${{ matrix.arch }}
    
    - name: Build MSI
      run: make msi ARCH=${{ matrix.arch }}
    
    - name: Build services
      run: make services ARCH=${{ matrix.arch }}
    
    - name: Run tests
      run: make test ARCH=${{ matrix.arch }}
    
    - name: Upload artifacts
      uses: actions/upload-artifact@v3
      with:
        name: quantumos-${{ matrix.arch }}
        path: build/${{ matrix.arch }}/
```

## Development Workflow

### Local Development
```bash
# Setup development environment
make setup  # Install cross-compilers

# Build for default architecture (x86_64)
make all

# Build for specific architecture
make all ARCH=arm64

# Run in QEMU
make run

# Debug with GDB
make debug
# In another terminal:
# gdb-multiarch build/x86_64/kernel/kernel.elf
# (gdb) target remote localhost:1234
# (gdb) break _start
# (gdb) continue
```

### Testing Framework
```bash
# Run unit tests
make test

# Run integration tests
make test-integration

# Run performance benchmarks
make benchmark

# Code coverage
make coverage
```

## Documentation and releases (as built)

The targets this section once listed (`make docs`, `make diagrams`, `make user-docs`,
`make version`, `make release`, `make changelog`, `make dist`, `make upload`) were never
implemented. What exists: `make help` prints the real target list; the ADRs under
`docs/adr/` and the subsystem docs are written by hand with the change that lands them;
a release is a `VERSION` bump, a `CHANGELOG.md` entry, a tag, and the GitHub Actions
release workflow that builds the ISO and publishes `quantumos-host-tools` to PyPI
(see `CONTRIBUTING.md`).

## Performance Monitoring

### Build Metrics
- **Build Time**: Track compilation time across architectures
- **Binary Size**: Monitor kernel and service sizes
- **Memory Usage**: Track build memory consumption
- **Test Coverage**: Ensure comprehensive test coverage

### Continuous Integration
- **Automated Testing**: Run tests on every commit
- **Static Analysis**: Code quality checks
- **Security Scanning**: Vulnerability detection
- **Performance Regression**: Benchmark comparisons
