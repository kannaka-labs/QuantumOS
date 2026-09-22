# QuantumOS Kernel Makefile

# Configuration
ARCH ?= x86_64
BUILD_DIR = build/$(ARCH)
KERNEL_DIR = kernel

# Cross-compiler detection: try x86_64-elf-gcc first, fall back to system gcc
# The cross-compiler is preferred but not available in all environments (e.g., Ubuntu 24.04)
CROSS_CC := $(shell which $(ARCH)-elf-gcc 2>/dev/null)
ifdef CROSS_CC
    CC = $(ARCH)-elf-gcc
    LD = $(ARCH)-elf-ld
    OBJCOPY = $(ARCH)-elf-objcopy
    OBJDUMP = $(ARCH)-elf-objdump
    AR = $(ARCH)-elf-ar
else
    # Fall back to system GCC with appropriate flags for freestanding code
    CC = gcc
    LD = ld
    OBJCOPY = objcopy
    OBJDUMP = objdump
    AR = ar
endif
GDB = gdb-multiarch

# Compiler flags
# -MMD -MP: emit header dependency files next to each object, so an edit to
# any kernel header rebuilds every .c that includes it. Without this, a
# struct-layout change (e.g. growing process_t) leaves stale objects compiled
# against the OLD layout — translation units then disagree about field
# offsets and sizeof, which manifests as wild memory corruption at runtime
# (found live in epic #62 phase 2). CI builds clean and never sees it; local
# incremental builds absolutely do.
CFLAGS = -Wall -Wextra -Werror -nostdlib -ffreestanding -mno-red-zone \
         -mno-mmx -mno-sse -mno-sse2 -fno-omit-frame-pointer \
         -fno-stack-protector -fno-pic -fno-pie -mcmodel=kernel \
         -MMD -MP \
         -I$(KERNEL_DIR)/include -I$(KERNEL_DIR)/../msi/include

# Linker flags
LDFLAGS = -nostdlib -z noexecstack -z max-page-size=0x1000 -T $(KERNEL_DIR)/link.ld

# Debug flags
DEBUG_CFLAGS = -g -O0
RELEASE_CFLAGS = -O2 -DNDEBUG

# Build type: debug (default) or release
BUILD_TYPE ?= debug
ifeq ($(BUILD_TYPE),release)
    CFLAGS += $(RELEASE_CFLAGS)
else
    CFLAGS += $(DEBUG_CFLAGS)
endif

# Version string single-sourced from the repo-root VERSION file and injected
# into the kernel banner (kernel/src/main.c). $(strip) drops the trailing
# newline; the 'v' prefix is added here so VERSION stays a bare PEP 440 string
# for the Python package (pyproject.toml reads the same file).
QOS_VERSION := v$(strip $(shell cat VERSION))
CFLAGS += -DQOS_VERSION='"$(QOS_VERSION)"'

# Optional: quantum-lottery scheduler. Off by default — the default build is
# byte-identical round-robin. Enable with `make SCHED_LOTTERY=1`, which turns
# each ready-process pick into a lottery draw from the qseed-mixed generator.
ifdef SCHED_LOTTERY
    CFLAGS += -DSCHED_LOTTERY
endif

# Optional: resonant scheduler (ghostd phase 5). Off by default — the default
# build is byte-identical round-robin. Enable with `make SCHED_RESONANT=1`,
# which routes pick_next through the integer-only fixed-point resonant field
# (Kuramoto order parameter + per-process resonant priority) and prints an
# honest rr-vs-resonant fairness comparison at boot. No FPU is used in the ISR
# (the dormant double-precision port stays unwired). See kernel/src/resonant_fixed.c.
ifdef SCHED_RESONANT
    CFLAGS += -DSCHED_RESONANT
endif

# Source files
# KERNEL_SOURCES captures all .c files in kernel/src/ (including process*.c)
KERNEL_SOURCES = $(wildcard $(KERNEL_DIR)/src/*.c)
# The fixed-point resonant scheduler is only compiled/linked when opted in, so
# the default build does not carry its object at all (default stays round-robin
# and byte-for-byte free of resonant code).
ifndef SCHED_RESONANT
KERNEL_SOURCES := $(filter-out $(KERNEL_DIR)/src/resonant_fixed.c,$(KERNEL_SOURCES))
endif
IPC_SOURCES = $(wildcard $(KERNEL_DIR)/src/ipc/*.c)
RESONANCE_SOURCES = $(wildcard $(KERNEL_DIR)/src/resonance/*.c)
ASSEMBLY_SOURCES = $(wildcard $(KERNEL_DIR)/src/*.S)
# Assembly files compile to *_asm.o to avoid naming collisions with C files
# Embedded user programs: compiled ELF images objcopy'd into linkable
# objects (symbols _binary_<name>_elf_start/_end)
USER_DIR = user
USER_BUILD = $(BUILD_DIR)/user
USER_PROGS = init echo client hbsvc ghostd ghost_test paradoxd paradox_test swarm_svc qsh quantumd kannakad fieldsyncd httpd modbusd quota_test delegation_test subagentd cpu_hog qsv qpud qpu_test agentd
USER_ELF_OBJS = $(USER_PROGS:%=$(USER_BUILD)/%_elf.o)

# libq: the freestanding ring-3 runtime, built as a static archive and linked
# into every user program (the linker pulls only the members a program refers
# to, so non-allocating programs get neither the heap arena nor printf).
LIBQ_DIR = $(USER_DIR)/libq
LIBQ_SRCS = $(wildcard $(LIBQ_DIR)/*.c)
LIBQ_OBJS = $(LIBQ_SRCS:$(LIBQ_DIR)/%.c=$(USER_BUILD)/libq/%.o)
LIBQ_HDRS = $(wildcard $(LIBQ_DIR)/*.h)
LIBQ_A = $(USER_BUILD)/libq.a

OBJECTS = $(KERNEL_SOURCES:$(KERNEL_DIR)/src/%.c=$(BUILD_DIR)/%.o) \
          $(IPC_SOURCES:$(KERNEL_DIR)/src/ipc/%.c=$(BUILD_DIR)/ipc/%.o) \
          $(RESONANCE_SOURCES:$(KERNEL_DIR)/src/resonance/%.c=$(BUILD_DIR)/resonance/%.o) \
          $(ASSEMBLY_SOURCES:$(KERNEL_DIR)/src/%.S=$(BUILD_DIR)/%_asm.o) \
          $(USER_ELF_OBJS) \
          $(BUILD_DIR)/initrd_tar.o

# Auto-generated header dependencies (-MMD). Missing .d files (asm, wrapped
# binaries, first build) are silently ignored.
-include $(OBJECTS:.o=.d)

# Targets
.PHONY: all clean kernel run debug dump test test-list test-coverage ci-smoke ci-smoke-mem256 ci-smoke-mem512 ci-smoke-sched ci-smoke-disk ci-smoke-disk-upgrade ci-smoke-net ci-smoke-http ci-smoke-httpd ci-smoke-quiet ci-smoke-resonant ci-smoke-qseed ci-smoke-swarm ci-smoke-mcp ci-smoke-mcp-gate ci-smoke-qsubmit ci-smoke-society ci-smoke-society-gate ci-smoke-society-agents ci-smoke-society-agents-gate ci-smoke-society3 ci-smoke-society3-gate ci-smoke-society4 ci-smoke-society4-gate ci-smoke-society-agents-n ci-smoke-society-agents-n-gate ci-smoke-iso ci-smoke-kbd ci-smoke-noserial ci-smoke-screen swarm-pingpong qos-agent qos-agent-list qos-claude qos-claude-list

all: kernel

kernel: $(BUILD_DIR)/kernel.elf $(BUILD_DIR)/kernel.elf32

$(BUILD_DIR)/kernel.elf: $(OBJECTS) $(KERNEL_DIR)/link.ld
	@mkdir -p $(dir $@)
	@echo "Linking kernel..."
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)
	@echo "Kernel linked successfully: $@"

# QEMU's -kernel multiboot loader refuses ELF64 images, so produce an
# ELF32 copy for direct boot (all load addresses are < 4 GB). Debugging
# still uses kernel.elf, which carries the 64-bit symbols.
$(BUILD_DIR)/kernel.elf32: $(BUILD_DIR)/kernel.elf
	$(OBJCOPY) -O elf32-i386 $< $@
	@echo "Boot image created: $@"

# --- Embedded user programs -------------------------------------------------
# Freestanding, no CRT, linked at USER_VBASE (0x40000000). Built with the
# system/cross gcc as a normal ET_EXEC ELF64, then wrapped into a linkable
# object so the kernel can carry it and the ELF loader can map it.
USER_CFLAGS = -Wall -Wextra -Werror -nostdlib -ffreestanding -fno-pic -fno-pie \
              -no-pie -static -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
              -fno-stack-protector -fno-asynchronous-unwind-tables -O2

# libq objects. mem.o and str.o define libc-named functions, so they carry the
# scoped anti-self-recursion guard (see mem.c); heap.o/printf.o do not — their
# external memset/memcpy calls are legitimately satisfied by mem.o.
$(USER_BUILD)/libq/%.o: $(LIBQ_DIR)/%.c $(LIBQ_HDRS) $(USER_DIR)/usys.h
	@mkdir -p $(dir $@)
	@echo "Building libq: $<..."
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/libq/mem.o: USER_CFLAGS += -fno-tree-loop-distribute-patterns -fno-builtin
$(USER_BUILD)/libq/str.o: USER_CFLAGS += -fno-tree-loop-distribute-patterns -fno-builtin

$(LIBQ_A): $(LIBQ_OBJS)
	@echo "Archiving libq: $@..."
	$(AR) rcs $@ $(LIBQ_OBJS)

# The archive is appended AFTER $< on the link line: ld resolves left-to-right,
# so the program's undefined symbols must precede the archive that satisfies them.
$(USER_BUILD)/%.elf: $(USER_DIR)/%.c $(USER_DIR)/user.ld $(USER_DIR)/usys.h $(USER_DIR)/ghost.h $(USER_DIR)/paradox.h $(USER_DIR)/sha256.h $(USER_DIR)/swarm.h $(USER_DIR)/qsv_engine.h $(USER_DIR)/qpu_circuit.h $(LIBQ_HDRS) $(LIBQ_A)
	@mkdir -p $(USER_BUILD)
	@echo "Building user program: $<..."
	$(CC) $(USER_CFLAGS) -T $(USER_DIR)/user.ld -o $@ $< $(LIBQ_A)

# Wrap each ELF as an object. Run objcopy from inside the build dir so the
# generated symbols are _binary_<name>_elf_{start,end,size}.
$(USER_BUILD)/%_elf.o: $(USER_BUILD)/%.elf
	cd $(USER_BUILD) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		$*.elf $*_elf.o

# --- Embedded initrd (epic #62 phase 2) --------------------------------------
# A deterministic ustar archive of rootfs/, wrapped like the user ELFs so the
# kernel carries it (symbols _binary_initrd_tar_{start,end}). The GNU tar
# flags pin ordering/ownership/mtime so identical trees give identical bytes.
#
# Phase 3: the archive is built from a STAGING copy of rootfs/ into which the
# compiled /bin programs are placed — programs that live on the filesystem
# and are started by the shell via SYS_SPAWN, NOT embedded in the kernel like
# the service ELFs. First citizen: /bin/hello.
ROOTFS_DIR = rootfs
ROOTFS_FILES = $(shell find $(ROOTFS_DIR) -type f 2>/dev/null)
ROOTFS_STAGE = $(BUILD_DIR)/rootfs-stage
INITRD_BIN_PROGS = hello args libqtest consciousnessd qtop qprobe life agentsub

$(BUILD_DIR)/initrd.tar: $(ROOTFS_FILES) $(INITRD_BIN_PROGS:%=$(USER_BUILD)/%.elf)
	@mkdir -p $(BUILD_DIR)
	@echo "Staging initrd tree ($(ROOTFS_DIR)/ + /bin programs)..."
	rm -rf $(ROOTFS_STAGE)
	mkdir -p $(ROOTFS_STAGE)/bin
	cp -r $(ROOTFS_DIR)/. $(ROOTFS_STAGE)/
	$(foreach p,$(INITRD_BIN_PROGS),cp $(USER_BUILD)/$(p).elf $(ROOTFS_STAGE)/bin/$(p);)
	@echo "Building initrd (ustar)..."
	tar --format=ustar --sort=name --owner=0 --group=0 --numeric-owner \
		--mtime='@0' -C $(ROOTFS_STAGE) -cf $@ .

$(BUILD_DIR)/initrd_tar.o: $(BUILD_DIR)/initrd.tar
	cd $(BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		initrd.tar initrd_tar.o

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/src/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# The kernel banner embeds QOS_VERSION (from the VERSION file, injected via
# CFLAGS). Declare main.o's dependency on VERSION so a version bump rebuilds the
# banner even on an incremental build (a full `make clean` build always does).
$(BUILD_DIR)/main.o: VERSION

# Assembly files compile to *_asm.o to avoid collision with C files of same name
$(BUILD_DIR)/%_asm.o: $(KERNEL_DIR)/src/%.S
	@mkdir -p $(dir $@)
	@echo "Assembling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Console-image boot stub (epic #101): identical to boot_asm.o except the
# multiboot header does not request a video mode, so GRUB boots it in VGA
# text and the kernel runs the on-screen console. Used only by the ISO.
$(BUILD_DIR)/boot_console_asm.o: $(KERNEL_DIR)/src/boot.S
	@mkdir -p $(dir $@)
	@echo "Assembling $< (console image, no video request)..."
	$(CC) $(CFLAGS) -DMB1_TEXT_ONLY -c $< -o $@

CONSOLE_OBJECTS = $(filter-out $(BUILD_DIR)/boot_asm.o,$(OBJECTS)) $(BUILD_DIR)/boot_console_asm.o

$(BUILD_DIR)/kernel-console.elf: $(OBJECTS) $(BUILD_DIR)/boot_console_asm.o $(KERNEL_DIR)/link.ld
	@echo "Linking console-image kernel..."
	$(LD) $(LDFLAGS) -o $@ $(CONSOLE_OBJECTS)

$(BUILD_DIR)/kernel-console.elf32: $(BUILD_DIR)/kernel-console.elf
	$(OBJCOPY) -O elf32-i386 $< $@

$(BUILD_DIR)/ipc/%.o: $(KERNEL_DIR)/src/ipc/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling IPC: $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Resonance subsystem does double-precision math, which the x86_64 ABI
# returns in XMM registers — it needs SSE even though the rest of the
# kernel is compiled without it. boot.S enables SSE (CR0/CR4) before
# kernel_main so these instructions are legal at runtime.
RESONANCE_CFLAGS = $(filter-out -mno-sse -mno-sse2 -mno-mmx,$(CFLAGS)) -msse -msse2

$(BUILD_DIR)/resonance/%.o: $(KERNEL_DIR)/src/resonance/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling Resonance: $<..."
	$(CC) $(RESONANCE_CFLAGS) -c $< -o $@

# Create bootable image (GRUB's multiboot v1 loader also wants ELF32).
# Menu entries backed by TWO kernel images (epic #101): GRUB honours
# the MB1 header's video request over gfxpayload (verified: text/keep
# still produced a linear FB), so the "console" entries boot a
# kernel-console image whose header requests no video mode — GRUB stays
# in VGA text and the kernel runs the scrolling screen console, the only
# interactive display on a laptop with no serial port. The DEFAULT
# console entry boots `quiet`: on the first real-laptop boot the demo
# kernel's narration (timer ticks, service lifecycle) scrolled the
# shell prompt away faster than a human could read it — quiet is the
# usable interactive machine, verbose is the debugging entry. The
# graphical entry boots the video-requesting image: 1024x768 splash +
# live field view, text on COM1.
$(BUILD_DIR)/kernel.iso: $(BUILD_DIR)/kernel.elf32 $(BUILD_DIR)/kernel-console.elf32
	@mkdir -p $(BUILD_DIR)/iso/boot/grub
	@cp $(BUILD_DIR)/kernel.elf32 $(BUILD_DIR)/iso/boot/kernel.elf
	@cp $(BUILD_DIR)/kernel-console.elf32 $(BUILD_DIR)/iso/boot/kernel-console.elf
	@echo "set timeout=3" > $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "set default=0" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "menuentry \"QuantumOS (console)\" {" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    multiboot /boot/kernel-console.elf quiet" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    boot" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "}" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@# The showcase: a CLEAN console (quiet) that ALSO runs the agent-native
	@# end-to-end demo (QPU + field + spawn + delegate) at boot. `agentdemo` is
	@# decoupled from `quiet` precisely so this entry stays clean.
	@echo "menuentry \"QuantumOS (agent-native demo)\" {" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    multiboot /boot/kernel-console.elf quiet agentdemo" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    boot" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "}" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "menuentry \"QuantumOS (console, verbose kernel log)\" {" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    multiboot /boot/kernel-console.elf" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    boot" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "}" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "menuentry \"QuantumOS (graphical wave field)\" {" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    insmod all_video" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    set gfxpayload=1024x768x32" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    multiboot /boot/kernel.elf" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "    boot" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo "}" >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@grub-mkrescue -o $@ $(BUILD_DIR)/iso 2>/dev/null || \
	 grub2-mkrescue -o $@ $(BUILD_DIR)/iso 2>/dev/null || \
	 (echo "ERROR: grub-mkrescue failed or missing (need grub-pc-bin xorriso mtools)"; exit 1)

# Run in QEMU
run: $(BUILD_DIR)/kernel.elf32
	@echo "Starting QEMU..."
	qemu-system-x86_64 -kernel $< -serial stdio -m 128M

run-iso: $(BUILD_DIR)/kernel.iso
	@echo "Starting QEMU with ISO..."
	qemu-system-x86_64 -cdrom $< -serial stdio -m 128M

# Debug with GDB (QEMU boots the ELF32 image; GDB reads 64-bit symbols
# from kernel.elf)
debug: $(BUILD_DIR)/kernel.elf $(BUILD_DIR)/kernel.elf32
	@echo "Starting QEMU in debug mode..."
	qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 -serial stdio -m 128M -s -S &
	@echo "Waiting for GDB connection..."
	@sleep 1
	$(GDB) $< -ex "target remote localhost:1234" -ex "break kernel_main" -ex "continue"

# Dump kernel information
dump: $(BUILD_DIR)/kernel.elf
	@echo "=== Kernel Information ==="
	@echo "Size: $$(wc -c < $<) bytes"
	@echo "Sections:"
	$(OBJDUMP) -h $<
	@echo "=== Disassembly ==="
	$(OBJDUMP) -d $< | head -50
	@echo "=== Symbols ==="
	$(OBJDUMP) -t $< | head -20

# Test directories and files
TEST_DIR = tests
TEST_UNIT_DIR = $(TEST_DIR)/unit
TEST_BUILD_DIR = $(BUILD_DIR)/tests
TEST_SOURCES = $(wildcard $(TEST_UNIT_DIR)/*.c)
TEST_OBJECTS = $(TEST_SOURCES:$(TEST_UNIT_DIR)/%.c=$(TEST_BUILD_DIR)/%.o)

# Test kernel - compiles and runs unit tests
test: kernel $(TEST_BUILD_DIR)/test_runner
	@echo "=== Running QuantumOS Unit Tests ==="
	@echo ""
	@# For kernel tests, we need to link them into a test kernel and run in QEMU
	@# Or run host-compiled tests if available
	@if [ -f $(TEST_BUILD_DIR)/test_runner ]; then \
		echo "Running host-based test runner..."; \
		$(TEST_BUILD_DIR)/test_runner; \
	else \
		echo "Running kernel-integrated tests via QEMU..."; \
		timeout 15s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
			-serial stdio -m 128M -display none -no-reboot 2>&1 | \
			tee $(TEST_BUILD_DIR)/test_output.txt; \
		echo ""; \
		echo "Test output saved to $(TEST_BUILD_DIR)/test_output.txt"; \
		if grep -q "FAIL" $(TEST_BUILD_DIR)/test_output.txt 2>/dev/null; then \
			echo ""; \
			echo "ERROR: Some tests FAILED"; \
			grep "FAIL" $(TEST_BUILD_DIR)/test_output.txt; \
			exit 1; \
		elif grep -q "PASS" $(TEST_BUILD_DIR)/test_output.txt 2>/dev/null; then \
			echo ""; \
			echo "All tests PASSED"; \
		else \
			echo ""; \
			echo "WARNING: No test results found in output"; \
		fi; \
	fi
	@echo ""
	@echo "=== Unit Tests Complete ==="

# Build test runner (host-side tests for non-kernel code)
$(TEST_BUILD_DIR)/test_runner: $(TEST_SOURCES)
	@mkdir -p $(TEST_BUILD_DIR)
	@echo "Note: Host-based test runner requires tests to be compilable without kernel dependencies"
	@# For now, we rely on kernel-integrated tests run via QEMU
	@touch $(TEST_BUILD_DIR)/.tests_checked

# Compile individual test files (for kernel-integrated testing)
$(TEST_BUILD_DIR)/%.o: $(TEST_UNIT_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling test: $<..."
	$(CC) $(CFLAGS) -DTEST_BUILD -c $< -o $@

# Run specific test file
test-%: kernel
	@echo "Running test: $*..."
	@if [ -f $(TEST_UNIT_DIR)/test_$*.c ]; then \
		echo "Test file found: $(TEST_UNIT_DIR)/test_$*.c"; \
		timeout 15s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
			-serial stdio -m 128M -display none -no-reboot 2>&1 | \
			grep -A 100 "test_$*" || echo "Test output not found"; \
	else \
		echo "Test file not found: $(TEST_UNIT_DIR)/test_$*.c"; \
		exit 1; \
	fi

# List available tests
test-list:
	@echo "Available tests:"
	@for f in $(TEST_UNIT_DIR)/test_*.c; do \
		if [ -f "$$f" ]; then \
			name=$$(basename "$$f" .c | sed 's/test_//'); \
			echo "  - $$name ($$f)"; \
		fi; \
	done
	@echo ""
	@echo "Run with: make test-<name>"
	@echo "Run all:  make test"

# Code coverage target
test-coverage: clean
	@echo "=== Building with Coverage Instrumentation ==="
	@mkdir -p $(TEST_BUILD_DIR)
	$(MAKE) CFLAGS="$(CFLAGS) --coverage -fprofile-arcs -ftest-coverage"
	@echo ""
	@echo "=== Running Tests with Coverage ==="
	$(MAKE) test || true
	@echo ""
	@echo "=== Generating Coverage Report ==="
	@if command -v lcov > /dev/null 2>&1; then \
		lcov --capture --directory . --output-file $(TEST_BUILD_DIR)/coverage.info 2>/dev/null || true; \
		lcov --remove $(TEST_BUILD_DIR)/coverage.info '/usr/*' --output-file $(TEST_BUILD_DIR)/coverage.info 2>/dev/null || true; \
		if [ -f $(TEST_BUILD_DIR)/coverage.info ]; then \
			echo "Coverage summary:"; \
			lcov --summary $(TEST_BUILD_DIR)/coverage.info 2>&1 | grep -E "lines|functions" || true; \
			if command -v genhtml > /dev/null 2>&1; then \
				genhtml $(TEST_BUILD_DIR)/coverage.info --output-directory $(TEST_BUILD_DIR)/coverage_html 2>/dev/null; \
				echo ""; \
				echo "HTML report generated at: $(TEST_BUILD_DIR)/coverage_html/index.html"; \
			fi; \
		else \
			echo "Coverage data not generated (freestanding code may not support gcov)"; \
		fi; \
	else \
		echo "lcov not installed. Install with: sudo apt-get install lcov"; \
	fi

# CI Smoke Test - builds and boots kernel, validates boot banner appears
# This is the "one-command" test for new contributors to verify their setup
ci-smoke: kernel
	@echo "=== QuantumOS CI Smoke Test ==="
	@echo ""
	@echo "[1/3] Build verified: $(BUILD_DIR)/kernel.elf exists"
	@test -f $(BUILD_DIR)/kernel.elf || (echo "ERROR: Kernel not built" && exit 1)
	@echo "[2/3] Running QEMU boot test (34 second timeout, shell session piped into the console)..."
	@# The SECOND `ghost` (after the run legs) gates qsh's first-match send_msg
	@# invariant (epic #175): qsh must still hold EXACTLY ONE IPC WRITE cap
	@# (->ghostd) after spawning children — a blanket spawn-channel mint (or any
	@# future change giving qsh a second IPC cap in a lower first-fit slot) would
	@# route this ghost_req to a dead child instead of ghostd and time out. The
	@# ghost-before-run leg alone never exercises that window.
	@# The THIRD `ghost` is a SEPARATE delayed write (ADR-0023): it must reach
	@# the REBORN shell, so it cannot ride the main burst — qsh reads input in
	@# 32-byte cons_read chunks and `exit` terminates mid-batch, so a ghost\n in
	@# the same chunk dies with the old shell. Delivered after the old shell has
	@# processed `exit` (bytes landing in the death->rebirth gap wait in the
	@# kernel RX ring for the reborn reader). Gated on the post-reborn log slice
	@# below.
	@# Then `ghost exit` + a FOURTH `ghost` (ADR-0023 Part-1 integration leg):
	@# restart the TARGET (ghostd) under the LIVING reborn shell — the direction
	@# a qsh exit never exercises. Without the dead-target unlink the shell's
	@# stale lower-slot ghostd cap wins first-match and routes to the dead pid;
	@# only unlink (Part 1) + the Pass-2 re-mint (Part 2) make the last answer
	@# appear. Gated on the post-'GHOSTD: FIELD REBORN' slice below.
	@( printf 'help\nps\nfree\nuptime\ndate\nghost\nqrand\nls\ncat /docs/hello.txt\nrun /bin/hello\nrun /bin/args alpha quantumos\nrun /bin/libqtest\nrun /bin/consciousnessd\nrun /bin/qtop\nrun /bin/life\nghost\nimprint the cat sat on the mat\nimprint pure quantum wave dynamics\nimprint hello little world\nrecall the cxt sxt on thx mxt\nfieldtest\nwrite /data/note ramfs-works\nls /data\nrm /data/note\nsync\nexit\n'; sleep 15; printf 'ghost\n'; sleep 4; printf 'ghost exit\n'; sleep 7; printf 'ghost\n'; sleep 6 ) | \
		timeout 34s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-append agentdemo \
		-serial stdio -m 128M -display none -no-reboot 2>&1 | tee /tmp/qemu-boot.log || true
	@echo ""
	@echo "[3/3] Validating boot output..."
	@if ! grep -q "QuantumOS ready" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: Kernel did not reach 'QuantumOS ready'"; \
		echo "Boot log:"; \
		cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; \
		echo "=== Smoke Test FAILED ==="; \
		exit 1; \
	fi
	@echo "SUCCESS: Kernel booted to idle loop (QuantumOS ready)"
	@# Frame-allocator rover gate (hot-path optimization): the amortized-O(1)
	@# search rover must hand out only distinct, genuinely-free frames and lose
	@# none. Printed only after the self-test's alloc/free invariants pass; a
	@# hint/wrap bug panics the boot here (and would also corrupt later gates).
	@if ! grep -q "PMMROVER: frame allocator distinct + leak-free under the search rover" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: frame-allocator rover gate missing (PMMROVER)"; \
		echo "Boot log:"; \
		cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; \
		echo "=== Smoke Test FAILED ==="; \
		exit 1; \
	fi
	@echo "SUCCESS: frame-allocator rover gate passed (distinct + leak-free allocation)"
	@# Heap-reservation gate (ADR-0021): the kernel heap's backing frames must be
	@# reserved so the allocator can never hand one out. The self-test panics the
	@# boot before this marker if the reservation is missing (revert-confirmed).
	@if ! grep -q "PMMHEAP: heap range reserved; allocator never hands a heap frame" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: heap-reservation gate missing (PMMHEAP)"; \
		echo "Boot log:"; \
		cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; \
		echo "=== Smoke Test FAILED ==="; \
		exit 1; \
	fi
	@echo "SUCCESS: heap-reservation gate passed (heap frames reserved from the PMM)"
	@# Physical-ceiling clamp gate (ADR-0021): the PMM clamps a synthetic > 1 GB
	@# frame count to PMM_MAX_FRAMES; reverting the clamp panics the boot here.
	@grep -q "PMMCLAMP: frame count clamps to the 1 GB identity ceiling" /tmp/qemu-boot.log 2>/dev/null || (echo "ERROR: 1GB-clamp gate missing (PMMCLAMP)"; cat /tmp/qemu-boot.log 2>/dev/null || true; echo "=== Smoke Test FAILED ==="; exit 1)
	@echo "SUCCESS: 1GB-clamp gate passed (frame count clamps to the identity ceiling)"
	@# PMM sizing gate (ADR-0021 PR-3): prove total_frames is DERIVED from the real
	@# multiboot memory map, not a hardcode. At -m 128M QEMU's e820 reports a 128
	@# MB-class usable region (~0x7FE0 frames; the top ~128 KB is reserved), so the
	@# count must land in [0x7F00, 0x8000] (~1 MB tolerance for QEMU e820 skew):
	@# below that rejects a zero/panic parse, above rejects the >1 GB clamp ceiling.
	@grep -q "PMMSIZE=" /tmp/qemu-boot.log 2>/dev/null || (echo "ERROR: PMM sizing marker missing (PMMSIZE)"; cat /tmp/qemu-boot.log 2>/dev/null || true; echo "=== Smoke Test FAILED ==="; exit 1)
	@PMMHEX=$$(grep -oE "PMMSIZE=[0-9A-Fa-f]{16}" /tmp/qemu-boot.log | head -1 | sed 's/PMMSIZE=//'); \
	PMMDEC=$$((0x$$PMMHEX)); \
	if [ "$$PMMDEC" -lt 32512 ] || [ "$$PMMDEC" -gt 32768 ]; then \
		echo "ERROR: PMM frame count $$PMMDEC (0x$$PMMHEX) outside 128 MB-class window [32512,32768] (PMMSIZE)"; \
		cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo "=== Smoke Test FAILED ==="; \
		exit 1; \
	fi; \
	echo "SUCCESS: PMM sizing gate passed (total_frames=0x$$PMMHEX derived from multiboot mmap)"
	@# High-memory gate (ADR-0021 PR-4): on the -m 128M leg there is no RAM above
	@# 128 MB, so the reachability self-test must report the SKIP branch — proving
	@# the guard ran (not a vacuous always-pass). The -m 256M/512M legs
	@# (ci-smoke-mem256/mem512) assert the live "writeback verified" branch instead.
	@grep -q "PMMHIGH: skipped (no RAM above 128 MB on this boot)" /tmp/qemu-boot.log 2>/dev/null || (echo "ERROR: high-memory skip gate missing (PMMHIGH)"; cat /tmp/qemu-boot.log 2>/dev/null || true; echo "=== Smoke Test FAILED ==="; exit 1)
	@echo "SUCCESS: high-memory gate passed (skip path exercised at 128M)"
	@# Residency-storm gate (ADR-0021 PR-4): a 1024-frame drain aimed at the heap
	@# must never surrender a reserved frame. Reverting the heap reservation trips
	@# this (the storm hands out a heap frame and the self-test panics the boot).
	@grep -q "PMMSTORM: heap survives a 1024-frame drain" /tmp/qemu-boot.log 2>/dev/null || (echo "ERROR: residency-storm gate missing (PMMSTORM)"; cat /tmp/qemu-boot.log 2>/dev/null || true; echo "=== Smoke Test FAILED ==="; exit 1)
	@echo "SUCCESS: residency-storm gate passed (heap survives a bulk drain)"
	@# Boot-validation gate (ADR-0021): the kernel is Multiboot1-only; a Multiboot2
	@# magic must be refused. Self-test drives the real predicate with a dummy
	@# info_addr (revert-confirmed: re-accepting MB2 panics the boot).
	@if ! grep -q "MB2REJECT: multiboot2 magic refused, multiboot1 accepted" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: boot-validation gate missing (MB2REJECT)"; \
		echo "Boot log:"; \
		cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; \
		echo "=== Smoke Test FAILED ==="; \
		exit 1; \
	fi
	@echo "SUCCESS: boot-validation gate passed (multiboot2 magic refused)"
	@# HMAC-SHA256 gate (ADR-0019): swarm_svc self-tests the integer HMAC against
	@# RFC 4231 tc2 at boot so the swarm-plane frame MAC agrees byte-for-byte with
	@# the host verifier. A broken HMAC prints "HMAC256: SELF-TEST FAILED" instead
	@# of the OK marker (revert-confirmed), so this presence check reddens.
	@if ! grep -q "HMAC256: self-test OK (RFC4231 tc2)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: HMAC-SHA256 self-test gate missing/failed (HMAC256)"; \
		echo "Boot log:"; \
		cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; \
		echo "=== Smoke Test FAILED ==="; \
		exit 1; \
	fi
	@echo "SUCCESS: HMAC-SHA256 gate passed (RFC 4231 tc2 self-test OK)"
	@# Capability high-water-mark gate (hot-path optimization): the per-gated-
	@# syscall cap lookups scan only [0, cap_hwm) not all 1024 slots. The self-test
	@# proves the hwm covers every allocated slot AND that the lookup honors the
	@# bound; a hint/off-by-one panics the boot before this line.
	@if ! grep -q "CAPHWM: capability lookups bounded by high-water-mark" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capability high-water-mark gate missing (CAPHWM)"; \
		echo "Boot log:"; \
		cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; \
		echo "=== Smoke Test FAILED ==="; \
		exit 1; \
	fi
	@echo "SUCCESS: capability high-water-mark gate passed (bounded lookups, correct coverage)"
	@# Spawn-channel unlink gate (epic #175): when a process dies, spawn-minted
	@# IPC caps TARGETING it are freed — scoped by origin tag AND resource type,
	@# so hand-wired pairs and colliding-resource_id caps survive (the self-test
	@# asserts all three survivors; an over-broad or mis-keyed revoker panics the
	@# boot before this line).
	@if ! grep -q "CAPUNLINK: IPC caps die with their target" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: dead-target IPC unlink gate missing (CAPUNLINK, ADR-0023)"; \
		echo "Boot log:"; \
		cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; \
		echo "=== Smoke Test FAILED ==="; \
		exit 1; \
	fi
	@echo "SUCCESS: spawn-channel unlink gate passed (scoped teardown, survivors intact)"
	@# ELF-loader hardening gate (adversarial bug-hunt of the proc/mem core):
	@# three malformed spawns (bad magic, p_offset overflow, sub-USER_VBASE
	@# p_vaddr) must be rejected with zero frame leak. This line is printed only
	@# after elf_spawn_selftest() passes; without the loader fixes the self-test
	@# panics the boot (leak / OOB-read spawn / kernel-half corruption) and the
	@# boot never reaches here.
	@if ! grep -q "ELFGUARD: malformed spawn rejected, no frame leak" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: ELF-loader hardening gate missing (ELFGUARD)"; \
		echo "Boot log:"; \
		cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; \
		echo "=== Smoke Test FAILED ==="; \
		exit 1; \
	fi
	@echo "SUCCESS: ELF-loader hardening gate passed (malformed spawns rejected, no leak)"
	@# Spawn address-space-leak gate (cross-cutting bug-hunt, resource-leak class):
	@# a spawn that builds a full address space then fails at process_create
	@# (ring-3 reachable by filling the process table) must reclaim it. Printed
	@# only after the self-test confirms no frame leak; without the finalize_user
	@# _process vmspace_destroy fix the boot panics before this line.
	@if ! grep -q "SPAWNLEAK: failed spawn reclaimed address space (no leak)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: spawn address-space-leak gate missing (SPAWNLEAK)"; \
		echo "Boot log:"; \
		cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; \
		echo "=== Smoke Test FAILED ==="; \
		exit 1; \
	fi
	@echo "SUCCESS: spawn address-space-leak gate passed (failed spawn reclaimed)"
	@# Argv-residue disclosure gate (cross-cutting bug-hunt, uninit-copyout class):
	@# the reused command-line parse buffer must not copy a prior spawn's argv into
	@# a later child's readable args page. Printed only after the self-test
	@# confirms no residue; without the parse_cmdline zero the boot panics here.
	@if ! grep -q "ARGVLEAK: reused spawn buffer carries no stale argv residue" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: argv-residue disclosure gate missing (ARGVLEAK)"; \
		echo "Boot log:"; \
		cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; \
		echo "=== Smoke Test FAILED ==="; \
		exit 1; \
	fi
	@echo "SUCCESS: argv-residue disclosure gate passed (no stale argv leak)"
	@# Watchdog give-up gate (cross-cutting bug-hunt, state-machine class): when a
	@# service's restart budget is exhausted, the confirmed-bad process must be
	@# reclaimed, not left RUNNING (a hung service was leaked). Printed only after
	@# the service self-test proves the process is destroyed on the give-up edge;
	@# without the fix the assert trips and the boot panics.
	@if ! grep -q "SVCGIVEUP: restart budget exhausted reclaims the process" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: watchdog give-up reclaim gate missing (SVCGIVEUP)"; \
		echo "Boot log:"; \
		cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; \
		echo "=== Smoke Test FAILED ==="; \
		exit 1; \
	fi
	@echo "SUCCESS: watchdog give-up reclaim gate passed (hung service reclaimed)"
	@# DNS answer-binding gate (adversarial bug-hunt of the untrusted-input
	@# surface): dns_parse must reject a spoofed-source or wrong-dest-port DNS
	@# reply (on-link SYS_RESOLVE poisoning), not just an unmatched txid. This
	@# line prints only after net_dns_guard_selftest() passes; without the source
	@# +port binding the spoofed replies are accepted and the boot panics here.
	@if ! grep -q "DNSGUARD: spoofed DNS answer rejected (src+port bound)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: DNS answer-binding gate missing (DNSGUARD)"; \
		echo "Boot log:"; \
		cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; \
		echo "=== Smoke Test FAILED ==="; \
		exit 1; \
	fi
	@echo "SUCCESS: DNS answer-binding gate passed (spoofed source/port rejected)"
	@# Adversarial-qseed gate (bug-hunt of the untrusted boot cmdline): the qseed=
	@# token must not be able to zero the PRNG (xorshift64's absorbing fixed
	@# point, which would silently kill SYS_QRAND, quantum_kernel_rand, the DNS
	@# anti-spoof randomization, and Lamport key material). Printed only after the
	@# quantum self-test's non-zero-guard assertions pass; without the guard the
	@# self-test panics the boot.
	@if ! grep -q "QSEEDGUARD: PRNG survives adversarial qseed (non-zero)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: adversarial-qseed PRNG guard missing (QSEEDGUARD)"; \
		echo "Boot log:"; \
		cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; \
		echo "=== Smoke Test FAILED ==="; \
		exit 1; \
	fi
	@echo "SUCCESS: adversarial-qseed PRNG guard passed (state stays non-zero)"
	@# ghostOS phase-1 merge gate: the ring-3 ghostd service must recall
	@# all three noisy probes to their stored patterns (issue #48).
	@if ! grep -q "GHOSTD: 3/3 RECALL OK" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: ghostd self-test gate missing (GHOSTD: 3/3 RECALL OK)"; \
		echo "Boot log:"; \
		cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; \
		echo "=== Smoke Test FAILED ==="; \
		exit 1; \
	fi
	@echo "SUCCESS: ghostd associative-memory gate passed (GHOSTD: 3/3 RECALL OK)"
	@# ghostOS phase-2 honesty gate: booted WITHOUT a qseed, ghostd must name
	@# its noise source as a plain PRNG and NEVER claim quantum provenance.
	@if ! grep -q "GHOSTD: noise source = prng (no qseed)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: ghostd did not report 'prng (no qseed)' on a seedless boot"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if grep -q "GHOSTD: noise source = qseed-derived" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: ghostd falsely claimed qseed provenance without a qseed"; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: ghostd noise-source honesty gate passed (prng, no false quantum claim)"
	@# phase-2 capability gate: the capless ghost-test must be denied SYS_QRAND.
	@if ! grep -q "QRAND: capless caller denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capless SYS_QRAND was not denied (QRAND: capless caller denied)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: SYS_QRAND capability gate proven (capless caller denied EPERM)"
	@# copy_from_user DoS gate (issue #158): a ring-3 process handing a syscall an
	@# in-range but UNMAPPED user pointer must get EFAULT, not panic the kernel.
	@# This gate is anti-vacuous by construction: WITHOUT the page-table-validating
	@# copy the sysinfo copy-out below faults in ring 0, boot_panic halts the
	@# machine, and NEITHER this line nor any later gate (incl. GHOSTD, above)
	@# would appear — so the boot could not have reached here at all.
	@if ! grep -q "COPYGUARD: unmapped pointer denied (EFAULT)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: copy_from_user unmapped-pointer guard missing (issue #158)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@# Second leg: a copy-OUT to a mapped-but-READ-ONLY page (the code page) must
	@# also EFAULT on the write-permission check, not fault the kernel.
	@if ! grep -q "COPYGUARD: RO copy-out denied (EFAULT)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: copy_from_user read-only copy-out guard missing (issue #158)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: copy_from_user DoS guard proven (unmapped + RO pointers denied EFAULT)"
	@# ghostd phase-4 device gate (issue #51): the capless ghost-test must be
	@# denied SYS_COM2 — only swarm_svc holds the COM2 device capability.
	@if ! grep -q "COM2: capless caller denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capless SYS_COM2 was not denied (COM2: capless caller denied)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: SYS_COM2 device gate proven (capless caller denied EPERM)"
	@# ghostOS phase-3 merge gate (issue #50): paradoxd must resolve the canned
	@# contradiction and print its deterministic RESOLVED line.
	@if ! grep -q "PARADOXD: RESOLVED" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: paradoxd resolution gate missing (PARADOXD: RESOLVED)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: paradoxd resolution gate passed (PARADOXD: RESOLVED)"
	@# phase-3 capability gate: an unauthorised targeted send is denied EPERM.
	@if ! grep -q "PARADOXD: capless send denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: unauthorised send was not denied (PARADOXD: capless send denied)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: paradoxd capability gate proven (unauthorised send denied EPERM)"
	@# phase-3 coupling gate: paradoxd's phase machine, gated on ghostd's field
	@# order parameter R over IPC, must actually transition (the two services
	@# are coupled through the field).
	@if ! grep -q "PARADOXD: phase ->" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: paradoxd/ghostd coupling produced no phase transition (PARADOXD: phase ->)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: paradoxd/ghostd field coupling proven (phase transition gated on ghost R)"
	@# epic #62 phase 1 (issue #63): the interactive shell must come up as a
	@# ring-3 service holding the console capability and greet on the console.
	@if ! grep -q "QSH: QuantumOS interactive shell ready" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: qsh did not greet (QSH: QuantumOS interactive shell ready)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: qsh interactive shell came up (console capability live)"
	@# Input integrity: the FIRST piped command ('help') must arrive intact
	@# across the boot handoff — every byte rescued, none eaten by UART init.
	@if ! grep -q "qsh commands:" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: piped 'help' did not execute intact (qsh commands:)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: first piped command arrived intact (no input bytes lost at boot)"
	@# The piped 'ps' must show the shell observing ITSELF running in the live
	@# process table — the SYS_SYSINFO introspection round trip.
	@if ! grep -q "qsh RUNNING" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: piped 'ps' did not list qsh RUNNING (SYS_SYSINFO round trip)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: ps ran — the shell sees itself RUNNING in the process table"
	@# 'free' must report live kernel memory stats.
	@if ! grep -q "MEM: heap free=" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: piped 'free' produced no memory stats (MEM: heap free=)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: free reported live kernel memory stats"
	@# 'ghost' must fetch ghostd's field status over capability-checked IPC —
	@# TWICE: once before any 'run' and once AFTER the spawn legs. The second
	@# answer gates the first-match routing invariant (epic #175): if a spawn
	@# ever handed qsh a second IPC WRITE cap (first-fit slots below the ghostd
	@# cap are guaranteed free by then), the post-run ghost_req would misroute
	@# to a dead child and never answer.
	@if [ "$$(grep -c 'qsh: ghost R=' /tmp/qemu-boot.log 2>/dev/null)" -lt 2 ]; then \
		echo "ERROR: expected TWO 'ghost' answers (pre-run + post-run); the post-run one gates"; \
		echo "       qsh's singleton-IPC-cap first-match invariant (qsh: ghost R= x2)"; \
		grep -c "qsh: ghost R=" /tmp/qemu-boot.log 2>/dev/null || true; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: shell queried ghostd's field over capability IPC (pre-run AND post-run)"
	@# 'uptime' + 'qrand' must answer (the shell's declared quantum-pool cap works).
	@if ! grep -q "qsh: uptime " /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: piped 'uptime' produced no answer (qsh: uptime)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "qsh: qrand " /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: piped 'qrand' drew no bytes (qsh: qrand) — shell quantum cap broken"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: uptime + qrand answered (shell quantum-pool cap live)"
	@# RTC: 'date' must report the wall-clock time from the CMOS RTC. The year
	@# comes from the (QEMU host) clock, so gate the "TIME: 20xx-.." shape —
	@# plausible for any real boot without pinning an exact timestamp.
	@if ! grep -qE "TIME: 20[0-9][0-9]-[0-1][0-9]-[0-3][0-9] " /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: 'date' did not report a plausible wall-clock time (TIME: 20xx-..)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: date reported the wall-clock time (CMOS RTC live)"
	@# epic #62 phase 2 (issue #64): the shell must greet with /etc/motd read
	@# through the VFS off the embedded initrd.
	@if ! grep -q "Welcome to QuantumOS" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: motd was not served through the VFS (Welcome to QuantumOS)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: motd greeted through the VFS (initrd read path live)"
	@# 'ls' must list the initrd inventory (SYS_READDIR).
	@if ! grep -q "FS: etc/motd" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: piped 'ls' did not list the initrd (FS: etc/motd)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: ls listed the initrd inventory (SYS_READDIR live)"
	@# 'cat' must stream a file's actual bytes (open/read/close round trip).
	@if ! grep -q "The initrd is real" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: piped 'cat /docs/hello.txt' did not print the file (The initrd is real)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: cat streamed a file off the initrd (open/read/close live)"
	@# epic #62 phase 3 (issue #65): 'run /bin/hello' must start a program OFF
	@# THE FILESYSTEM (not kernel-embedded) and the program must actually run.
	@if ! grep -q "HELLO: greetings from /bin/hello" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: /bin/hello did not run from the initrd (HELLO: greetings)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: shell executed a program off the filesystem (SYS_SPAWN live)"
	@# ...and the shell must collect its exit code (SYS_WAITPID round trip).
	@if ! grep -q "exited (code 42)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: the shell did not report hello's exit code (exited (code 42))"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: exit code returned to the shell (SYS_WAITPID live)"
	@# epic #62 follow-up (issue #69): 'run /bin/args alpha quantumos' must pass
	@# an argument vector — the program reads argc=3 and both args back.
	@if ! grep -q "ARGS: argc=3" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: /bin/args did not see argc=3 (argv not delivered)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "ARGS: argv\[1\]=alpha" /tmp/qemu-boot.log 2>/dev/null || \
	    ! grep -q "ARGS: argv\[2\]=quantumos" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: /bin/args did not echo its arguments (argv[1]=alpha argv[2]=quantumos)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: argv delivered to a spawned program (argc + both args read back)"
	@# libq foundation: 'run /bin/libqtest' must reach the sentinel — proving the
	@# freestanding runtime (mem*/str*, plus heap/printf as they land) works at
	@# -O2 in ring 3. Also the runtime backstop for the -O2 self-recursion trap.
	@if ! grep -q "LIBQ: self-test OK" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: libq self-test did not pass (LIBQ: self-test OK)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: libq freestanding runtime self-test passed (LIBQ: self-test OK)"
	@if ! grep -q "LIBQ printf d=-7 u=42 x=beef s=ok" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: libq printf->SYS_WRITE path did not run (LIBQ printf d=-7 u=42 x=beef s=ok)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: libq printf -> SYS_WRITE path exercised end-to-end"
	@# first native app citizen: 'run /bin/consciousnessd' runs a fixed-point
	@# Kuramoto field on libq; the order parameter must actually synchronise
	@# (r climb past 0.8) for the CONSCIOUSNESS EMERGED marker to print.
	@if ! grep -q "consciousnessd: CONSCIOUSNESS EMERGED" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: consciousnessd did not synchronise (consciousnessd: CONSCIOUSNESS EMERGED)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: consciousnessd Kuramoto field synchronised on libq (CONSCIOUSNESS EMERGED)"
	@# second citizen: kannakad, now a SERVICE of the KERNEL field (epic #95
	@# phase 2): at boot it imprints 7 seeds via SYS_IMPRINT (region 1),
	@# recalls a byte-corrupted probe back to the EXACT stored text, and
	@# proves retrieval reinforcement raised the winner's score through the
	@# syscall's write-side contract — all three, or no RESONANCE VERIFIED.
	@if ! grep -q "kannakad: RESONANCE VERIFIED" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: kannakad kernel-field recall/reinforcement failed (kannakad: RESONANCE VERIFIED)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: kannakad recall + reinforcement verified on the KERNEL field (RESONANCE VERIFIED)"
	@# fourth citizen: quantumd runs at boot as a quantum-pool SERVICE and draws
	@# REAL entropy (SYS_QRAND); its amplitude-amplification recall must agree
	@# with the classical argmax for QUANTUM VERIFIED to print.
	@if ! grep -q "quantumd: QUANTUM VERIFIED" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: quantumd quantum demo failed (quantumd: QUANTUM VERIFIED)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: quantumd drew real quantum entropy + amplitude-amplification recall (QUANTUM VERIFIED)"
	@# fifth citizen: 'run /bin/qtop' renders a snapshot dashboard from the
	@# uncapped SYS_SYSINFO surface (memory gauge, clock, resonance-field wave,
	@# live process table); DASHBOARD RENDERED proves the whole render path ran.
	@if ! grep -q "qtop: DASHBOARD RENDERED" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: qtop dashboard did not render (qtop: DASHBOARD RENDERED)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: qtop snapshot dashboard rendered from SYS_SYSINFO (DASHBOARD RENDERED)"
	@# sixth citizen (issue #102): 'run /bin/life' runs Conway's Game of Life on
	@# a torus. A glider is 5 cells and stays 5 while it moves, so a live count
	@# COMPUTED from the evolved grid == 5 after 16 generations proves the step
	@# function is correct (a broken neighbour count explodes or dies).
	@if ! grep -q "LIFE: glider intact after 16 generations (live=5)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: /bin/life did not keep the glider intact (LIFE: glider intact ... live=5)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: /bin/life Game of Life glider computed correctly (live=5 after 16 gens)"
	@# epic #95: the kernel holographic field. Three imprints land in
	@# slots 0..2, a ~15%-corrupted probe must recall the EXACT stored
	@# content (a line form echoed input cannot produce), a cap-holder's
	@# cross-region request and a capless caller's requests must be
	@# denied with exactly EPERM, and a degenerate probe must be a clean
	@# n=0 — never a kernel fault.
	@if ! grep -q "FIELD: imprinted slot 2" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: field imprints did not land (FIELD: imprinted slot 2)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -qF "FIELD: winner=\"the cat sat on the mat\" slot=0 score=" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: noisy-probe recall did not recover the stored pattern"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "FIELD: cross-region denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: cross-region isolation not proven"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "FIELD: empty-probe ok (n=0)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: degenerate probe misbehaved"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "FIELD: capless imprint denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capless SYS_IMPRINT was not denied"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "FIELD: capless recall denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capless SYS_RECALL was not denied"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: kernel holographic field verified (imprint/recall/isolation/capless/degenerate)"
	@# epic #71 phase 2: the writable RAM overlay. 'write' must store bytes
	@# (kernel-reported count, not an echo), 'ls /data' must show the file
	@# tagged [ram] with the kernel-computed size, and 'rm' must remove it.
	@if ! grep -q "qsh: wrote " /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: 'write' did not store to the overlay (qsh: wrote)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "FS: data/note .* \[ram\]" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: 'ls /data' did not list the overlay file (FS: data/note ... [ram])"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "qsh: removed" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: 'rm' did not remove the overlay file (qsh: removed)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: writable RAM overlay works (write stored, ls [ram], rm removed)"
	@# Capability gate: the capless ghost-test must be denied filesystem writes.
	@if ! grep -q "FSW: capless caller denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capless filesystem-create was not denied (FSW: capless caller denied)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: filesystem-write capability gate proven (capless caller denied EPERM)"
	@# Capability gate: the capless ghost-test must be denied SYS_RESOLVE.
	@if ! grep -q "NETC: capless caller denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capless SYS_RESOLVE was not denied (NETC: capless caller denied)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: SYS_RESOLVE capability gate proven (capless caller denied EPERM)"
	@# Capability gate (epic #80): the capless ghost-test must be denied a
	@# SYS_UDP socket bind. The probe checks EXACTLY -4 (EPERM), so an
	@# unimplemented syscall (ENOSYS -3) or an argument error (EINVAL -1)
	@# cannot fake a pass; the cap check precedes every network check, so
	@# this holds in this NIC-less default boot.
	@if ! grep -q "NETC: capless UDP bind denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capless SYS_UDP bind was not denied (NETC: capless UDP bind denied)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: SYS_UDP capability gate proven (capless UDP bind denied EPERM)"
	@# Capability gate (epic #82): the capless ghost-test must be denied a
	@# SYS_TCP connect. Exact -4 (EPERM), so ENOSYS(-3)/EINVAL(-1) can't fake
	@# it; the cap check precedes the NIC-present check, so this holds in the
	@# NIC-less default boot.
	@if ! grep -q "NETC: capless TCP connect denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capless SYS_TCP connect was not denied (NETC: capless TCP connect denied)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: SYS_TCP capability gate proven (capless TCP connect denied EPERM)"
	@# Honesty gate: this default boot is DISKLESS, so the driver must say so
	@# and MUST NOT claim a disk, and 'sync' must fail honestly (no disk).
	@if ! grep -q "ATA: no disk" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: diskless boot did not report 'ATA: no disk'"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if grep -q "ATA: disk present" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: diskless boot falsely claimed 'ATA: disk present'"; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "qsh: sync failed (no disk)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: diskless 'sync' did not fail honestly (qsh: sync failed (no disk))"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: diskless honesty proven (no-disk reported, sync fails cleanly)"
	@# Capability gate: the capless ghost-test must be denied SYS_SPAWN — only
	@# qsh holds the spawn capability.
	@if ! grep -q "SPAWN: capless caller denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capless SYS_SPAWN was not denied (SPAWN: capless caller denied)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: SYS_SPAWN capability gate proven (capless caller denied EPERM)"
	@# Capability gate: the capless ghost-test must be denied SYS_CONS — only
	@# qsh holds the console device capability.
	@if ! grep -q "CONS: capless caller denied (EPERM)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capless SYS_CONS was not denied (CONS: capless caller denied)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: SYS_CONS capability gate proven (capless caller denied EPERM)"
	@# epic #135: intent manifest + first ENFORCED quota. The kernel boot
	@# self-test drives the real manifest check to a deny, recording an
	@# AUDIT_MDENY that proves the deny path is live (on the shipped system
	@# caps==intent, so no citizen ever trips it — this is what keeps a
	@# constant-true check from shipping green).
	@if ! grep -q "manifest self-test: PASS (MDENY recorded)" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: manifest self-test did not pass (manifest self-test: PASS (MDENY recorded))"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: manifest self-test proven (deny path live, MDENY recorded)"
	@# quota-test spawns /bin/qprobe twice; the SECOND must be refused by the
	@# spawn quota (the first ENFORCED quota). It prints QUOTA ENFORCED only on
	@# the exact {first ok, second EPERM} outcome, else QUOTA BROKEN.
	@if ! grep -q "QUOTA ENFORCED pid=" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: spawn quota did not enforce (QUOTA ENFORCED pid=)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if grep -q "QUOTA BROKEN" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: spawn quota was NOT enforced (QUOTA BROKEN present)"; \
		grep "QUOTA BROKEN" /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: spawn quota ENFORCED (second over-quota spawn refused; no QUOTA BROKEN)"
	@# epic #144: CPU-quota enforcement. The cpu-hog citizen busy-spins with a
	@# finite cpu_limit; the kernel TERMINATES it from the timer tick once its
	@# scheduled-in cpu_ticks cross the budget, emitting an unforgeable kernel
	@# "CPUKILL: pid=" line. (The hog is dead and cannot self-report; the ledger
	@# entry is the load-bearing proof — see the MCP gate.) Convergence bound:
	@# time-to-kill ~= cpu_limit x N_runnable x 10ms; cpu_limit=10 registered
	@# early keeps this well inside the boot window.
	@if ! grep -q "CPUKILL: pid=" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: cpu-hog was not terminated for its CPU budget (CPUKILL: pid=)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: CPU quota ENFORCED (cpu-hog terminated over budget; CPUKILL recorded)"
	@# epic #148 (A1): qsv — EXACT integer quantum state vector in ring 3.
	@# CI asserts INTEGER EQUALITY, never sampled counts: Grover-3q must hit
	@# exactly 121/128 and Grover-4q exactly 63001/65536 (amp -16064 at h=28),
	@# with the norm identity sum|v|^2 == 2^h checked in-citizen (any failure
	@# prints QSV BROKEN, which must be ABSENT). Then the digest cross-check:
	@# the in-OS engine's sha256 of the final Grover-4q state must equal the
	@# INDEPENDENT host Python mirror's digest (two implementations agree
	@# bit-for-bit), and must NOT equal the mirror's --corrupt digest (so the
	@# comparison cannot pass vacuously — a broken extractor/mirror pair that
	@# "always matches" fails the inequality leg).
	@if grep -q "QSV BROKEN" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: qsv engine self-check failed (QSV BROKEN)"; \
		grep "QSV" /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "QSV: grover3 h=15 amp=176 p=121/128 norm=OK" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: qsv Grover-3q did not hit exactly 121/128"; \
		grep "QSV" /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "QSV: grover4 h=28 amp=-16064 p=63001/65536 norm=OK" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: qsv Grover-4q did not hit exactly 63001/65536"; \
		grep "QSV" /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "QSV: PROOF COMPLETE" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: qsv proof did not complete"; \
		grep "QSV" /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@QSV_OS=$$(grep -o "QSV: state=sha256:[0-9a-f]*" /tmp/qemu-boot.log | head -1 | cut -d: -f3); \
	QSV_HOST=$$(python3 scripts/qsv_mirror.py); \
	QSV_BAD=$$(python3 scripts/qsv_mirror.py --corrupt); \
	if [ -z "$$QSV_OS" ]; then \
		echo "ERROR: qsv printed no state digest"; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi; \
	if [ "$$QSV_OS" != "$$QSV_HOST" ]; then \
		echo "ERROR: qsv digest mismatch — OS=$$QSV_OS host-mirror=$$QSV_HOST"; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi; \
	if [ "$$QSV_OS" = "$$QSV_BAD" ]; then \
		echo "ERROR: corrupt-circuit digest MATCHED (vacuous comparison)"; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: qsv EXACT integer quantum computation (Grover 121/128 + 63001/65536; digest matches independent mirror, corrupt differs)"
	@# epic #148 (A2+A3): the QPU job BROKER. Submitters (qpu_test) queue OPAQUE
	@# circuits; the kernel enforces the QPU cap + qsub quota + slot lifecycle and
	@# never parses a circuit; the executor (qpud) runs the exact integer engine
	@# and returns the result. STRUCTURAL cross-reference (not a bare print): the
	@# submitter's "job=<id>" is the kernel-assigned id, and qpud's independent
	@# "executed job=<id> owner=" line for the SAME id proves the result was
	@# actually brokered — forging it requires collusion on a monotonic kernel id.
	@if grep -q "QPU BROKEN" /tmp/qemu-boot.log 2>/dev/null || grep -q "QPU: WARNING" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: QPU broker self-check failed (QPU BROKEN / WARNING)"; \
		grep "QPU" /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@JB=$$(grep -o "QPU: bell via broker job=[0-9]*" /tmp/qemu-boot.log | head -1 | grep -o "[0-9]*$$"); \
	if [ -z "$$JB" ] || ! grep -qF "p=1/2 (exact)" /tmp/qemu-boot.log || ! grep -q "QPUD: executed job=$$JB owner=" /tmp/qemu-boot.log; then \
		echo "ERROR: Bell was not brokered (no matching QPUD executed job=$$JB)"; \
		grep "QPU" /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@JG=$$(grep -o "QPU: grover3 via broker job=[0-9]*" /tmp/qemu-boot.log | head -1 | grep -o "[0-9]*$$"); \
	if [ -z "$$JG" ] || ! grep -qF "p=121/128" /tmp/qemu-boot.log || ! grep -q "QPUD: executed job=$$JG owner=" /tmp/qemu-boot.log; then \
		echo "ERROR: Grover-3q was not brokered (no matching QPUD executed job=$$JG)"; \
		grep "QPU" /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@for gate in \
		"QPU: quota ENFORCED (third submit refused)" \
		"QPU: capless submit denied (EPERM)" \
		"QPU: executor submit denied (EPERM)" \
		"QPU: submitter complete denied (EPERM)"; do \
		if ! grep -qF "$$gate" /tmp/qemu-boot.log 2>/dev/null; then \
			echo "ERROR: missing QPU gate: $$gate"; \
			grep "QPU" /tmp/qemu-boot.log 2>/dev/null || true; \
			echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
		fi; \
	done
	@echo "SUCCESS: QPU broker (submit->execute->poll cross-referenced; qsub quota + WRITE/EXECUTE partition + capless deny)"
	@# epic #137: cross-ring capability delegation. The delegator hands a
	@# NARROWED READ-only field cap to the sub-agent; the sub-agent proves the
	@# narrowing ({recall ok, imprint EPERM}) -> DELEG ENFORCED, then proves
	@# DELEGATED provenance: the delegator exits, the reaper cascade-revokes the
	@# derived cap, and the sub-agent's recall flips to EPERM -> DELEG REVOKED.
	@# A static grant survives an unrelated process's death — only a derived cap
	@# cascades — so REVOKED is the un-fakeable delegated-vs-static distinguisher.
	@if ! grep -q "DELEG ENFORCED pid=" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: delegated cap not enforced (DELEG ENFORCED pid=)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "DELEG REVOKED pid=" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: delegated cap did not cascade-revoke on delegator exit (DELEG REVOKED pid=)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if grep -q "DELEG BROKEN" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: capability delegation was NOT enforced (DELEG BROKEN present)"; \
		grep "DELEG BROKEN" /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: capability delegation proven (narrowed cap enforced + cascade-revoked; no BROKEN)"
	@# Agent-native end-to-end demo (the mission showcase): an orchestrator ring-3
	@# citizen runs a QPU job through the broker, imprints+recalls holographic field
	@# memory, spawns a sub-process, AND delegates a narrowed field cap to EACH of a
	@# society of sub-agents — all four verified. The gate line prints only if every
	@# step succeeded (society => every distinct sub-agent recalled the exact phrase
	@# from its own noisy probe and its content digest matched the orchestrator's).
	@if ! grep -q "AGENTD: DEMO OK qpu+field+spawn+society" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: agent-native end-to-end demo did not complete (AGENTD: DEMO OK)"; \
		grep -E "AGENT BROKEN|AGENTD: DEMO BROKEN|AGENTSUB BROKEN" /tmp/qemu-boot.log 2>/dev/null || true; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "AGENT: society consensus 3/3" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: society content consensus not reached (AGENT: society consensus 3/3)"; \
		grep -E "AGENT:|AGENTSUB" /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@# Division of labor (epic #177): every specialist published its DISTINCT
	@# result into its OWN workspace region — orchestrator-verified (live==1
	@# per region + exact recomputed content) — with sibling writes refused.
	@if ! grep -q "AGENT: division of labor 3/3" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: division of labor not proven (AGENT: division of labor 3/3)"; \
		grep -E "AGENT|AGENTSUB" /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@# Society-of-societies handoff (epic #178): the orchestrator computed its
	@# society's qseed-salted aggregate and handed it to fieldsyncd (which
	@# broadcasts it to coupled peers; single-VM boots just hold it). Inside
	@# the DEMO OK conjunction — a dead handoff cannot ship green.
	@if ! grep -q "AGENT: aggregate a" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: society aggregate not handed off (AGENT: aggregate a...)"; \
		grep -E "AGENT" /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if grep -qE "AGENT BROKEN|AGENTD: DEMO BROKEN|AGENTSUB BROKEN" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: agent demo reported a BROKEN step"; \
		grep -E "AGENT BROKEN|AGENTD: DEMO BROKEN|AGENTSUB BROKEN" /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: agent-native society demo proven end to end (QPU + field + spawn + society content consensus)"
	@# Supervision gate: after the piped 'exit', the watchdog must restart the
	@# shell, which reintroduces itself as reborn.
	@if ! grep -q "QSH: reborn" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: shell was not restarted after exit (QSH: reborn)"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: watchdog rebirthed the shell after exit (QSH: reborn)"
	@# IPC re-wire gate (ADR-0023): the REBORN shell must answer the delayed
	@# 'ghost' — its ghostd IPC pair comes back only via the declarative
	@# ipc_peers re-mint (the citizens.c hand mints are gone; without the
	@# re-mint the reborn shell holds no ghostd cap and the send is
	@# EPERM-denied). ANCHORED to the post-'QSH: reborn' slice: the session
	@# already prints 'qsh: ghost R=' twice BEFORE the rebirth, so a whole-log
	@# grep would pass with the feature entirely broken. Single UART + single
	@# CPU means serial output is strictly ordered — the slice cannot capture
	@# a pre-rebirth answer. Revert-confirm: dropping qsh's ipc_peers
	@# declaration removes ghost wiring on EVERY incarnation (all R= answers
	@# vanish, including the two the x2 gate above counts); THIS slice is the
	@# rebirth discriminator.
	@if ! awk '/QSH: reborn/{f=1} f' /tmp/qemu-boot.log 2>/dev/null | grep -q "qsh: ghost R="; then \
		echo "ERROR: reborn shell did not answer 'ghost' — IPC peer re-mint broken (ADR-0023)"; \
		echo "Post-reborn slice:"; \
		awk '/QSH: reborn/{f=1} f' /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: reborn shell re-acquired its ghostd IPC pair (declarative re-mint, ADR-0023)"
	@# Part-1 INTEGRATION gate (ADR-0023): `ghost exit` restarted ghostd under
	@# the LIVING shell. The shell held a live qsh->ghostd cap across ghostd's
	@# death; without the dead-target unlink that stale cap sits at a LOWER
	@# first-fit slot than the Pass-2 re-mint and wins untargeted send_msg's
	@# first-match, routing to the dead pid — no answer. Only unlink + re-mint
	@# together produce a 'ghost R=' AFTER the 'GHOSTD: FIELD REBORN' banner.
	@# (The self-test gates Part 1's MECHANISM; this leg gates its NECESSITY
	@# in the live routing path.) Anchored the same way as the reborn-shell
	@# gate: single UART, strictly ordered, the slice cannot see earlier
	@# answers. Inherently non-vacuous: if `ghost exit` were dropped, the
	@# banner never prints and the slice is empty.
	@if ! grep -q "GHOSTD: exiting" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: 'ghost exit' never reached ghostd (GHOSTD: exiting missing)"; \
		echo "Boot log tail:"; tail -40 /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "GHOSTD: FIELD REBORN" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: watchdog did not rebirth ghostd after 'ghost exit' (GHOSTD: FIELD REBORN missing)"; \
		echo "Boot log tail:"; tail -40 /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! awk '/GHOSTD: FIELD REBORN/{f=1} f' /tmp/qemu-boot.log 2>/dev/null | grep -q "qsh: ghost R="; then \
		echo "ERROR: living shell got no answer from REBORN ghostd — stale-cap unlink or re-mint broken (ADR-0023 Part 1)"; \
		echo "Post-FIELD-REBORN slice:"; \
		awk '/GHOSTD: FIELD REBORN/{f=1} f' /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: living shell survived a ghostd rebirth (dead-target unlink + re-mint, ADR-0023 Part 1)"
	@# epic #73: the default boot attaches no rtl8139, so the NIC driver must
	@# report its honest absence and MUST NOT claim a NIC came up.
	@if ! grep -q "NET: no rtl8139" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: NIC-less boot did not report 'NET: no rtl8139'"; \
		echo "Boot log:"; cat /tmp/qemu-boot.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if grep -q "NET: rtl8139 up" /tmp/qemu-boot.log 2>/dev/null; then \
		echo "ERROR: NIC-less boot falsely claimed 'NET: rtl8139 up'"; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: NIC-less boot degrades honestly (no rtl8139 reported)"
	@echo ""
	@echo "=== Smoke Test PASSED ==="

# CI Smoke Test (ADR-0021 PR-4 differential sizing, -m 256M): boot at 256 MB and
# prove (1) the PMM sized itself from the LARGER map — total_frames ~doubles vs
# the 128M leg, which a 128 MB hardcode could never do; (2) the high-memory
# reachability self-test took its LIVE branch — a top-of-pool frame at >= 128 MB
# was allocated AND written/read-back through the boot.S identity map. Together
# with ci-smoke (128M) and ci-smoke-mem512 this is the true hardcode-vs-live
# differential: a hardcode prints 0x7FE0 at EVERY -m and never PMMHIGH-live.
ci-smoke-mem256: kernel
	@echo "=== ADR-0021 differential sizing gate: -m 256M ==="
	@test -f $(BUILD_DIR)/kernel.elf32 || (echo "ERROR: kernel not built" && exit 1)
	@timeout 14s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 -append quiet -serial stdio -m 256M -display none -no-reboot 2>&1 | tee /tmp/qemu-mem256.log || true
	@grep -q "QuantumOS ready" /tmp/qemu-mem256.log 2>/dev/null || (echo "ERROR: kernel did not reach 'QuantumOS ready' at -m 256M"; cat /tmp/qemu-mem256.log 2>/dev/null || true; echo "=== mem256 FAILED ==="; exit 1)
	@grep -q "PMMSIZE=" /tmp/qemu-mem256.log 2>/dev/null || (echo "ERROR: PMMSIZE marker missing at -m 256M"; echo "=== mem256 FAILED ==="; exit 1)
	@PMMHEX=$$(grep -oE "PMMSIZE=[0-9A-Fa-f]{16}" /tmp/qemu-mem256.log | head -1 | sed 's/PMMSIZE=//'); \
	PMMDEC=$$((0x$$PMMHEX)); \
	if [ "$$PMMDEC" -lt 65280 ] || [ "$$PMMDEC" -gt 65536 ]; then \
		echo "ERROR: -m 256M frame count $$PMMDEC (0x$$PMMHEX) outside 256 MB-class window [65280,65536] — sizing did not track -m"; \
		cat /tmp/qemu-mem256.log 2>/dev/null || true; \
		echo "=== mem256 FAILED ==="; \
		exit 1; \
	fi; \
	echo "SUCCESS: -m 256M sized to 0x$$PMMHEX (~2x the 128M leg — sizing tracks real RAM)"
	@grep -q "PMMHIGH: high-frame alloc + identity writeback verified" /tmp/qemu-mem256.log 2>/dev/null || (echo "ERROR: high-memory LIVE gate missing at -m 256M (PMMHIGH writeback)"; cat /tmp/qemu-mem256.log 2>/dev/null || true; echo "=== mem256 FAILED ==="; exit 1)
	@echo "SUCCESS: high-memory writeback verified at -m 256M (identity map reaches >= 128 MB RAM)"
	@grep -q "PMMSTORM: heap survives a 1024-frame drain" /tmp/qemu-mem256.log 2>/dev/null || (echo "ERROR: residency-storm gate missing at -m 256M (PMMSTORM)"; echo "=== mem256 FAILED ==="; exit 1)
	@echo "=== mem256 differential gate PASSED ==="

# CI Smoke Test (ADR-0021 PR-4 differential sizing, -m 512M): as ci-smoke-mem256
# but at 512 MB — total_frames must ~quadruple the 128M leg (~0x1FFE0), and the
# high-memory writeback must verify a frame near 512 MB is reachable through the
# 1 GB identity map. Confirms sizing scales linearly with real RAM, not a step.
ci-smoke-mem512: kernel
	@echo "=== ADR-0021 differential sizing gate: -m 512M ==="
	@test -f $(BUILD_DIR)/kernel.elf32 || (echo "ERROR: kernel not built" && exit 1)
	@timeout 14s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 -append quiet -serial stdio -m 512M -display none -no-reboot 2>&1 | tee /tmp/qemu-mem512.log || true
	@grep -q "QuantumOS ready" /tmp/qemu-mem512.log 2>/dev/null || (echo "ERROR: kernel did not reach 'QuantumOS ready' at -m 512M"; cat /tmp/qemu-mem512.log 2>/dev/null || true; echo "=== mem512 FAILED ==="; exit 1)
	@grep -q "PMMSIZE=" /tmp/qemu-mem512.log 2>/dev/null || (echo "ERROR: PMMSIZE marker missing at -m 512M"; echo "=== mem512 FAILED ==="; exit 1)
	@PMMHEX=$$(grep -oE "PMMSIZE=[0-9A-Fa-f]{16}" /tmp/qemu-mem512.log | head -1 | sed 's/PMMSIZE=//'); \
	PMMDEC=$$((0x$$PMMHEX)); \
	if [ "$$PMMDEC" -lt 130816 ] || [ "$$PMMDEC" -gt 131072 ]; then \
		echo "ERROR: -m 512M frame count $$PMMDEC (0x$$PMMHEX) outside 512 MB-class window [130816,131072] — sizing did not track -m"; \
		cat /tmp/qemu-mem512.log 2>/dev/null || true; \
		echo "=== mem512 FAILED ==="; \
		exit 1; \
	fi; \
	echo "SUCCESS: -m 512M sized to 0x$$PMMHEX (~4x the 128M leg — sizing tracks real RAM)"
	@grep -q "PMMHIGH: high-frame alloc + identity writeback verified" /tmp/qemu-mem512.log 2>/dev/null || (echo "ERROR: high-memory LIVE gate missing at -m 512M (PMMHIGH writeback)"; cat /tmp/qemu-mem512.log 2>/dev/null || true; echo "=== mem512 FAILED ==="; exit 1)
	@echo "SUCCESS: high-memory writeback verified at -m 512M (identity map reaches ~512 MB RAM)"
	@grep -q "PMMSTORM: heap survives a 1024-frame drain" /tmp/qemu-mem512.log 2>/dev/null || (echo "ERROR: residency-storm gate missing at -m 512M (PMMSTORM)"; echo "=== mem512 FAILED ==="; exit 1)
	@echo "=== mem512 differential gate PASSED ==="

# CI Smoke Test (persistence): the epic #71 capstone. Boot the SAME disk image
# TWICE. Boot 1 attaches a freshly-zeroed image, writes a file to the overlay,
# and syncs it to disk. Boot 2 attaches the SAME image and must read the file
# back — proving persistence across reboots.
#
# Gate-integrity rules (learned from the design attack):
#  - The image is recreated fresh INSIDE this recipe every run (never a make
#    prerequisite), so a stale image from a previous run can never make it pass.
#  - The two boots tee to DISTINCT logs. qsh echoes typed input, so boot 1's log
#    contains the content string from the 'write' command echo — proving nothing.
#    The content gate is therefore checked ONLY in boot 2's log, and boot 2 never
#    types the content (only 'cat'), so its appearance there is genuine readback.
#  - Boot 1 gates its own 'disk present' + sync-success BEFORE boot 2 runs, so a
#    broken write/sync fails here (correctly attributed) instead of surfacing as
#    boot 2's "content missing".
#  - The two QEMU runs are strictly sequential (QEMU takes a write lock on the
#    raw image).
DISK_IMG = $(BUILD_DIR)/disk.img
ci-smoke-disk: kernel
	@echo "=== QuantumOS Persistence Smoke Test (two boots, one disk) ==="
	@# SAFETY gate first: a disk carrying a FOREIGN volume (fake MBR in
	@# sector 0) must never be written — on real hardware LBA 0 is the
	@# machine's partition table. The image must be BYTE-IDENTICAL
	@# after a sync attempt, and the refusal must be logged.
	@rm -f /tmp/qemu-foreign.img
	@dd if=/dev/zero of=/tmp/qemu-foreign.img bs=1M count=2 2>/dev/null
	@printf 'FAKE-MBR-DO-NOT-TOUCH' | dd of=/tmp/qemu-foreign.img conv=notrunc 2>/dev/null
	@md5sum /tmp/qemu-foreign.img | cut -d" " -f1 > /tmp/qemu-foreign.before
	@( printf 'write /data/x probe\nsync\n'; sleep 8 ) | \
		timeout 10s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-drive file=/tmp/qemu-foreign.img,format=raw,if=ide -serial stdio -m 128M \
		-display none -no-reboot 2>&1 | tee /tmp/qemu-disk-boot0.log || true
	@if ! grep -q "SYNC: disk carries a foreign volume - refusing to overwrite" /tmp/qemu-disk-boot0.log 2>/dev/null; then \
		echo "ERROR: sync did not refuse a foreign volume"; \
		echo "Boot log:"; cat /tmp/qemu-disk-boot0.log 2>/dev/null || true; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@md5sum /tmp/qemu-foreign.img | cut -d" " -f1 > /tmp/qemu-foreign.after
	@if ! cmp -s /tmp/qemu-foreign.before /tmp/qemu-foreign.after; then \
		echo "ERROR: sync MODIFIED a foreign volume despite refusing"; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: foreign volume refused and left byte-identical — SAFE FOR REAL DISKS"
	@rm -f $(DISK_IMG)
	@dd if=/dev/zero of=$(DISK_IMG) bs=1M count=2 2>/dev/null
	@echo "[boot 1] write /data/note + sync to a fresh disk..."
	@( printf 'write /data/note tide-remembers-x91\nimprint the tide remembers x91\nsync\n'; sleep 8 ) | \
		timeout 10s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-drive file=$(DISK_IMG),format=raw,if=ide -serial stdio -m 128M \
		-display none -no-reboot 2>&1 | tee /tmp/qemu-disk-boot1.log || true
	@if ! grep -q "ATA: disk present" /tmp/qemu-disk-boot1.log 2>/dev/null; then \
		echo "ERROR: boot 1 did not detect the disk (ATA: disk present)"; \
		echo "Boot log:"; cat /tmp/qemu-disk-boot1.log 2>/dev/null || true; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: boot 1 detected the ATA disk"
	@if ! grep -q "ATA: sector RW self-test OK" /tmp/qemu-disk-boot1.log 2>/dev/null; then \
		echo "ERROR: boot 1 disk RW self-test did not pass"; \
		echo "Boot log:"; cat /tmp/qemu-disk-boot1.log 2>/dev/null || true; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: boot 1 sector read/write self-test passed"
	@if ! grep -q "qsh: sync ok" /tmp/qemu-disk-boot1.log 2>/dev/null; then \
		echo "ERROR: boot 1 sync did not succeed (qsh: sync ok)"; \
		echo "Boot log:"; cat /tmp/qemu-disk-boot1.log 2>/dev/null || true; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: boot 1 flushed the overlay to disk (qsh: sync ok)"
	@# epic #96: the sync must also have serialized the kernel field.
	@if ! grep -q "FIELD: synced slots to disk" /tmp/qemu-disk-boot1.log 2>/dev/null; then \
		echo "ERROR: boot 1 sync did not write the field section"; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: boot 1 serialized the kernel field to disk"
	@# durable authority ledger: the sync must also have persisted the audit
	@# ledger. Gated at boot 1 so a failed/skipped audit write is attributed
	@# HERE, not surfaced confusingly as boot 2's "content missing".
	@if ! grep -q "AUDIT: synced ledger to disk" /tmp/qemu-disk-boot1.log 2>/dev/null; then \
		echo "ERROR: boot 1 sync did not persist the audit ledger"; \
		echo "Boot log:"; cat /tmp/qemu-disk-boot1.log 2>/dev/null || true; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: boot 1 persisted the authority ledger to disk"
	@echo "[boot 2] cat /data/note off the SAME disk (fresh boot, no write)..."
	@( printf 'cat /data/note\nrecall the tjde remembers x9j\n'; sleep 8 ) | \
		timeout 10s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-drive file=$(DISK_IMG),format=raw,if=ide -serial stdio -m 128M \
		-display none -no-reboot 2>&1 | tee /tmp/qemu-disk-boot2.log || true
	@if ! grep -q "FS: restored persisted files from disk" /tmp/qemu-disk-boot2.log 2>/dev/null; then \
		echo "ERROR: boot 2 did not restore the archive from disk"; \
		echo "Boot log:"; cat /tmp/qemu-disk-boot2.log 2>/dev/null || true; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: boot 2 restored the persisted archive at boot"
	@# THE capstone: the content typed in boot 1 is read back in boot 2 — and
	@# boot 2 never typed it, so this can only be a genuine disk readback.
	@if ! grep -q "tide-remembers-x91" /tmp/qemu-disk-boot2.log 2>/dev/null; then \
		echo "ERROR: boot 2 could not read back the file written in boot 1"; \
		echo "Boot log:"; cat /tmp/qemu-disk-boot2.log 2>/dev/null || true; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: content written in boot 1 read back in boot 2 — PERSISTENCE PROVEN"
	@# epic #96: the FIELD side of the same story. Boot 2 restored the
	@# blob, region 0 was inherited at qsh's first grant (audited), and a
	@# corrupted probe recalled the EXACT text imprinted in boot 1 — which
	@# boot 2 never typed, so only a disk restore can produce it.
	@if ! grep -q "FIELD: restored slots from disk" /tmp/qemu-disk-boot2.log 2>/dev/null; then \
		echo "ERROR: boot 2 did not restore the field from disk"; \
		echo "Boot log:"; cat /tmp/qemu-disk-boot2.log 2>/dev/null || true; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "service: field region 0 inherited from disk (scrub skipped)" /tmp/qemu-disk-boot2.log 2>/dev/null; then \
		echo "ERROR: region 0 was not inherited at the first grant"; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@if ! grep -qF "FIELD: winner=\"the tide remembers x91\"" /tmp/qemu-disk-boot2.log 2>/dev/null; then \
		echo "ERROR: boot 2 recall did not recover the imprint from boot 1"; \
		echo "Boot log:"; cat /tmp/qemu-disk-boot2.log 2>/dev/null || true; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: field memory imprinted in boot 1 recalled in boot 2 — FIELD PERSISTENCE PROVEN"
	@# durable authority ledger: boot 2 restored the ledger, and the restore-
	@# time ring scan (main.c:169, BEFORE this boot emits any audit entry) found
	@# a GRANT — which can only have come from boot 1's persisted ledger, and
	@# which boot 2 never typed. Race-free (GRANT is emitted deterministically by
	@# core_services_init every boot, not by a timing-driven IRQ).
	@if ! grep -q "AUDIT: restored ledger from disk" /tmp/qemu-disk-boot2.log 2>/dev/null; then \
		echo "ERROR: boot 2 did not restore the audit ledger from disk"; \
		echo "Boot log:"; cat /tmp/qemu-disk-boot2.log 2>/dev/null || true; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "AUDIT: restored ledger carries a prior-boot GRANT" /tmp/qemu-disk-boot2.log 2>/dev/null; then \
		echo "ERROR: boot 2 restored ledger lacks a prior-boot GRANT (authority not durable)"; \
		echo "Boot log:"; cat /tmp/qemu-disk-boot2.log 2>/dev/null || true; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: authority ledger written in boot 1 restored in boot 2 — AUDIT PERSISTENCE PROVEN"
	@echo "[boot A] corrupt the audit blob at its fixed home; fs + field must survive, audit must cold-start..."
	@# audit home = 4096 - AUDIT_HOME_TOP_OFFSET(27) = 4069 (FROZEN at its
	@# legacy absolute position, epic #177 — field growth cannot move it); one
	@# random sector there breaks the audit checksum but touches neither fs
	@# (low sectors) nor field (4053..4062). Proves the audit section is
	@# crash-isolated like the field.
	@dd if=/dev/urandom of=$(DISK_IMG) bs=512 seek=4069 count=1 conv=notrunc 2>/dev/null
	@( printf 'help\n'; sleep 8 ) | \
		timeout 10s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-drive file=$(DISK_IMG),format=raw,if=ide -serial stdio -m 128M \
		-display none -no-reboot 2>&1 | tee /tmp/qemu-disk-bootA.log || true
	@if ! grep -q "AUDIT:.*cold start" /tmp/qemu-disk-bootA.log 2>/dev/null; then \
		echo "ERROR: boot A did not detect the corrupted audit blob"; \
		echo "Boot log:"; cat /tmp/qemu-disk-bootA.log 2>/dev/null || true; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@if grep -q "AUDIT: restored ledger from disk" /tmp/qemu-disk-bootA.log 2>/dev/null; then \
		echo "ERROR: boot A restored a CORRUPTED audit blob as truth"; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "FS: restored persisted files from disk" /tmp/qemu-disk-bootA.log 2>/dev/null; then \
		echo "ERROR: audit corruption bled into the fs section (isolation broken)"; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "FIELD: restored slots from disk" /tmp/qemu-disk-bootA.log 2>/dev/null; then \
		echo "ERROR: audit corruption bled into the field section (isolation broken)"; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: corrupted audit detected, cold-started honestly, fs + field unaffected — AUDIT ISOLATION PROVEN"
	@echo "[boot 3] corrupt the field blob at its fixed home; fs must survive, field must cold-start..."
	@# field home = audit home(4069) - FIELD_HOME_RESERVE_SECTORS(16) = 4053
	@# (the field blob lives BELOW the frozen audit home since epic #177; the
	@# home/header sector is the deterministic corruption target).
	@dd if=/dev/urandom of=$(DISK_IMG) bs=512 seek=4053 count=1 conv=notrunc 2>/dev/null
	@( printf 'help\n'; sleep 8 ) | \
		timeout 10s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-drive file=$(DISK_IMG),format=raw,if=ide -serial stdio -m 128M \
		-display none -no-reboot 2>&1 | tee /tmp/qemu-disk-boot3.log || true
	@if ! grep -q "FIELD: persisted field checksum mismatch - cold start" /tmp/qemu-disk-boot3.log 2>/dev/null; then \
		echo "ERROR: boot 3 did not detect the corrupted field blob"; \
		echo "Boot log:"; cat /tmp/qemu-disk-boot3.log 2>/dev/null || true; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@if grep -q "FIELD: restored slots from disk" /tmp/qemu-disk-boot3.log 2>/dev/null; then \
		echo "ERROR: boot 3 restored a CORRUPTED field blob as truth"; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "FS: restored persisted files from disk" /tmp/qemu-disk-boot3.log 2>/dev/null; then \
		echo "ERROR: field corruption bled into the fs section (isolation broken)"; \
		echo ""; echo "=== Persistence Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: corrupted field detected, cold-started honestly, fs unaffected — SECTION ISOLATION PROVEN"
	@echo ""
	@echo "=== Persistence Test PASSED ==="

# CI Smoke Test (disk UPGRADE path, epic #177): ci-smoke-disk above always
# writes AND reads with the SAME kernel, so it never exercises the cross-version
# case where an OLDER kernel's on-disk FIELD descriptor meets a NEWER field
# geometry. #177 froze the audit-ledger home (AUDIT_HOME_TOP_OFFSET) precisely so
# a field-geometry bump can never relocate — and thus never discard — the
# format-unchanged authority ledger on an existing disk. This gate proves BOTH
# halves, non-vacuously:
#   1. the on-disk audit descriptor names EXACTLY the frozen home (count-27) —
#      any change to audit_home_lba() moves it and fails this assert. The
#      compile-time _Static_assert pins the CONSTANT; this pins the FUNCTION's
#      actual on-disk result.
#   2. a superblock carrying a PRE-#177 field descriptor (old home count-6, old
#      4-region size 2456) restores the ledger anyway and cold-starts the field
#      GRACEFULLY (the lba/bytes mismatch short-circuits before any off-the-end
#      blob read from the stale LBA).
DISK_UPG_IMG = $(BUILD_DIR)/disk-upgrade.img
ci-smoke-disk-upgrade: kernel
	@echo "=== QuantumOS Disk Upgrade-Path Test (epic #177 frozen audit home) ==="
	@rm -f $(DISK_UPG_IMG)
	@dd if=/dev/zero of=$(DISK_UPG_IMG) bs=1M count=2 2>/dev/null
	@echo "[boot 1] write a valid QDSK disk (fs + field + audit) with the current kernel..."
	@( printf 'write /data/note upgrade-probe\nimprint the field abides\nsync\n'; sleep 8 ) | \
		timeout 10s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-drive file=$(DISK_UPG_IMG),format=raw,if=ide -serial stdio -m 128M \
		-display none -no-reboot 2>&1 | tee /tmp/qemu-upg-boot1.log || true
	@if ! grep -q "qsh: sync ok" /tmp/qemu-upg-boot1.log 2>/dev/null; then \
		echo "ERROR: boot 1 sync failed — no valid disk to upgrade"; \
		cat /tmp/qemu-upg-boot1.log 2>/dev/null || true; \
		echo ""; echo "=== Upgrade Test FAILED ==="; exit 1; \
	fi
	@# (1) FROZEN-HOME GUARD: the superblock's audit descriptor (QDSK_AUDIT_LBA_OFF
	@# = byte 44) must name LBA count-27 = 4096-27 = 4069 on this 2 MiB image.
	@# od -tu4 reads the little-endian u32 the kernel wrote.
	@AUDIT_LBA=$$(dd if=$(DISK_UPG_IMG) bs=1 skip=44 count=4 2>/dev/null | od -An -tu4 | tr -d ' '); \
	if [ "$$AUDIT_LBA" != "4069" ]; then \
		echo "ERROR: audit home is NOT frozen at count-27 (4069) — superblock names $$AUDIT_LBA."; \
		echo "       audit_home_lba() moved; a field-geometry upgrade would now discard the ledger."; \
		echo ""; echo "=== Upgrade Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: audit ledger home frozen at LBA 4069 (count-27) — survives field growth"
	@# (2) Simulate a PRE-#177 disk: rewrite ONLY the FIELD descriptor to the old
	@# geometry — field_lba (byte 32) = count-6 = 4090, field_bytes (byte 36) =
	@# 2456 (24 + 4*8*76, the 4-region blob). Audit descriptor (byte 44+) is
	@# untouched. python3 writes the two LE u32s unambiguously — a `printf '\xNN'`
	@# is NOT portable (make runs recipes under /bin/sh, whose printf lacks \xHH
	@# and would splat the literal string across the audit descriptor).
	@python3 -c "f=open('$(DISK_UPG_IMG)','r+b'); f.seek(32); f.write((4090).to_bytes(4,'little')+(2456).to_bytes(4,'little')); f.close()"
	@echo "[boot 2] boot the current kernel on the stale-field-descriptor disk..."
	@( printf 'help\n'; sleep 8 ) | \
		timeout 10s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-drive file=$(DISK_UPG_IMG),format=raw,if=ide -serial stdio -m 128M \
		-display none -no-reboot 2>&1 | tee /tmp/qemu-upg-boot2.log || true
	@# The ledger must RESTORE — its frozen home is untouched by the field change.
	@if ! grep -q "AUDIT: restored ledger from disk" /tmp/qemu-upg-boot2.log 2>/dev/null; then \
		echo "ERROR: the authority ledger was LOST across a field-descriptor upgrade"; \
		cat /tmp/qemu-upg-boot2.log 2>/dev/null || true; \
		echo ""; echo "=== Upgrade Test FAILED ==="; exit 1; \
	fi
	@# ...and the FIELD must cold-start gracefully from the implausible descriptor.
	@if ! grep -q "FIELD: superblock names an implausible field section" /tmp/qemu-upg-boot2.log 2>/dev/null; then \
		echo "ERROR: stale field descriptor did not trigger a graceful cold start"; \
		cat /tmp/qemu-upg-boot2.log 2>/dev/null || true; \
		echo ""; echo "=== Upgrade Test FAILED ==="; exit 1; \
	fi
	@if grep -q "FIELD: restored slots from disk" /tmp/qemu-upg-boot2.log 2>/dev/null; then \
		echo "ERROR: field FALSELY restored from a pre-#177 descriptor"; \
		echo ""; echo "=== Upgrade Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: ledger restored + field cold-started across a field-geometry upgrade"
	@echo ""
	@echo "=== Disk Upgrade-Path Test PASSED ==="

# CI Smoke Test (networking): boot WITH an rtl8139 NIC on QEMU's user-mode
# network (SLIRP), and prove the link layer works end to end. SLIRP's gateway
# (10.0.2.2) always answers ARP, so an ARP request that gets a reply exercises
# the full path: PCI enumeration -> rtl8139 driver -> Ethernet TX -> SLIRP ->
# RX interrupt -> ARP parse. Both directions through the real driver.
ci-smoke-net: kernel
	@echo "=== QuantumOS Networking Smoke Test (rtl8139 + user-net) ==="
	@echo "[1/3] Booting with -device rtl8139 on QEMU user-net (+ shell nslookup + udping)..."
	@( printf 'nslookup example.com\nudping example.com\n'; sleep 15 ) | timeout 17s qemu-system-x86_64 \
		-kernel $(BUILD_DIR)/kernel.elf32 \
		-netdev user,id=n0 -device rtl8139,netdev=n0 -serial stdio -m 128M \
		-display none -no-reboot 2>&1 | tee /tmp/qemu-net.log || true
	@echo ""
	@echo "[2/3] The NIC must be found and brought up..."
	@if ! grep -q "NET: rtl8139 up" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: rtl8139 was not detected/brought up (NET: rtl8139 up)"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: rtl8139 NIC detected via PCI and brought up"
	@echo "[3/3] ARP must resolve the SLIRP gateway (TX + RX round trip)..."
	@if grep -q "NET: ARP 10.0.2.2 timed out" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: ARP timed out — no reply from the SLIRP gateway"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "NET: ARP 10.0.2.2 is at MAC" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: ARP did not resolve the gateway (NET: ARP 10.0.2.2 is at MAC)"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: ARP-resolved 10.0.2.2 — link layer works both directions"
	@echo "[4/4] DHCP must obtain the SLIRP lease (IPv4 + UDP + checksums)..."
	@if grep -q "NET: DHCP timed out" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: DHCP timed out — no lease from the SLIRP server"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "NET: DHCP lease 10.0.2.15" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: DHCP did not obtain the lease (NET: DHCP lease 10.0.2.15)"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: DHCP lease 10.0.2.15 obtained — IPv4/UDP stack works end to end"
	@echo "[5/6] ICMP echo to the gateway must round-trip (unicast IPv4 + checksum)..."
	@if ! grep -q "NET: ping 10.0.2.2 reply received" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: ICMP echo did not round-trip (NET: ping 10.0.2.2 reply received)"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: ICMP ping 10.0.2.2 round-tripped — unicast IP works"
	@# The DNS capstone resolves a hostname through SLIRP's DNS proxy, which
	@# forwards to the runner's resolver — a real end-to-end hostname lookup.
	@echo "[6/6] DNS must resolve a hostname to an A record (the capstone)..."
	@if grep -q "NET: DNS example.com timed out" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: DNS query timed out — no answer from the resolver"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@if ! grep -qE "NET: DNS example.com -> [0-9]+\.[0-9]+\.[0-9]+\.[0-9]+" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: DNS did not resolve example.com to an A record"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: DNS resolved example.com to an A record — full stack proven"
	@# Ring-3 network access (#77): the piped 'nslookup example.com' must
	@# resolve a hostname from the SHELL via SYS_RESOLVE (not the boot
	@# self-test) — the "qsh:" prefix distinguishes it from the "NET:" line.
	@if ! grep -qE "qsh: example.com -> [0-9]+\.[0-9]+\.[0-9]+\.[0-9]+" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: the shell's nslookup did not resolve (qsh: example.com -> a.b.c.d)"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: ring-3 nslookup resolved a hostname via SYS_RESOLVE"
	@# Ring-3 UDP sockets (epic #80): the piped 'udping example.com' does DNS
	@# ENTIRELY IN USERSPACE — builds the query in ring 3, UDP_SENDTO to
	@# 10.0.2.3:53, UDP_RECVFROM the raw reply, parses it in ring 3. The
	@# byte-count line proves raw datagrams flow both ways through the
	@# socket API. Hermeticity: SLIRP only FORWARDS DNS to the runner's
	@# resolver (it synthesizes nothing), so these gates share the
	@# runner-resolver dependency of the DNS gates above — no new risk
	@# class, since those already hard-fail without a resolver.
	@if grep -q "qsh: udping: timed out" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: udping got no datagram back (qsh: udping: timed out)"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@if ! grep -qE "qsh: udp [0-9]+ bytes from 10.0.2.3:53" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: the socket API did not carry a datagram round trip (qsh: udp N bytes from 10.0.2.3:53)"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: ring-3 UDP sockets carried raw datagrams both ways (SYS_UDP)"
	@if ! grep -qE "qsh: udpdns example.com -> [0-9]+\.[0-9]+\.[0-9]+\.[0-9]+" /tmp/qemu-net.log 2>/dev/null; then \
		echo "ERROR: userspace DNS over SYS_UDP did not parse an A record (qsh: udpdns example.com -> a.b.c.d)"; \
		echo "Boot log:"; cat /tmp/qemu-net.log 2>/dev/null || true; \
		echo ""; echo "=== Networking Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: userspace DNS client over ring-3 UDP sockets parsed an A record"
	@echo ""
	@echo "=== Networking Test PASSED ==="

# CI Smoke Test (TCP client — epic #82): the shell fetches a web page.
#
# The hermetic proof runs a loopback HTTP server on the runner and has the
# guest fetch from it: SLIRP forwards a guest connection to 10.0.2.2:PORT to
# the host's 127.0.0.1:PORT, so this exercises a full three-way handshake,
# bidirectional data, and FIN teardown against a REAL TCP peer with zero
# external network. The server-start, readiness probe, QEMU boot, and server
# kill all live in ONE recipe line (a single shell) so the background server
# is alive for the whole QEMU lifetime and reaped on exit — Make runs each
# recipe line in its own shell, so a `&` job started on a separate line would
# orphan and race. `http example.com` follows as the (non-hermetic, same class
# as the DNS gates) real-world capstone.
ci-smoke-http: kernel
	@echo "=== QuantumOS TCP Client Smoke Test (http fetch) ==="
	@echo "[1/2] Loopback HTTP server up, then boot QEMU to fetch from it..."
	@set -e; \
	 mkdir -p /tmp/qos-httproot; \
	 printf 'quantumos tcp fetch ok\n' > /tmp/qos-httproot/index.html; \
	 ( cd /tmp/qos-httproot && python3 -m http.server 18080 --bind 127.0.0.1 ) >/tmp/qos-httpd.log 2>&1 & \
	 HPID=$$!; \
	 trap 'kill $$HPID 2>/dev/null || true' EXIT; \
	 ok=0; for i in $$(seq 1 50); do if curl -s -o /dev/null http://127.0.0.1:18080/; then ok=1; break; fi; sleep 0.2; done; \
	 [ $$ok = 1 ] || { echo "ERROR: loopback httpd never came up"; cat /tmp/qos-httpd.log 2>/dev/null || true; exit 1; }; \
	 ( printf 'http 10.0.2.2 18080\nhttp example.com\n'; sleep 20 ) | timeout 22s qemu-system-x86_64 \
		-kernel $(BUILD_DIR)/kernel.elf32 \
		-netdev user,id=n0 -device rtl8139,netdev=n0 -serial stdio -m 128M \
		-display none -no-reboot 2>&1 | tee /tmp/qemu-http.log || true
	@echo ""
	@echo "[2/2] Validating the fetch..."
	@# Hermetic hard proof: the three-way handshake + GET + response + FIN
	@# against the loopback server. Tolerant HTTP/1.[01] so python's
	@# protocol_version isn't hardcoded; the host label carries no port.
	@if ! grep -qE "qsh: http 10.0.2.2 -> HTTP/1\.[01] 200" /tmp/qemu-http.log 2>/dev/null; then \
		echo "ERROR: hermetic TCP fetch did not complete (qsh: http 10.0.2.2 -> HTTP/1.x 200)"; \
		echo "Boot log:"; cat /tmp/qemu-http.log 2>/dev/null || true; \
		echo "Server log:"; cat /tmp/qos-httpd.log 2>/dev/null || true; \
		echo ""; echo "=== TCP Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: three-way handshake + GET + response + FIN against a loopback server (hermetic)"
	@if ! grep -qE "qsh: http 10.0.2.2: [0-9]+ bytes received" /tmp/qemu-http.log 2>/dev/null; then \
		echo "ERROR: hermetic fetch did not reach EOF (qsh: http 10.0.2.2: N bytes received)"; \
		echo "Boot log:"; cat /tmp/qemu-http.log 2>/dev/null || true; \
		echo ""; echo "=== TCP Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: bidirectional data + clean teardown (byte count at EOF)"
	@# Real-world capstone: same non-hermetic runner-egress class as the
	@# existing example.com DNS gate.
	@if ! grep -qE "qsh: http example.com -> HTTP/1\.[01] 200" /tmp/qemu-http.log 2>/dev/null; then \
		echo "ERROR: real-world fetch failed (qsh: http example.com -> HTTP/1.x 200)"; \
		echo "Boot log:"; cat /tmp/qemu-http.log 2>/dev/null || true; \
		echo ""; echo "=== TCP Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: fetched http://example.com/ over TCP — QuantumOS reads the web"
	@echo ""
	@echo "=== TCP Test PASSED ==="

# CI Smoke Test (TCP server + httpd — epic #98): the host fetches a live
# status page FROM QuantumOS. QEMU SLIRP hostfwd maps host 127.0.0.1:18081
# to the guest's DHCP address :8080, where the ring-3 httpd service holds
# the kernel's single passive-open listener. Design notes baked in from the
# epic-98 attack panel:
#  - SLIRP accepts the HOST side of a hostfwd connection immediately, even
#    before the guest listens — a pre-listen curl burns its full --max-time
#    (never a fast refused). So the retry loops are WALL-CLOCK deadlines
#    with a QEMU-liveness check, not iteration counts.
#  - After each serve, the singleton server conn spends ~1s in TIME_WAIT
#    before re-listening; a SYN in that gap is silently dropped. BOTH curls
#    therefore run retry loops.
#  - Anti-vacuity: success requires the body sentinel ("QuantumOS status"),
#    a served= counter that STRICTLY RISES across the two fetches (a static
#    page or a stranger's server cannot satisfy it; strictly-greater, not
#    +1, because a timed-out attempt may have been served invisibly), and
#    uptime=[1-9] on the SECOND body (guaranteed >=1s after boot by the
#    TIME_WAIT gap itself — the first body may honestly read uptime=0s).
#  - QEMU is killed and reaped INSIDE the single recipe block before the
#    assertion lines read the logs (each later @-line is its own shell).
ci-smoke-httpd: kernel
	@echo "=== QuantumOS TCP Server Smoke Test (httpd status page) ==="
	@echo "[1/2] Boot with hostfwd, fetch the status page twice from the host..."
	@set -e; \
	 rm -f /tmp/qemu-httpd.log /tmp/qos-httpd-1.txt /tmp/qos-httpd-2.txt; \
	 ( sleep 120 ) | timeout 125s qemu-system-x86_64 \
		-kernel $(BUILD_DIR)/kernel.elf32 \
		-netdev user,id=n0,hostfwd=tcp:127.0.0.1:18081-:8080 \
		-device rtl8139,netdev=n0 \
		-serial stdio -m 128M -display none -no-reboot > /tmp/qemu-httpd.log 2>&1 & \
	 QPID=$$!; \
	 trap 'kill $$QPID 2>/dev/null || true' EXIT; \
	 deadline=$$(( $$(date +%s) + 75 )); ok1=0; \
	 while [ $$(date +%s) -lt $$deadline ]; do \
	   kill -0 $$QPID 2>/dev/null || break; \
	   body=$$(curl -s --max-time 2 http://127.0.0.1:18081/ 2>/dev/null || true); \
	   if printf '%s' "$$body" | grep -q "QuantumOS status"; then \
	     printf '%s\n' "$$body" > /tmp/qos-httpd-1.txt; ok1=1; break; \
	   fi; \
	   sleep 0.5; \
	 done; \
	 if [ $$ok1 != 1 ]; then \
	   echo "ERROR: first fetch never returned the status page"; \
	   echo "Boot log:"; cat /tmp/qemu-httpd.log 2>/dev/null || true; \
	   echo "=== TCP Server Test FAILED ==="; exit 1; \
	 fi; \
	 s1=$$(grep -oE 'served=[0-9]+' /tmp/qos-httpd-1.txt | head -1 | cut -d= -f2); \
	 [ -n "$$s1" ] || { echo "ERROR: first body carried no served= counter"; \
	   cat /tmp/qos-httpd-1.txt; exit 1; }; \
	 deadline=$$(( $$(date +%s) + 40 )); ok2=0; \
	 while [ $$(date +%s) -lt $$deadline ]; do \
	   kill -0 $$QPID 2>/dev/null || break; \
	   body=$$(curl -s --max-time 2 http://127.0.0.1:18081/ 2>/dev/null || true); \
	   s2=$$(printf '%s' "$$body" | grep -oE 'served=[0-9]+' | head -1 | cut -d= -f2); \
	   if [ -n "$$s2" ] && [ "$$s2" -gt "$$s1" ]; then \
	     printf '%s\n' "$$body" > /tmp/qos-httpd-2.txt; ok2=1; break; \
	   fi; \
	   sleep 0.5; \
	 done; \
	 kill $$QPID 2>/dev/null || true; wait $$QPID 2>/dev/null || true; \
	 if [ $$ok2 != 1 ]; then \
	   echo "ERROR: second fetch never showed a RISING served counter (re-listen broken?)"; \
	   echo "First body:"; cat /tmp/qos-httpd-1.txt 2>/dev/null || true; \
	   echo "Boot log:"; cat /tmp/qemu-httpd.log 2>/dev/null || true; \
	   echo "=== TCP Server Test FAILED ==="; exit 1; \
	 fi
	@echo ""
	@echo "[2/2] Validating..."
	@# Service actually started (failure-mode discriminator vs network issues).
	@if ! grep -q "httpd (ring 3) serving the status page" /tmp/qemu-httpd.log 2>/dev/null; then \
		echo "ERROR: httpd service never started"; \
		cat /tmp/qemu-httpd.log 2>/dev/null || true; \
		echo "=== TCP Server Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: httpd service up with the network capability"
	@# Live value: uptime computed at request time, strictly positive on the
	@# second serve (>= 1s guaranteed by the TIME_WAIT gap between serves).
	@if ! grep -qE "uptime=[1-9][0-9]*s" /tmp/qos-httpd-2.txt 2>/dev/null; then \
		echo "ERROR: second body has no live uptime"; \
		cat /tmp/qos-httpd-2.txt 2>/dev/null || true; \
		echo "=== TCP Server Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: passive open + accept + live body + rising served counter"
	@echo "         (host curl -> SLIRP hostfwd -> QuantumOS httpd, twice)"
	@echo ""
	@echo "=== TCP Server Test PASSED ==="

# CI Smoke Test (guest-to-guest networking, epic #97 prerequisite): boot TWO
# QuantumOS guests joined by a QEMU socket netdev (a raw L2 segment with NO
# SLIRP, so no DHCP and no ARP proxy), each with a distinct static IP and a
# distinct MAC. Each runs `net2 <peer>`, sending a UDP probe to the other and
# polling for the other's probe. A received datagram whose source is the
# PEER's IP (different from our own) can only mean our ARP responder answered
# the peer's request and the on-link route delivered — it is impossible to
# fake by loopback. Both logs must show it: one-sided success is a failure.
# The `-netdev socket,udp=` mode is order-independent (no listener/connect
# race); frames sent before both guests are up are simply lost, which UDP
# tolerates by design.
ci-smoke-2net: kernel
	@echo "=== QuantumOS Guest-to-Guest Networking Test (two nodes, one wire) ==="
	@# One shell for the whole dance (make runs each recipe line in its own
	@# shell, so the two background QEMUs and the wait MUST share this block).
	@# No set -e: `timeout` returns 124 when it reaps the guest, the normal
	@# end of a headless boot — the grep gates below are the verdict.
	@rm -f /tmp/qos-2net-a.log /tmp/qos-2net-b.log; \
	 ( printf 'net2 10.0.0.2\n'; sleep 30 ) | timeout 34s qemu-system-x86_64 \
		-kernel $(BUILD_DIR)/kernel.elf32 -append "ip=10.0.0.1 quiet" \
		-netdev socket,id=n0,udp=127.0.0.1:7811,localaddr=127.0.0.1:7810 \
		-device rtl8139,netdev=n0,mac=52:54:00:00:97:01 \
		-serial stdio -m 128M -display none -no-reboot > /tmp/qos-2net-a.log 2>&1 & \
	 APID=$$!; \
	 ( printf 'net2 10.0.0.1\n'; sleep 30 ) | timeout 34s qemu-system-x86_64 \
		-kernel $(BUILD_DIR)/kernel.elf32 -append "ip=10.0.0.2 quiet" \
		-netdev socket,id=n0,udp=127.0.0.1:7810,localaddr=127.0.0.1:7811 \
		-device rtl8139,netdev=n0,mac=52:54:00:00:97:02 \
		-serial stdio -m 128M -display none -no-reboot > /tmp/qos-2net-b.log 2>&1 & \
	 BPID=$$!; \
	 wait $$APID || true; wait $$BPID || true
	@if ! grep -q "NET: static ip 10.0.0.1" /tmp/qos-2net-a.log 2>/dev/null || \
	    ! grep -q "NET: static ip 10.0.0.2" /tmp/qos-2net-b.log 2>/dev/null; then \
		echo "ERROR: static IP provenance missing on one or both guests"; \
		echo "--- A ---"; cat /tmp/qos-2net-a.log 2>/dev/null || true; \
		echo "--- B ---"; cat /tmp/qos-2net-b.log 2>/dev/null || true; \
		echo "=== 2-Node Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: both guests configured static IPs with no DHCP server"
	@if ! grep -q "NET2: probe from 10.0.0.2" /tmp/qos-2net-a.log 2>/dev/null; then \
		echo "ERROR: guest A never received guest B's probe"; \
		echo "--- A ---"; cat /tmp/qos-2net-a.log 2>/dev/null || true; \
		echo "=== 2-Node Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "NET2: probe from 10.0.0.1" /tmp/qos-2net-b.log 2>/dev/null; then \
		echo "ERROR: guest B never received guest A's probe"; \
		echo "--- B ---"; cat /tmp/qos-2net-b.log 2>/dev/null || true; \
		echo "=== 2-Node Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: both guests exchanged UDP datagrams over a raw L2 — ARP responder + static route PROVEN"
	@echo ""
	@echo "=== 2-Node Test PASSED ==="

# CI Smoke Test (field coupling, epic #97): two QuantumOS guests couple their
# ghostd oscillator fields over UDP. Each is booted with a DIFFERENT qseed, so
# ghostd seeds its field divergently when coupling engages — the cross-node
# order parameter R_x therefore STARTS low (the two fields are uncorrelated)
# and can only RISE because fieldsyncd is carrying the peer's phases over the
# wire. This is the anti-vacuous design the review demanded: two identical
# frozen fields would read R_x=1.0 from t=0 and prove nothing, so the gate
# asserts BOTH a low early sample AND convergence on BOTH nodes. The local
# recall gates (GHOSTD 3/3, kannakad) must also still pass on each guest.
ci-smoke-fieldsync: kernel
	@echo "=== QuantumOS Two-Node Field Coupling Test (two kernels, one field) ==="
	@rm -f /tmp/qos-fs-a.log /tmp/qos-fs-b.log; \
	 ( sleep 30 ) | timeout 34s qemu-system-x86_64 \
		-kernel $(BUILD_DIR)/kernel.elf32 \
		-append "ip=10.0.0.1 peer=10.0.0.2 qseed=1111111111111111" \
		-netdev socket,id=n0,udp=127.0.0.1:7811,localaddr=127.0.0.1:7810 \
		-device rtl8139,netdev=n0,mac=52:54:00:00:97:01 \
		-serial stdio -m 128M -display none -no-reboot > /tmp/qos-fs-a.log 2>&1 & \
	 APID=$$!; \
	 ( sleep 30 ) | timeout 34s qemu-system-x86_64 \
		-kernel $(BUILD_DIR)/kernel.elf32 \
		-append "ip=10.0.0.2 peer=10.0.0.1 qseed=8888888888888888" \
		-netdev socket,id=n0,udp=127.0.0.1:7810,localaddr=127.0.0.1:7811 \
		-device rtl8139,netdev=n0,mac=52:54:00:00:97:02 \
		-serial stdio -m 128M -display none -no-reboot > /tmp/qos-fs-b.log 2>&1 & \
	 BPID=$$!; \
	 wait $$APID || true; wait $$BPID || true
	@# 1. Each node received the OTHER's phase frames (real reception, peer IP).
	@if ! grep -q "FIELDSYNC: frame from 10.0.0.2" /tmp/qos-fs-a.log 2>/dev/null || \
	    ! grep -q "FIELDSYNC: frame from 10.0.0.1" /tmp/qos-fs-b.log 2>/dev/null; then \
		echo "ERROR: a node never received the peer's phase frames"; \
		echo "--- A ---"; grep FIELDSYNC /tmp/qos-fs-a.log 2>/dev/null | tail -8 || true; \
		echo "--- B ---"; grep FIELDSYNC /tmp/qos-fs-b.log 2>/dev/null | tail -8 || true; \
		echo "=== Field Coupling Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: both nodes received the peer's field phases over UDP"
	@# 2. Non-vacuity: each field STARTED divergent — a sub-0.50 R_x sample.
	@if ! grep -qE "FIELDSYNC: R_x=0\.[0-4][0-9] " /tmp/qos-fs-a.log 2>/dev/null || \
	    ! grep -qE "FIELDSYNC: R_x=0\.[0-4][0-9] " /tmp/qos-fs-b.log 2>/dev/null; then \
		echo "ERROR: no low R_x sample — fields were not divergent, gate would be vacuous"; \
		echo "--- A ---"; grep "R_x=" /tmp/qos-fs-a.log 2>/dev/null | head -6 || true; \
		echo "--- B ---"; grep "R_x=" /tmp/qos-fs-b.log 2>/dev/null | head -6 || true; \
		echo "=== Field Coupling Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: both fields started divergent (low R_x) — the gate is non-vacuous"
	@# 3. Both nodes SYNCHRONIZED (R_x rose past 0.80 through the coupling).
	@if ! grep -q "FIELDSYNC: SYNCHRONIZED (R_x>=0.80)" /tmp/qos-fs-a.log 2>/dev/null || \
	    ! grep -q "FIELDSYNC: SYNCHRONIZED (R_x>=0.80)" /tmp/qos-fs-b.log 2>/dev/null; then \
		echo "ERROR: the two fields did not synchronize"; \
		echo "--- A ---"; grep "R_x=" /tmp/qos-fs-a.log 2>/dev/null | tail -6 || true; \
		echo "--- B ---"; grep "R_x=" /tmp/qos-fs-b.log 2>/dev/null | tail -6 || true; \
		echo "=== Field Coupling Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: both fields synchronized over the wire (R_x >= 0.80)"
	@# 4. The local recall gates MUST still pass on both nodes — coupling must
	@#    not have broken ghostd's own associative memory.
	@if ! grep -q "GHOSTD: 3/3 RECALL OK" /tmp/qos-fs-a.log 2>/dev/null || \
	    ! grep -q "GHOSTD: 3/3 RECALL OK" /tmp/qos-fs-b.log 2>/dev/null; then \
		echo "ERROR: coupling broke a node's local ghostd recall"; \
		echo "=== Field Coupling Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: local ghostd recall still passes on both nodes — coupling is additive"
	@echo ""
	@echo "=== Field Coupling Test PASSED — TWO KERNELS, ONE FIELD ==="

# CI Smoke Test (quiet boot): boot with `-append quiet` and prove the interactive
# console is CLEAN — the periodic timer-tick heartbeat and the demo services'
# steady-state chatter are silenced — WHILE the shell still comes up and answers
# a command. This is the clean-shell contract; the default boot (which keeps all
# that output, and whose gates depend on it) is unchanged.
ci-smoke-quiet: kernel
	@echo "=== QuantumOS Quiet-Boot Smoke Test (clean interactive console) ==="
	@echo "[1/3] Booting with -append quiet and typing 'help'..."
	@( printf 'help\n'; sleep 6 ) | timeout 8s qemu-system-x86_64 \
		-kernel $(BUILD_DIR)/kernel.elf32 -append quiet \
		-serial stdio -m 128M -display none -no-reboot 2>&1 | tee /tmp/qemu-quiet.log || true
	@echo "[2/3] The shell must still come up and answer 'help'..."
	@if ! grep -q "interactive shell ready" /tmp/qemu-quiet.log 2>/dev/null; then \
		echo "ERROR: qsh did not come up under a quiet boot"; \
		echo "Boot log:"; cat /tmp/qemu-quiet.log 2>/dev/null || true; \
		echo ""; echo "=== Quiet Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "qsh commands:" /tmp/qemu-quiet.log 2>/dev/null; then \
		echo "ERROR: 'help' produced no output — the shell is not usable under quiet"; \
		echo "Boot log:"; cat /tmp/qemu-quiet.log 2>/dev/null || true; \
		echo ""; echo "=== Quiet Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: qsh came up and answered 'help' under a quiet boot"
	@echo "[3/3] The periodic heartbeat/chatter must be SILENCED (the whole point)..."
	@if grep -q "Timer tick:" /tmp/qemu-quiet.log 2>/dev/null; then \
		echo "ERROR: quiet boot still printed the periodic timer-tick heartbeat"; \
		echo "Boot log:"; cat /tmp/qemu-quiet.log 2>/dev/null || true; \
		echo ""; echo "=== Quiet Test FAILED ==="; exit 1; \
	fi
	@if grep -q "PARADOXD: phase ->" /tmp/qemu-quiet.log 2>/dev/null; then \
		echo "ERROR: quiet boot still printed paradoxd phase chatter"; \
		echo "Boot log:"; cat /tmp/qemu-quiet.log 2>/dev/null || true; \
		echo ""; echo "=== Quiet Test FAILED ==="; exit 1; \
	fi
	@if grep -q "lambda-damp" /tmp/qemu-quiet.log 2>/dev/null; then \
		echo "ERROR: quiet boot still printed ghostd lambda-damp chatter"; \
		echo "Boot log:"; cat /tmp/qemu-quiet.log 2>/dev/null || true; \
		echo ""; echo "=== Quiet Test FAILED ==="; exit 1; \
	fi
	@# The process/service/syscall lifecycle chatter (boot_log_v) must be
	@# silenced too — it repeats as the demo services churn and would bury
	@# the interactive prompt just like the heartbeat did.
	@if grep -qE "Process created successfully|Process destroyed|reaped process|syscall: user process exited|kernel-thread (alpha|beta): alive|service started:" /tmp/qemu-quiet.log 2>/dev/null; then \
		echo "ERROR: quiet boot still printed process/service/syscall lifecycle chatter"; \
		echo "Boot log:"; cat /tmp/qemu-quiet.log 2>/dev/null || true; \
		echo ""; echo "=== Quiet Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: the periodic heartbeat + process/service/syscall chatter is silenced — a clean console"
	@echo ""
	@echo "=== Quiet Test PASSED ==="

# CI Smoke Test (resonant scheduler): rebuild WITH SCHED_RESONANT=1 and prove
# the alternate policy still boots to ready, still passes the ghostd merge gate
# (the field service works under resonant scheduling), and prints the honest
# rr-vs-resonant fairness comparison. This is the ghostd phase-5 scheduler gate.
ci-smoke-resonant:
	@echo "=== QuantumOS CI Smoke Test (resonant scheduler) ==="
	@echo ""
	@echo "[1/3] Rebuilding kernel with SCHED_RESONANT=1..."
	@$(MAKE) -s clean
	@$(MAKE) -s SCHED_RESONANT=1 kernel
	@echo "[2/3] Running QEMU boot test (10 second timeout)..."
	@timeout 10s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-serial stdio -m 128M -display none -no-reboot 2>&1 | tee /tmp/qemu-boot-resonant.log || true
	@echo ""
	@echo "[3/3] Validating boot output..."
	@if ! grep -q "QuantumOS ready" /tmp/qemu-boot-resonant.log 2>/dev/null; then \
		echo "ERROR: resonant build did not reach 'QuantumOS ready'"; \
		cat /tmp/qemu-boot-resonant.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: resonant build booted to idle loop (QuantumOS ready)"
	@# The field service must still work under the alternate scheduler.
	@if ! grep -q "GHOSTD: 3/3 RECALL OK" /tmp/qemu-boot-resonant.log 2>/dev/null; then \
		echo "ERROR: ghostd merge gate missing under resonant scheduling (GHOSTD: 3/3 RECALL OK)"; \
		cat /tmp/qemu-boot-resonant.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: ghostd gate passed under resonant scheduling (GHOSTD: 3/3 RECALL OK)"
	@# The honest comparison must be printed: round-robin baseline...
	@if ! grep -q "SCHED: policy=rr fairness=" /tmp/qemu-boot-resonant.log 2>/dev/null; then \
		echo "ERROR: round-robin fairness baseline not printed (SCHED: policy=rr fairness=)"; \
		cat /tmp/qemu-boot-resonant.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@# ...and the resonant order parameter + fairness alongside it.
	@if ! grep -q "SCHED: policy=resonant r=" /tmp/qemu-boot-resonant.log 2>/dev/null; then \
		echo "ERROR: resonant order parameter/fairness not printed (SCHED: policy=resonant r=)"; \
		cat /tmp/qemu-boot-resonant.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: honest rr-vs-resonant comparison printed:"
	@grep "SCHED: " /tmp/qemu-boot-resonant.log 2>/dev/null || true
	@echo ""
	@echo "=== Smoke Test (resonant) PASSED ==="

# CI Smoke Test (qseed mode): boot WITH a quantum-entropy handoff on the
# kernel command line and prove ghostd traces its noise to that qseed.
ci-smoke-qseed: kernel
	@echo "=== QuantumOS CI Smoke Test (qseed handoff) ==="
	@echo ""
	@echo "[1/3] Build verified: $(BUILD_DIR)/kernel.elf exists"
	@test -f $(BUILD_DIR)/kernel.elf || (echo "ERROR: Kernel not built" && exit 1)
	@# The seed is deliberately FFFFFFFFFFFFFFFC == (uint64_t)-4: the one value
	@# that collides with the EPERM errno sentinel, exercising the qsh `qseed`
	@# command's out-of-band capability probe (a raw `== -4` test misread it as a
	@# denial). `qseed` is piped in so qsh prints the value for the gate below.
	@echo "[2/3] Running QEMU boot test WITH -append qseed=FFFFFFFFFFFFFFFC (16s, qseed piped)..."
	@( printf 'qseed\n'; sleep 20 ) | \
		timeout 16s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-append "qseed=FFFFFFFFFFFFFFFC" \
		-serial stdio -m 128M -display none -no-reboot 2>&1 | tee /tmp/qemu-boot-qseed.log || true
	@echo ""
	@echo "[3/3] Validating boot output..."
	@if ! grep -q "QuantumOS ready" /tmp/qemu-boot-qseed.log 2>/dev/null; then \
		echo "ERROR: Kernel did not reach 'QuantumOS ready'"; \
		cat /tmp/qemu-boot-qseed.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if ! grep -q "GHOSTD: 3/3 RECALL OK" /tmp/qemu-boot-qseed.log 2>/dev/null; then \
		echo "ERROR: ghostd self-test gate missing (GHOSTD: 3/3 RECALL OK)"; \
		cat /tmp/qemu-boot-qseed.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@# The kernel must still echo it accepted the qseed handoff...
	@if ! grep -q "Boot entropy accepted from cmdline (qseed=)" /tmp/qemu-boot-qseed.log 2>/dev/null; then \
		echo "ERROR: kernel did not echo the qseed handoff"; \
		cat /tmp/qemu-boot-qseed.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@# ...and ghostd must trace its noise to that qseed (and NOT to a seedless PRNG).
	@if ! grep -q "GHOSTD: noise source = qseed-derived" /tmp/qemu-boot-qseed.log 2>/dev/null; then \
		echo "ERROR: ghostd did not report qseed-derived noise on a seeded boot"; \
		cat /tmp/qemu-boot-qseed.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@if grep -q "GHOSTD: noise source = prng (no qseed)" /tmp/qemu-boot-qseed.log 2>/dev/null; then \
		echo "ERROR: ghostd reported 'no qseed' despite a qseed handoff"; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: qseed handoff traced end-to-end (kernel echo + GHOSTD qseed-derived noise)"
	@# phase-3 gate still holds under a qseed boot: paradoxd resolves + couples.
	@if ! grep -q "PARADOXD: RESOLVED" /tmp/qemu-boot-qseed.log 2>/dev/null; then \
		echo "ERROR: paradoxd resolution gate missing under qseed (PARADOXD: RESOLVED)"; \
		cat /tmp/qemu-boot-qseed.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: paradoxd resolution gate passed under qseed (PARADOXD: RESOLVED)"
	@# errno/sentinel-collision gate (cross-cutting bug-hunt, errno-confusion): the
	@# qsh `qseed` command must report the ACTUAL seed FFFFFFFFFFFFFFFC, not misread
	@# it as EPERM because the value equals the -4 errno sentinel. With the raw
	@# `s == -4` test this printed "qseed denied (EPERM)"; the out-of-band cap probe
	@# fixes it. Anti-vacuous: this specific seed is the only one that trips it.
	@if ! grep -q "qsh: qseed fffffffffffffffc" /tmp/qemu-boot-qseed.log 2>/dev/null; then \
		echo "ERROR: qsh misreported the -4-collision qseed (errno/sentinel confusion)"; \
		echo "  (expected 'qsh: qseed fffffffffffffffc'; a 'denied (EPERM)' here is the bug)"; \
		cat /tmp/qemu-boot-qseed.log 2>/dev/null || true; \
		echo ""; echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: qseed errno-collision gate passed (real seed reported, not EPERM)"
	@echo ""
	@echo "=== Smoke Test (qseed) PASSED ==="

# CI Smoke Test (swarm bridge): boot with COM2 routed to a file and prove the
# ring-3 swarm_svc's Lamport-signed boot attestation verifies end-to-end — both
# seedless (qseed=none) and with a qseed handoff (attested qseed == cmdline).
# This is the one-way CI gate; the two-way PING/PONG path is proven locally via
# `make swarm-pingpong` (headless two-way serial into QEMU needs a TCP client).
ci-smoke-swarm: kernel
	@echo "=== QuantumOS CI Smoke Test (swarm bridge attestation) ==="
	@echo ""
	@echo "[seedless] boot: COM1 -> stdio, COM2 -> file..."
	@rm -f /tmp/qemu-swarm-com2.bin
	@timeout 10s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-serial stdio -serial file:/tmp/qemu-swarm-com2.bin \
		-m 128M -display none -no-reboot 2>&1 | tee /tmp/qemu-swarm.log || true
	@if ! grep -q "SWARM: boot attestation emitted" /tmp/qemu-swarm.log 2>/dev/null; then \
		echo "ERROR: swarm console gate missing (SWARM: boot attestation emitted)"; \
		cat /tmp/qemu-swarm.log 2>/dev/null || true; \
		echo "=== Smoke Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: swarm console gate present (SWARM: boot attestation emitted)"
	@python3 scripts/verify_attestation.py /tmp/qemu-swarm-com2.bin --qseed none || \
		(echo "=== Smoke Test FAILED (seedless attestation) ==="; exit 1)
	@echo "SUCCESS: seedless boot attestation verified (qseed=none)"
	@echo ""
	@echo "[qseed] boot WITH -append qseed=DEADBEEFCAFEBABE, COM2 -> file..."
	@rm -f /tmp/qemu-swarm-com2q.bin
	@timeout 10s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-append "qseed=DEADBEEFCAFEBABE" \
		-serial stdio -serial file:/tmp/qemu-swarm-com2q.bin \
		-m 128M -display none -no-reboot 2>&1 | tee /tmp/qemu-swarmq.log || true
	@python3 scripts/verify_attestation.py /tmp/qemu-swarm-com2q.bin --qseed DEADBEEFCAFEBABE || \
		(echo "=== Smoke Test FAILED (qseed attestation) ==="; exit 1)
	@echo "SUCCESS: qseed boot attestation verified (attested qseed == cmdline)"
	@echo ""
	@echo "=== Smoke Test (swarm bridge) PASSED ==="

# Local two-way exercise: COM2 as a TCP server; drive PING/PONG + a DATA request
# routed to ghostd over capability-checked IPC. Not part of CI (needs a client).
# Any model ↔ QuantumOS (scripts/qos_agent.py): a bring-your-own-key agent
# that drives a live VM through the FROZEN MCP tool surface (ADR-0020) — an
# external model is the consumer that freeze was built for, reusing
# scripts/qos_mcp.py verbatim (no tool duplication). Needs the guest built
# (kernel prereq) and your credentials. Providers: PROVIDER=anthropic (the
# default — Claude, ANTHROPIC_API_KEY or `ant auth login`) or PROVIDER=openai
# (any OpenAI-compatible endpoint — OPENAI_API_KEY, plus MODEL=... and
# optionally BASE_URL=http://localhost:11434/v1 for a keyless local Ollama).
# Pass a free-form prompt with TASK=..., or a curated preset with
# EXPERIMENT={recall,society,quantum,explore}; deps: pip install -r
# requirements-agent.txt
qos-agent: kernel
	@echo "=== $(if $(PROVIDER),$(PROVIDER),anthropic) drives QuantumOS (BYO key) ==="
	QOS_KERNEL=$(BUILD_DIR)/kernel.elf32 python3 scripts/qos_agent.py \
		$(if $(PROVIDER),--provider $(PROVIDER)) \
		$(if $(MODEL),--model "$(MODEL)") \
		$(if $(BASE_URL),--base-url "$(BASE_URL)") \
		$(if $(TASK),"$(TASK)",--experiment $(if $(EXPERIMENT),$(EXPERIMENT),recall))

# Back-compat alias: the original Claude-only spelling, same defaults.
qos-claude: qos-agent

# Prove the model→MCP→qos_bridge handshake end to end with NO API call and no
# key: spawn the MCP server, list its tools, exit. A fast sanity check that the
# agent's plumbing is intact.
qos-agent-list:
	@python3 scripts/qos_agent.py --list-tools

qos-claude-list: qos-agent-list

swarm-pingpong: kernel
	@echo "=== QuantumOS swarm bridge two-way (PING/PONG + DATA->ghostd) ==="
	@(timeout 18s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-serial file:/tmp/qemu-swarm-com1.log \
		-serial tcp:127.0.0.1:5566,server -m 128M -display none -no-reboot \
		>/dev/null 2>&1 &) ; sleep 1
	@python3 scripts/swarm_pingpong.py --host 127.0.0.1 --port 5566 --timeout 16

# CI Smoke Test (MCP server — epic #99): drive the full agent-facing lifecycle
# through qos_bridge (the exact code path the FastMCP tools delegate to), so a
# real MCP agent's boot->status->imprint->recall->run->tamper->shutdown is
# proven end to end. The test OWNS its QEMU process (Python, not a shell
# background job — the #98 single-owner lesson one level up): it binds a
# listener, launches QEMU as a serial CLIENT for COM2 (byte-0 attestation
# capture, no free-port race), drains COM1 on a thread (no pipe deadlock), and
# reaps QEMU in a finally + atexit + a SIGTERM/SIGINT handler + PR_SET_PDEATHSIG
# (so a `timeout -k` SIGTERM, then SIGKILL, still leaves no orphan — no shell
# trap is needed, and a trap matching the qemu cmdline would self-match this
# recipe's own shell). Stdlib-only (no `mcp` package) so it runs in the
# integration job with no pip. Anti-vacuous: verified+qseed-bound attestation,
# live>=3 ghostd field, TWO discriminated recalls, hello's unique exit-42, and
# a CRC-valid crypto-tamper that must be refused.
# The gate timeout lives in exactly ONE place (MCP_GATE_TIMEOUT) and the CI
# integration job invokes `make ci-smoke-mcp-gate` with the SAME target — no
# duplicated `timeout ... python3 test_qos_mcp.py` literal to drift out of sync
# (the #93 Makefile/ci.yml desync lesson). ci-smoke-mcp-gate has NO `kernel`
# prerequisite, so the artifact-based CI job does not rebuild the kernel.
MCP_GATE_TIMEOUT ?= 240s

ci-smoke-mcp: kernel ci-smoke-mcp-gate

ci-smoke-mcp-gate:
	@echo "=== QuantumOS MCP Server Lifecycle Test (epics #99, #125) ==="
	timeout -k 5 $(MCP_GATE_TIMEOUT) python3 scripts/test_qos_mcp.py

# CI Smoke Test (COM2 quantum submit, epic #149 B1): a host agent frames an
# OPAQUE circuit over the attested COM2 bridge; swarm_svc forwards it to the
# SYS_QPU broker, qpud runs it on the exact integer engine, and the exact result
# comes back over the wire the host never typed. Two DISTINCT circuits (Bell +
# Grover-3q) rule out a hardcoded bridge; the Grover-3q wire result is
# cross-checked against the host PennyLane gateway (if installed); a malformed
# circuit returns a wire-visible broker error; and the qsub quota is enforced
# over the wire with the swarm-svc incarnation asserted stable across the burst.
ci-smoke-qsubmit: kernel
	@echo "=== QuantumOS COM2 Quantum-Submit Test (epic #149 B1) ==="
	QOS_KERNEL=$(BUILD_DIR)/kernel.elf32 python3 scripts/test_qos_qsubmit.py

# Swarm-plane key channel (ADR-0019 increment B): the host admits the group
# session key over the attested COM2 channel; swarm_svc forwards it to
# fieldsyncd over the boot-minted IPC send-cap. Proves the key reaches
# fieldsyncd end to end (non-vacuous: no key installed before the admit).
ci-smoke-keyadmit: kernel
	@echo "=== QuantumOS swarm-plane key-admit Test (ADR-0019) ==="
	QOS_KERNEL=$(BUILD_DIR)/kernel.elf32 python3 scripts/test_qos_keyadmit.py

# COM2 DATA-reply authentication (ADR-0019 Extension): a keyed VM's STATUS reply
# carries a per-request nonce echo + HMAC tag the host verifies, so a tool
# RESULT is provably fresh + unforged. Positive + replay-reject + forgery-reject
# + fail-open (unkeyed status still plain). Revert-confirmed (neutering the guest
# auth reply makes the keyed host reject the stripped-tag reply).
ci-smoke-replyauth: kernel
	@echo "=== QuantumOS COM2 reply-auth Test (ADR-0019 Extension) ==="
	QOS_KERNEL=$(BUILD_DIR)/kernel.elf32 python3 scripts/test_qos_replyauth.py

# COM2 QSUBMIT reply-auth (ADR-0019 Extension): the LIVE circuit-submit path.
# A keyed VM's QSUBMIT reply carries a nonce echo + HMAC across BOTH the async
# DONE (nonce stashed at accept) and a synchronous error (in-hand nonce), so a
# forged job result is rejected. Positive DONE + sync-error + replay-reject +
# forgery-reject + fail-open (unkeyed submit still plain). Revert-confirmed
# (neutering the guest emit makes the keyed host reject the nonce-less DONE).
ci-smoke-qsubmit-replyauth: kernel
	@echo "=== QuantumOS COM2 QSUBMIT reply-auth Test (ADR-0019 Extension) ==="
	QOS_KERNEL=$(BUILD_DIR)/kernel.elf32 python3 scripts/test_qos_qsubmit_replyauth.py

# Attestation-by-default (ADR-0019 Extension): QosVM.attest() turns the built
# reply-auth ON for a session, so the agent-facing MCP tools (qos_boot admits a
# key, qos_status/qos_qpu_run) return attested-fresh results by default — not
# merely boot-verified. baseline-dormant -> attest -> attested STATUS + attested
# qpu_run. Revert-confirmed (a no-op attest reddens the status/qpu legs).
ci-smoke-attested: kernel
	@echo "=== QuantumOS attestation-by-default Test (ADR-0019 Extension) ==="
	QOS_KERNEL=$(BUILD_DIR)/kernel.elf32 python3 scripts/test_qos_attested.py

# COM2 round-trip latency gate (ADR-0022 prereq 1): asserts a bounded one-hop
# PING/PONG median (the transport + scheduler-cadence floor under every tool
# call) and records the STATUS baseline. Gross-regression guard, not a tight
# perf tracker. Revert-confirmed: bumping SCHED_QUANTUM_TICKS reddens it.
ci-smoke-latency: kernel
	@echo "=== QuantumOS COM2 latency Test (ADR-0022) ==="
	QOS_KERNEL=$(BUILD_DIR)/kernel.elf32 python3 scripts/test_qos_latency.py

# Scheduler perf/stability baseline gate (ADR-0022 prereq 2). Emits the tick-
# normalized SCHED baseline (preempt/switch per 1000 guest ticks, max reschedule
# gap, run-count spread) and ASSERTS the calibration-selected scalar — preemptions
# per 1000 guest ticks, ~200 at the default quantum — within [100, 600], plus a
# non-vacuous liveness floor. Guest-tick-normalized, so host-CPU-invariant on slow
# CI. Revert-confirmed: SCHED_QUANTUM_TICKS 5 -> 20 drops the rate to ~50 and reddens
# the floor. QOS_SCHED_LOAD=1 drives COM2 PING load during sampling.
ci-smoke-sched: kernel
	@echo "=== QuantumOS scheduler baseline recorder (ADR-0022 prereq 2) ==="
	QOS_KERNEL=$(BUILD_DIR)/kernel.elf32 python3 scripts/test_qos_sched.py

# Authenticated FSYN (ADR-0019 increment B-frame-auth): boots two 2-VM societies
# and proves the per-frame HMAC gates coupling — same key couples, different
# keys REJECT each other and do not synchronize. Revert-confirmed (neutering the
# MAC check makes the different-key pair couple, reddening the negative case).
# Single-sourced timeout so ci.yml never duplicates a `timeout` literal.
KEYAUTH_GATE_TIMEOUT ?= 420s

ci-smoke-keyauth: kernel ci-smoke-keyauth-gate

ci-smoke-keyauth-gate:
	@echo "=== QuantumOS authenticated-FSYN Test (ADR-0019 B-frame-auth) ==="
	timeout -k 5 $(KEYAUTH_GATE_TIMEOUT) python3 scripts/test_qos_keyauth.py

# Agent society (epic #131): boots TWO attested QuantumOS VMs coupled into one
# holographic field and proves they synchronize (R_x >= 0.80 from a divergent
# start) under the Python harness — the MCP-driven generalization of
# ci-smoke-fieldsync, with attestation-gated reporting. Single-sourced timeout
# like MCP_GATE_TIMEOUT so ci.yml invokes the SAME command (the #93 lesson); the
# gate reaps both QEMUs on timeout via the society's single reaper.
SOCIETY_GATE_TIMEOUT ?= 180s

ci-smoke-society: kernel ci-smoke-society-gate

ci-smoke-society-gate:
	@echo "=== QuantumOS Agent Society Test (epic #131) ==="
	timeout -k 5 $(SOCIETY_GATE_TIMEOUT) python3 scripts/test_qos_society.py

# Society of societies (epic #178): TWO kernels, each running its OWN complete
# agent society (agentdemo token), coupled AND exchanging qseed-salted society
# aggregates over the FSYP wire — host-verified cross-appearance. A separate
# gate from ci-smoke-society so the demo-free society timing stays untouched
# (design-review finding); generous timeout because the agentdemo work rides
# the same boots (the #171 lesson). Single-sourced like the others.
SOCIETY_AGENTS_GATE_TIMEOUT ?= 420s

ci-smoke-society-agents: kernel ci-smoke-society-agents-gate

ci-smoke-society-agents-gate:
	@echo "=== QuantumOS Society-of-Societies Test (epic #178, two societies) ==="
	timeout -k 5 $(SOCIETY_AGENTS_GATE_TIMEOUT) python3 scripts/test_qos_society_agents.py

# N-way society-of-societies (societies epic increment 3; epic #178 at N>2): THREE
# agentdemo VMs each run a full local sub-agent society and EXCHANGE their
# qseed-salted aggregates over FSYP full-mesh — each node must show BOTH peers'
# aggregates (N-1=2), host-recomputed and value-attributed (the FSYP print carries
# no source, and FSYP stays source-IP-filtered/unauthenticated per ADR-0019, so
# the host value recompute is the integrity check; a FIXTURE precheck requires all
# 3 expected values pairwise-distinct). Larger budget than the 2-VM gate: three
# agentdemo boots (QPU+spawns+field ops each) ride the same VMs on an
# oversubscribed runner. Revert-confirm: boot one member WITHOUT agentdemo (drop
# it from extra_tokens) -> its value never reaches the others -> the N-1 assertion
# reddens; or collide two qseeds -> the pairwise-distinct fixture reddens. FAILS
# LOUD if host multicast is unavailable.
SOCIETY_AGENTS_N_GATE_TIMEOUT ?= 720s

ci-smoke-society-agents-n: kernel ci-smoke-society-agents-n-gate

ci-smoke-society-agents-n-gate:
	@echo "=== QuantumOS N-Way Society-of-Societies Test (epic #178 at N=3) ==="
	timeout -k 5 $(SOCIETY_AGENTS_N_GATE_TIMEOUT) python3 scripts/test_qos_society_agents_n.py

# N-way society (epic #139): THREE kernels mean-field-couple on a shared mcast
# L2. A separate gate/timeout from the 2-node one (a 3-body field converges
# slower and the mcast transport differs) — the proven ci-smoke-society stays
# untouched (the #93 single-source lesson). SEPARATE timeout, single-sourced.
SOCIETY3_GATE_TIMEOUT ?= 300s

ci-smoke-society3: kernel ci-smoke-society3-gate

ci-smoke-society3-gate:
	@echo "=== QuantumOS N-Way Society Test (epic #139, three kernels) ==="
	timeout -k 5 $(SOCIETY3_GATE_TIMEOUT) python3 scripts/test_qos_society3.py

# N=4 society — the DOCUMENTED configuration ceiling (societies epic increment 2):
# FOUR kernels on a shared mcast L2, the max the host bridge addresses
# (_NET/_MAC are 4 entries) with each node's peer set at N-1=3 against a
# 4-slot ghostd table. Proves the full 4-cycle mesh (12 directed frame
# observations, per-IP in boot_n), four distinct attested identities, and
# min-pairwise R_x >= 0.80 from divergent (< 0.50) per-node starts, then a clean
# 4-way reap. Deliberately does NOT assert slot-full/eviction (unreachable at
# 3/4 slots, and unobservable). Larger timeout than society3: 4 QEMU-TCG VMs
# oversubscribe the runner's cores and dilate convergence wall-clock. FAILS LOUD
# if host multicast is unavailable. Revert-confirm: drop one member's qseed to a
# duplicate -> boot_n's distinct-seed guard reddens; shrink the roster to 3 ->
# this is society3, not the ceiling. See ADR-0014 for the true N=5 constant wall.
SOCIETY4_GATE_TIMEOUT ?= 600s

ci-smoke-society4: kernel ci-smoke-society4-gate

ci-smoke-society4-gate:
	@echo "=== QuantumOS N=4 Society Test (societies epic, the documented ceiling) ==="
	timeout -k 5 $(SOCIETY4_GATE_TIMEOUT) python3 scripts/test_qos_society4.py

# N-way authenticated swarm plane (epic #139 + ADR-0019): THREE kernels on a
# shared mcast L2, and the host-admitted group session key gating the FIELD wire
# across N>2. The gate does NOT assert on R_x/SYNCHRONIZED (a keyless node
# one-way-locks to the keyed pair and prints SYNCHRONIZED — vacuous); it asserts
# on frame-ADMISSION signals offset-anchored at each member's FSKEY: (a) key A,B
# only -> FSAUTH bad-MAC on A,B + zero 'frame from C' admitted, while C still
# receives from A,B (one-way lock, positively confirmed); (b) key C too -> C's
# frames resume + fresh post-key sync; (c) a REAL host mcast capture-replay ->
# per-reason replay marker + rekey line. A separate single-sourced timeout; the
# proven society3/keyauth gates stay untouched (the #93 single-source lesson).
# Revert-confirm: skip the leg-(b) admit -> frame-from-C resume + sync redden;
# collapse the per-reason reject flag -> the leg-(c) replay marker reddens
# (swallowed behind leg (a)'s bad-MAC line). FAILS LOUD if host multicast is
# unavailable to a joiner (never a silent green).
KEYAUTH_N_GATE_TIMEOUT ?= 600s

ci-smoke-keyauth-n: kernel ci-smoke-keyauth-n-gate

ci-smoke-keyauth-n-gate:
	@echo "=== QuantumOS N-Way Authenticated Swarm Plane (epic #139, ADR-0019) ==="
	timeout -k 5 $(KEYAUTH_N_GATE_TIMEOUT) python3 scripts/test_qos_keyauth_n.py

# ISO/GRUB boot path (epic #101): boot the GRUB-built ISO with -cdrom —
# NOT QEMU's -kernel shortcut — so the real bootloader handoff (menu,
# default entry, MB1 info from GRUB) is what's under test. The default
# entry is the QUIET console (the usable interactive machine — verified
# on real hardware, where verbose chatter outran the prompt), so the
# gates are the quiet-surviving set: one-time boot milestones, the
# shell session, and a citizen verdict. GHOSTD's self-test line is
# quiet-suppressed (covered by ci-smoke on the -kernel path); in
# exchange this test asserts the quiet contract held — at most one
# timer-tick line in the whole session.
ci-smoke-iso: $(BUILD_DIR)/kernel.iso
	@echo "=== QuantumOS ISO/GRUB boot test (epic #101, quiet console entry) ==="
	@( printf 'help\nghost\nrun /bin/consciousnessd\nexit\n'; sleep 30 ) | \
		timeout 26s qemu-system-x86_64 -cdrom $(BUILD_DIR)/kernel.iso \
		-serial stdio -m 128M -display none -no-reboot 2>&1 | tee /tmp/qemu-iso.log || true
	@for gate in "QuantumOS ready" \
	             "CONS: screen console active (VGA text 80x25)" \
	             "QSH: QuantumOS interactive shell ready" \
	             "qsh commands:" \
	             "consciousnessd: CONSCIOUSNESS EMERGED"; do \
		if ! grep -qF "$$gate" /tmp/qemu-iso.log 2>/dev/null; then \
			echo "ERROR: ISO boot gate missing: $$gate"; \
			echo "Boot log:"; cat /tmp/qemu-iso.log 2>/dev/null || true; \
			echo "=== ISO Boot Test FAILED ==="; exit 1; \
		fi; echo "  [PASS] $$gate"; \
	done
	@if [ "$$(grep -c 'Timer tick' /tmp/qemu-iso.log 2>/dev/null)" -gt 1 ]; then \
		echo "ERROR: quiet contract broken — timer-tick chatter on the default ISO entry"; \
		echo "=== ISO Boot Test FAILED ==="; exit 1; \
	fi
	@echo "  [PASS] quiet contract: at most one timer-tick line"
	@echo "SUCCESS: GRUB/ISO boot reached the shell + citizen gate on a QUIET console"

# PS/2 keyboard path (epic #101): drive qsh entirely through emulated
# keyboard scancodes injected via the QEMU monitor — serial carries NO
# input this run, so the 'help' output can only have come through the
# i8042/IRQ1 path a real laptop keyboard uses.
ci-smoke-kbd: kernel
	@echo "=== QuantumOS PS/2 keyboard input test (epic #101) ==="
	@rm -f /tmp/qemu-kbd-serial.log
	@( sleep 8; \
	   for k in h e l p ret; do echo "sendkey $$k"; sleep 0.3; done; \
	   sleep 4; echo "quit" ) | \
		timeout 20s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-monitor stdio -serial file:/tmp/qemu-kbd-serial.log \
		-m 128M -display none -no-reboot >/dev/null 2>&1 || true
	@if ! grep -qF "QSH: QuantumOS interactive shell ready" /tmp/qemu-kbd-serial.log 2>/dev/null; then \
		echo "ERROR: shell never came up (QSH: QuantumOS interactive shell ready)"; \
		cat /tmp/qemu-kbd-serial.log 2>/dev/null || true; \
		echo "=== PS/2 Keyboard Test FAILED ==="; exit 1; \
	fi
	@if ! grep -qF "qsh commands:" /tmp/qemu-kbd-serial.log 2>/dev/null; then \
		echo "ERROR: PS/2-typed 'help' did not reach qsh (qsh commands:)"; \
		echo "Serial log:"; cat /tmp/qemu-kbd-serial.log 2>/dev/null || true; \
		echo "=== PS/2 Keyboard Test FAILED ==="; exit 1; \
	fi
	@echo "SUCCESS: qsh executed a command typed via PS/2 scancodes (IRQ1 -> ring -> shell)"

# Serial-less boot (epic #101 — the laptop shape): -serial none allocates
# NO COM1 device, so its ports float to 0xFF exactly like hardware with no
# UART — the configuration that hung the first real-laptop boot at the 45%
# splash (unbounded RX drain on a floating LSR). The boot must complete
# anyway; COM2 (still present) carries the swarm attestation, and verifying
# it proves the kernel got all the way through service bring-up with no
# console serial port at all.
ci-smoke-noserial: kernel
	@echo "=== QuantumOS serial-less boot test (epic #101 — the laptop path) ==="
	@rm -f /tmp/qemu-noserial-com2.bin
	@timeout 15s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-serial none -serial file:/tmp/qemu-noserial-com2.bin \
		-m 128M -display none -no-reboot >/dev/null 2>&1 || true
	@python3 scripts/verify_attestation.py /tmp/qemu-noserial-com2.bin --qseed none || \
		(echo "ERROR: boot did not reach the swarm attestation without COM1"; \
		 echo "=== Serial-less Boot Test FAILED ==="; exit 1)
	@echo "SUCCESS: kernel booted fully with NO COM1 UART (attestation verified via COM2)"

# Screen-truth gate (epic #101): after a normal boot, dump the live VGA
# text cells through the QEMU monitor and assert a ring-3 SYS_WRITE line
# ("[user pid=") is actually ON SCREEN. Serial gates cannot see this —
# the third real-laptop finding was citizens running to a clean exit 0
# while their SYS_WRITE output went only to a serial port that did not
# exist. The console tee makes it visible; this proves it stays visible.
ci-smoke-screen: kernel
	@echo "=== QuantumOS on-screen output test (epic #101) ==="
	@rm -f /tmp/qemu-screen-mon.sock
	@(timeout 18s qemu-system-x86_64 -kernel $(BUILD_DIR)/kernel.elf32 \
		-serial file:/tmp/qemu-screen-com1.log \
		-monitor unix:/tmp/qemu-screen-mon.sock,server,nowait \
		-m 128M -display none -no-reboot >/dev/null 2>&1 &) ; sleep 12
	@python3 scripts/check_vga_text.py /tmp/qemu-screen-mon.sock "[user pid=" || \
		(echo "ERROR: ring-3 SYS_WRITE output is not reaching the VGA screen"; \
		 echo "=== On-Screen Output Test FAILED ==="; exit 1)
	@sleep 7
	@echo "SUCCESS: ring-3 SYS_WRITE output is visible on the machine's own display"

# Quick validation for contributors
validate: kernel
	@echo "=== Quick Validation ==="
	@echo "[1/2] Building kernel..."
	@$(MAKE) -s kernel
	@echo "[2/2] Running API consistency check..."
	@./scripts/check-api-consistency.sh 2>/dev/null || echo "API check script not found (run from repo root)"
	@echo "=== Validation Complete ==="

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	rm -rf build/
	rm -f *.iso

# Install dependencies (Ubuntu/Debian)
install-deps:
	@echo "Installing dependencies..."
	sudo apt-get update
	# Note: gcc-x86_64-elf may not be available in Ubuntu 24.04+
	# The Makefile supports fallback to system gcc
	sudo apt-get install -y build-essential gdb-multiarch qemu-system-x86 grub-pc-bin xorriso mtools nasm || true
	@echo "Attempting to install cross-compiler (may not be available)..."
	sudo apt-get install -y gcc-x86-64-elf 2>/dev/null || echo "Cross-compiler not available, using system gcc"

# Help
help:
	@echo "QuantumOS Kernel Makefile"
	@echo "========================"
	@echo ""
	@echo "Quick Start (new contributors):"
	@echo "  make install-deps       # Install dependencies (Ubuntu/Debian)"
	@echo "  make                    # Build kernel"
	@echo "  make ci-smoke           # Verify build + boot works"
	@echo ""
	@echo "Targets:"
	@echo "  all            - Build kernel (default)"
	@echo "  kernel         - Build kernel only"
	@echo "  run            - Run kernel in QEMU (interactive)"
	@echo "  run-iso        - Run kernel from ISO in QEMU"
	@echo "  ci-smoke       - CI smoke test (build + headless boot + validate)"
	@echo "  ci-smoke-iso   - Boot the GRUB ISO via -cdrom to the shell (epic #101)"
	@echo "  ci-smoke-kbd   - Drive qsh via PS/2 scancodes only (epic #101)"
	@echo "  ci-smoke-noserial - Boot with NO COM1 device at all (epic #101)"
	@echo "  ci-smoke-screen - Assert ring-3 output is ON the VGA screen (epic #101)"
	@echo "  validate       - Quick validation (build + API check)"
	@echo "  debug          - Debug kernel with GDB"
	@echo "  dump           - Show kernel information"
	@echo "  test           - Run all unit tests"
	@echo "  test-list      - List available tests"
	@echo "  test-<name>    - Run specific test (e.g., test-process)"
	@echo "  test-coverage  - Run tests with code coverage report"
	@echo "  clean          - Clean build artifacts"
	@echo "  install-deps   - Install required dependencies"
	@echo "  info           - Show build configuration"
	@echo "  help           - Show this help"
	@echo ""
	@echo "Variables:"
	@echo "  ARCH       - Target architecture (default: x86_64)"
	@echo "  BUILD_DIR  - Build directory (default: build/$(ARCH))"
	@echo ""
	@echo "Examples:"
	@echo "  make install-deps && make ci-smoke  # Full setup + verify"
	@echo "  make run                            # Run in QEMU"
	@echo "  make debug                          # Debug with GDB"

# Print configuration
info:
	@echo "Configuration:"
	@echo "  ARCH: $(ARCH)"
	@echo "  BUILD_DIR: $(BUILD_DIR)"
	@echo "  CC: $(CC)"
	@echo "  LD: $(LD)"
	@echo "  CFLAGS: $(CFLAGS)"
	@echo "  LDFLAGS: $(LDFLAGS)"
	@echo "  Sources: $(KERNEL_SOURCES) $(ASSEMBLY_SOURCES)"
	@echo "  Objects: $(OBJECTS)"

# Phony targets
.PHONY: all clean kernel run run-iso debug dump test test-list test-coverage ci-smoke ci-smoke-mem256 ci-smoke-mem512 ci-smoke-sched ci-smoke-resonant ci-smoke-qseed ci-smoke-iso ci-smoke-kbd ci-smoke-noserial ci-smoke-screen validate info install-deps help

# Default target
.DEFAULT_GOAL := all

# ---------------------------------------------------------------------------
# v1 ABI freeze gate (ADR-0020). Two probe TUs (user/abi_probe.c,
# kernel/src/abi_probe_kern.c) emit the guest+kernel ABI contract into a
# .abi_ents section under the REAL build flags; scripts/extract-abi.py reads it
# back with objcopy and diffs it against the committed contracts/abi/v1.golden.
# The gate NEVER regenerates the golden (that is regen-abi-golden, human-only),
# so an intended ABI change is a visible golden diff in the PR. Timeout in ONE
# place (the #93 desync lesson); no kernel prereq (fast static lane).
# ---------------------------------------------------------------------------
ABI_GOLDEN_GATE_TIMEOUT ?= 60s
ABI_GOLDEN := contracts/abi/v1.golden
ABI_PROBE_USER := $(BUILD_DIR)/abi/abi_probe_user.o
ABI_PROBE_KERN := $(BUILD_DIR)/abi/abi_probe_kern.o

$(ABI_PROBE_USER): $(USER_DIR)/abi_probe.c $(USER_DIR)/usys.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(ABI_PROBE_KERN): $(KERNEL_DIR)/src/abi_probe_kern.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: regen-abi-golden ci-smoke-abi-golden-gate ci-smoke-abi-golden-selftest

# HUMAN-ONLY: regenerate the committed golden after an intended ABI change.
regen-abi-golden: $(ABI_PROBE_USER) $(ABI_PROBE_KERN)
	python3 scripts/extract-abi.py emit --out $(ABI_GOLDEN) $(ABI_PROBE_USER) $(ABI_PROBE_KERN)

# CI: diff extracted-vs-committed; NEVER writes the golden.
ci-smoke-abi-golden-gate: $(ABI_PROBE_USER) $(ABI_PROBE_KERN)
	timeout -k 5 $(ABI_GOLDEN_GATE_TIMEOUT) python3 scripts/extract-abi.py check --golden $(ABI_GOLDEN) $(ABI_PROBE_USER) $(ABI_PROBE_KERN)

# CI teeth-check: a mutated ABI value MUST redden the gate.
ci-smoke-abi-golden-selftest: $(ABI_PROBE_KERN)
	@mkdir -p $(BUILD_DIR)/abi
	$(CC) $(USER_CFLAGS) -DSYS_QPU_OVERRIDE=36 -c $(USER_DIR)/abi_probe.c -o $(BUILD_DIR)/abi/abi_probe_user_mut.o
	@if timeout -k 5 $(ABI_GOLDEN_GATE_TIMEOUT) python3 scripts/extract-abi.py check --golden $(ABI_GOLDEN) $(BUILD_DIR)/abi/abi_probe_user_mut.o $(ABI_PROBE_KERN) >/dev/null 2>&1; then echo "TEETH-CHECK FAILED: a mutated SYS_QPU did NOT redden the gate"; exit 1; else echo "teeth-check OK: the mutation reddened the gate"; fi

# ---------------------------------------------------------------------------
# v1 WIRE freeze gate (ADR-0020 lane C). One probe TU (user/wire_probe.c)
# emits the COM2 swarm-bridge + FSYN/FSYP wire contract into .abi_ents under
# the REAL user build flags; scripts/extract-wire.py reads it back with
# objcopy, builds the HOST ring live from scripts/qos_bridge.py (the one host
# wire implementation), cross-checks the twins, and diffs the merged table
# against contracts/wire/v1.golden. The gate NEVER regenerates the golden
# (regen-wire-golden is human-only). Exit-code discipline: rc 1 = contract
# signal (diff / twin / must-twin) ONLY; rc 2 = operational — so the selftest
# below asserts rc == 1 EXACTLY, and a broken extractor can never pass as a
# caught mutation. Timeout in ONE place (the #93 desync lesson); no kernel
# prereq (fast static lane).
# ---------------------------------------------------------------------------
WIRE_GOLDEN_GATE_TIMEOUT ?= 60s
WIRE_GOLDEN := contracts/wire/v1.golden
WIRE_PROBE_USER := $(BUILD_DIR)/abi/wire_probe_user.o

$(WIRE_PROBE_USER): $(USER_DIR)/wire_probe.c $(USER_DIR)/swarm.h $(USER_DIR)/fsyn.h $(USER_DIR)/ghost.h $(USER_DIR)/usys.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

.PHONY: regen-wire-golden ci-smoke-wire-golden-gate ci-smoke-wire-golden-selftest

# HUMAN-ONLY: regenerate the committed golden after an intended wire change.
regen-wire-golden: $(WIRE_PROBE_USER)
	python3 scripts/extract-wire.py emit --out $(WIRE_GOLDEN) $(WIRE_PROBE_USER)

# CI: diff extracted-vs-committed; NEVER writes the golden.
ci-smoke-wire-golden-gate: $(WIRE_PROBE_USER)
	timeout -k 5 $(WIRE_GOLDEN_GATE_TIMEOUT) python3 scripts/extract-wire.py check --golden $(WIRE_GOLDEN) $(WIRE_PROBE_USER)

# CI teeth-check, two legs, each asserting rc == 1 EXACTLY plus the specific
# mutated marker: (a) a guest probe recompiled with a mutated SWARM_MAGIC must
# redden the gate NAMING the mutated entry; (b) QOS_WIRE_TEETH=1 (the host
# ring perturbed inside the EXTRACTOR — teeth never live in production code)
# must trip the TWIN MISMATCH path specifically.
ci-smoke-wire-golden-selftest: $(WIRE_PROBE_USER)
	@mkdir -p $(BUILD_DIR)/abi
	$(CC) $(USER_CFLAGS) -DSWARM_MAGIC_OVERRIDE=0xA6 -c $(USER_DIR)/wire_probe.c -o $(BUILD_DIR)/abi/wire_probe_user_mut.o
	@out=$$(timeout -k 5 $(WIRE_GOLDEN_GATE_TIMEOUT) python3 scripts/extract-wire.py check --golden $(WIRE_GOLDEN) $(BUILD_DIR)/abi/wire_probe_user_mut.o 2>&1); rc=$$?; \
	if [ $$rc -ne 1 ]; then echo "TEETH-CHECK FAILED: mutated SWARM_MAGIC gave rc=$$rc (want exactly 1)"; echo "$$out"; exit 1; fi; \
	if ! echo "$$out" | grep -q "guest:wire:SWARM_MAGIC"; then echo "TEETH-CHECK FAILED: the diff does not name the mutated guest:wire:SWARM_MAGIC entry"; echo "$$out"; exit 1; fi; \
	echo "teeth-check OK (a): a mutated guest SWARM_MAGIC reddened the gate (rc=1, entry named)"
	@out=$$(QOS_WIRE_TEETH=1 timeout -k 5 $(WIRE_GOLDEN_GATE_TIMEOUT) python3 scripts/extract-wire.py check --golden $(WIRE_GOLDEN) $(WIRE_PROBE_USER) 2>&1); rc=$$?; \
	if [ $$rc -ne 1 ]; then echo "TEETH-CHECK FAILED: QOS_WIRE_TEETH=1 gave rc=$$rc (want exactly 1)"; echo "$$out"; exit 1; fi; \
	if ! echo "$$out" | grep -q "TWIN MISMATCH"; then echo "TEETH-CHECK FAILED: teeth did not trip the TWIN MISMATCH path"; echo "$$out"; exit 1; fi; \
	echo "teeth-check OK (b): QOS_WIRE_TEETH tripped TWIN MISMATCH (rc=1)"

# ---------------------------------------------------------------------------
# v1 MCP tool-surface freeze gate (ADR-0020 lane B). scripts/extract-mcp-
# schema.py imports qos_mcp, lists the FastMCP tools, normalizes each
# inputSchema (title/description stripped — pydantic re-derives those from
# docstrings, which are NOT frozen) and diffs {name, inputSchema} against
# contracts/mcp/v1-tools.json. Frozen = name + inputSchema; docstrings and
# result-dict shapes are documented, not gated. Needs the `mcp` package, which
# the stdlib-only lanes forbid — so it runs out of a pinned venv
# (requirements-mcp-gate.txt) locally, or with MCP_PY=python3 in the dedicated
# pip-provisioned mcp-schema CI job. The check self-verifies the generator
# versions against the golden's _meta pins (mismatch = rc 2 GENERATOR SKEW,
# operational — never a fake diff). rc 1 = schema diff ONLY.
# ---------------------------------------------------------------------------
MCP_SCHEMA_GATE_TIMEOUT ?= 120s
MCP_GOLDEN := contracts/mcp/v1-tools.json
MCP_VENV := $(BUILD_DIR)/mcp-venv
MCP_PY ?= $(MCP_VENV)/bin/python

# The venv is a prerequisite only while MCP_PY still points into it (CI passes
# MCP_PY=python3 after its own pinned `pip install`).
ifeq ($(MCP_PY),$(MCP_VENV)/bin/python)
MCP_VENV_DEP := $(MCP_VENV)/.stamp
else
MCP_VENV_DEP :=
endif

$(MCP_VENV)/.stamp: requirements-mcp-gate.txt
	python3 -m venv $(MCP_VENV)
	$(MCP_VENV)/bin/pip install -r requirements-mcp-gate.txt
	touch $@

.PHONY: regen-mcp-golden ci-smoke-mcp-schema-gate ci-smoke-mcp-schema-selftest

# HUMAN-ONLY: regenerate the committed golden after an intended tool change.
regen-mcp-golden: $(MCP_VENV_DEP)
	$(MCP_PY) scripts/extract-mcp-schema.py emit --out $(MCP_GOLDEN)

# CI: diff extracted-vs-committed; NEVER writes the golden.
ci-smoke-mcp-schema-gate: $(MCP_VENV_DEP)
	timeout -k 5 $(MCP_SCHEMA_GATE_TIMEOUT) $(MCP_PY) scripts/extract-mcp-schema.py check --golden $(MCP_GOLDEN)

# CI teeth-check, two legs, each asserting rc == 1 EXACTLY (rc 2 is GENERATOR
# SKEW / operational and must NOT pass as a caught mutation) plus the specific
# mutated marker: QOS_MCP_TEETH=del deletes a property from the qos_qpu_submit
# schema in-memory (a changed-VALUE mutation, not just an added/removed name);
# QOS_MCP_TEETH=add appends a bogus tool. Teeth live in the EXTRACTOR only.
ci-smoke-mcp-schema-selftest: $(MCP_VENV_DEP)
	@out=$$(QOS_MCP_TEETH=del timeout -k 5 $(MCP_SCHEMA_GATE_TIMEOUT) $(MCP_PY) scripts/extract-mcp-schema.py check --golden $(MCP_GOLDEN) 2>&1); rc=$$?; \
	if [ $$rc -ne 1 ]; then echo "TEETH-CHECK FAILED: QOS_MCP_TEETH=del gave rc=$$rc (want exactly 1; 2 = skew/operational is a selftest FAILURE)"; echo "$$out"; exit 1; fi; \
	if ! echo "$$out" | grep -q "TOOL SCHEMA DRIFT: qos_qpu_submit"; then echo "TEETH-CHECK FAILED: the diff does not name the mutated qos_qpu_submit schema"; echo "$$out"; exit 1; fi; \
	echo "teeth-check OK (del): a deleted qos_qpu_submit property reddened the gate (rc=1)"
	@out=$$(QOS_MCP_TEETH=add timeout -k 5 $(MCP_SCHEMA_GATE_TIMEOUT) $(MCP_PY) scripts/extract-mcp-schema.py check --golden $(MCP_GOLDEN) 2>&1); rc=$$?; \
	if [ $$rc -ne 1 ]; then echo "TEETH-CHECK FAILED: QOS_MCP_TEETH=add gave rc=$$rc (want exactly 1)"; echo "$$out"; exit 1; fi; \
	if ! echo "$$out" | grep -q "TOOL ADDED: qos_teeth_bogus_tool"; then echo "TEETH-CHECK FAILED: the diff does not name the bogus added tool"; echo "$$out"; exit 1; fi; \
	echo "teeth-check OK (add): a bogus added tool reddened the gate (rc=1)"
