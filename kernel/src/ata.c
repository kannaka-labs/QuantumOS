/**
 * QuantumOS ATA PIO Block Driver implementation (epic #71 phase 1).
 *
 * Polled LBA28 on the primary master. No interrupts (nIEN set), no DMA,
 * no slave, no ATAPI — the narrowest driver that makes persistence real.
 * Every wait is bounded; every data phase re-checks the status bits the
 * spec says can appear (ERR, DF) so a sick controller degrades to an
 * error return instead of a wedged kernel.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/ata.h>
#include <kernel/boot.h>

/* Register offsets from ATA_IO_BASE */
#define ATA_REG_DATA 0
#define ATA_REG_ERROR 1
#define ATA_REG_FEATURES 1
#define ATA_REG_SECCOUNT 2
#define ATA_REG_LBA_LO 3
#define ATA_REG_LBA_MID 4
#define ATA_REG_LBA_HI 5
#define ATA_REG_DRIVE 6
#define ATA_REG_STATUS 7
#define ATA_REG_COMMAND 7

/* Control block (alternate status / device control) */
#define ATA_CTRL_BASE 0x3F6
#define ATA_CTRL_NIEN 0x02 /* disable drive interrupts — we poll */

/* Status bits */
#define ATA_ST_ERR 0x01
#define ATA_ST_DRQ 0x08
#define ATA_ST_DF 0x20
#define ATA_ST_RDY 0x40
#define ATA_ST_BSY 0x80

/* Commands */
#define ATA_CMD_READ_SECTORS 0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_CACHE_FLUSH 0xE7
#define ATA_CMD_IDENTIFY 0xEC

/* Bounded polling budget. QEMU's IDE model completes PIO transfers
 * within microseconds; a real drive within milliseconds. This bound is
 * generous for both and small enough that a dead controller costs a
 * blink, not a hang. */
#define ATA_SPIN_MAX 1000000u

static int disk_present;
static uint32_t disk_sectors;

static inline void io_outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t io_inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t io_inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ~400ns settle after a drive-select: four reads of the alternate
 * status register, per spec. */
static void ata_io_delay(void) {
    for (int i = 0; i < 4; i++) {
        (void)io_inb(ATA_CTRL_BASE);
    }
}

/* Wait for BSY to clear. Returns the final status byte, or -1 on
 * timeout. */
static int wait_not_busy(void) {
    for (uint32_t i = 0; i < ATA_SPIN_MAX; i++) {
        uint8_t st = io_inb(ATA_IO_BASE + ATA_REG_STATUS);
        if (!(st & ATA_ST_BSY)) {
            return st;
        }
    }
    return -1;
}

/* Wait for a data phase: BSY clear and DRQ set, without ERR/DF.
 * Returns 0 ready, -1 error/timeout. */
static int wait_drq(void) {
    for (uint32_t i = 0; i < ATA_SPIN_MAX; i++) {
        uint8_t st = io_inb(ATA_IO_BASE + ATA_REG_STATUS);
        if (st & ATA_ST_BSY) {
            continue;
        }
        if (st & (ATA_ST_ERR | ATA_ST_DF)) {
            return -1;
        }
        if (st & ATA_ST_DRQ) {
            return 0;
        }
    }
    return -1;
}

/* Select the master drive with the top LBA28 nibble and settle. */
static void select_drive(uint32_t lba) {
    io_outb(ATA_IO_BASE + ATA_REG_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_io_delay();
}

/* Program the LBA28 registers for a one-sector transfer. */
static void setup_lba(uint32_t lba) {
    io_outb(ATA_IO_BASE + ATA_REG_FEATURES, 0);
    io_outb(ATA_IO_BASE + ATA_REG_SECCOUNT, 1);
    io_outb(ATA_IO_BASE + ATA_REG_LBA_LO, (uint8_t)lba);
    io_outb(ATA_IO_BASE + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
    io_outb(ATA_IO_BASE + ATA_REG_LBA_HI, (uint8_t)(lba >> 16));
}

/* One-sector read at `lba` into buf[512]. */
static int read_sector(uint32_t lba, uint8_t *buf) {
    select_drive(lba);
    setup_lba(lba);
    io_outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);

    if (wait_drq() != 0) {
        return -1;
    }
    for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++) {
        uint16_t w = io_inw(ATA_IO_BASE + ATA_REG_DATA);
        buf[i * 2] = (uint8_t)w;
        buf[i * 2 + 1] = (uint8_t)(w >> 8);
    }
    return 0;
}

/* One-sector write of buf[512] at `lba`. Does NOT flush — the caller
 * (ata_write) issues one CACHE FLUSH after the whole run, because a
 * flush command issued while the drive is still BSY from the write is
 * ignored (and, on real hardware, illegal). */
static int write_sector(uint32_t lba, const uint8_t *buf) {
    select_drive(lba);
    setup_lba(lba);
    io_outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);

    if (wait_drq() != 0) {
        return -1;
    }
    for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++) {
        uint16_t w = (uint16_t)buf[i * 2] | ((uint16_t)buf[i * 2 + 1] << 8);
        io_outw(ATA_IO_BASE + ATA_REG_DATA, w);
    }

    /* Complete the WRITE: wait for BSY to clear and confirm no error
     * BEFORE any further command. This is the write-completion handshake
     * the flush was wrongly racing. */
    int st = wait_not_busy();
    if (st < 0 || (st & (ATA_ST_ERR | ATA_ST_DF))) {
        return -1;
    }
    return 0;
}

/* Flush the write cache once, after a run of writes has fully drained.
 * Its own generous budget: a real drive's flush latency depends on host
 * storage, unlike the microsecond command/DRQ handshakes. */
#define ATA_FLUSH_SPIN_MAX 10000000u
static int cache_flush(void) {
    io_outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    for (uint32_t i = 0; i < ATA_FLUSH_SPIN_MAX; i++) {
        uint8_t st = io_inb(ATA_IO_BASE + ATA_REG_STATUS);
        if (!(st & ATA_ST_BSY)) {
            return (st & (ATA_ST_ERR | ATA_ST_DF)) ? -1 : 0;
        }
    }
    return -1;
}

/* Any timeout/protocol error latches the channel dead: leaving it mid-
 * protocol (e.g. DRQ still asserted with an unread sector) would let a
 * later command drain stale data and return the WRONG sector as success.
 * Fail-fast is safer than wrong data for a persistence store. */
static void ata_fault(const char *what) {
    disk_present = 0;
    boot_log("ATA: fault — persistence disabled:");
    boot_log(what);
}

int ata_read(uint32_t lba, void *buf, uint32_t count) {
    if (!disk_present || !buf || count == 0 || lba + count < lba || lba + count > disk_sectors) {
        return -1;
    }
    uint8_t *p = (uint8_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        if (read_sector(lba + s, p + (uint64_t)s * ATA_SECTOR_SIZE) != 0) {
            ata_fault("read");
            return -1;
        }
    }
    return 0;
}

int ata_write(uint32_t lba, const void *buf, uint32_t count) {
    if (!disk_present || !buf || count == 0 || lba + count < lba || lba + count > disk_sectors) {
        return -1;
    }
    const uint8_t *p = (const uint8_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        if (write_sector(lba + s, p + (uint64_t)s * ATA_SECTOR_SIZE) != 0) {
            ata_fault("write");
            return -1;
        }
    }
    /* One flush for the whole run, after every sector has drained (not
     * per sector — that would be one host fsync per sector). */
    if (cache_flush() != 0) {
        ata_fault("flush");
        return -1;
    }
    return 0;
}

int ata_present(void) {
    return disk_present;
}

uint32_t ata_sector_count(void) {
    return disk_sectors;
}

int ata_init(void) {
    disk_present = 0;
    disk_sectors = 0;

    /* Polled operation: mask drive interrupts at the control block. */
    io_outb(ATA_CTRL_BASE, ATA_CTRL_NIEN);

    /* Select the master and issue IDENTIFY with the address registers
     * zeroed (spec requirement for the probe). */
    io_outb(ATA_IO_BASE + ATA_REG_DRIVE, 0xA0);
    ata_io_delay();
    io_outb(ATA_IO_BASE + ATA_REG_SECCOUNT, 0);
    io_outb(ATA_IO_BASE + ATA_REG_LBA_LO, 0);
    io_outb(ATA_IO_BASE + ATA_REG_LBA_MID, 0);
    io_outb(ATA_IO_BASE + ATA_REG_LBA_HI, 0);
    io_outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    /* Status 0 = nothing on the bus. */
    if (io_inb(ATA_IO_BASE + ATA_REG_STATUS) == 0) {
        boot_log("ATA: no disk — persistence disabled");
        return 0;
    }

    if (wait_not_busy() < 0) {
        boot_log("ATA: controller timeout — persistence disabled");
        return 0;
    }

    /* Non-ATA devices (ATAPI etc.) advertise themselves in the LBA
     * signature bytes after IDENTIFY — treat them as absent. */
    if (io_inb(ATA_IO_BASE + ATA_REG_LBA_MID) != 0 || io_inb(ATA_IO_BASE + ATA_REG_LBA_HI) != 0) {
        boot_log("ATA: non-ATA device — persistence disabled");
        return 0;
    }

    if (wait_drq() != 0) {
        boot_log("ATA: IDENTIFY failed — persistence disabled");
        return 0;
    }

    /* Drain the 256-word IDENTIFY block; words 60-61 hold the LBA28
     * addressable sector count. */
    uint16_t id[256];
    for (int i = 0; i < 256; i++) {
        id[i] = io_inw(ATA_IO_BASE + ATA_REG_DATA);
    }
    disk_sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    if (disk_sectors == 0) {
        boot_log("ATA: disk reports 0 LBA28 sectors — persistence disabled");
        return 0;
    }

    disk_present = 1;
    boot_log("ATA: disk present (LBA28), sectors:");
    early_console_write_hex(disk_sectors);
    /* The sector RW self-test runs from blkdev_init() once this binds. */
    return 1;
}

/* ---- blkdev binding ---- */
const blkdev_ops_t ata_blkdev = {
    .name = "ata",
    .tag = "ATA",
    .init = ata_init,
    .present = ata_present,
    .sector_count = ata_sector_count,
    .read = ata_read,
    .write = ata_write,
};
