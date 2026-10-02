//
// nvs — one CRC-checked record in the last flash sector. See nvs.h.
//
#include "nvs.h"
#include "board_config.h"

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#include <string.h>

// The last sector of the flash the board is built for. PICO_FLASH_SIZE_BYTES
// comes from the board header — 4 MB for PICO_BOARD=pico2 — and the image
// never grows anywhere near it.
//
// This build is for a Supermini, not a Pico 2, and its flash chip is whatever
// the vendor fitted. A part that is LARGER still has this address. A part that
// is SMALLER does not: the write lands wherever the chip wraps that address
// to, or nowhere. So the record is verified the honest way — write it, reset,
// and see whether the boot banner says "restored from flash". If it does not,
// set NVS_FLASH_SIZE_BYTES in board_config.h to what `picotool info -a`
// reports and rebuild.
#ifdef NVS_FLASH_SIZE_BYTES
#define NVS_OFFSET      (NVS_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#else
#define NVS_OFFSET      (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#endif
#define NVS_MAGIC       0x3153564Eu     // "NVS1"

typedef struct {
    uint32_t magic;
    uint32_t len;
    uint32_t crc;
    uint32_t pad;
} hdr_t;

#define NVS_MAX_LEN     (FLASH_SECTOR_SIZE - sizeof(hdr_t))

// Flash is memory-mapped through XIP, so reading is a pointer.
static const uint8_t *nvs_base(void) { return (const uint8_t *)(XIP_BASE + NVS_OFFSET); }

// CRC-32 (IEEE), bit by bit — a few hundred bytes at most, once per boot.
static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

bool nvs_read(void *buf, size_t len)
{
    const hdr_t *h = (const hdr_t *)nvs_base();
    if (h->magic != NVS_MAGIC || h->len != len || len > NVS_MAX_LEN)
        return false;
    const uint8_t *data = nvs_base() + sizeof(hdr_t);
    if (crc32(data, len) != h->crc)
        return false;
    memcpy(buf, data, len);
    return true;
}

// Erase, then program whole pages. Interrupts off for the duration: the flash
// cannot serve instruction fetches while it is being erased, so nothing else —
// an ISR included — may run until it is back. On this single-core build that
// is all the locking there is to do.
static void write_sector(const uint8_t *pages, size_t n_pages)
{
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(NVS_OFFSET, FLASH_SECTOR_SIZE);
    if (n_pages)
        flash_range_program(NVS_OFFSET, pages, n_pages * FLASH_PAGE_SIZE);
    restore_interrupts(ints);
}

bool nvs_write(const void *buf, size_t len)
{
    if (len > NVS_MAX_LEN)
        return false;

    // Assembled in RAM, padded to a page: flash_range_program() writes whole
    // 256-byte pages and the source must not itself be in flash.
    static uint8_t image[FLASH_SECTOR_SIZE];
    size_t total = sizeof(hdr_t) + len;
    size_t pages = (total + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;

    memset(image, 0xFF, pages * FLASH_PAGE_SIZE);
    hdr_t *h = (hdr_t *)image;
    h->magic = NVS_MAGIC;
    h->len   = (uint32_t)len;
    h->crc   = crc32((const uint8_t *)buf, len);
    h->pad   = 0xFFFFFFFFu;
    memcpy(image + sizeof(hdr_t), buf, len);

    write_sector(image, pages);
    return true;
}

void nvs_erase(void)
{
    write_sector(NULL, 0);
}
