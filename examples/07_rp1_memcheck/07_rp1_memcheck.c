/*
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>

#include <slash/ctldev.h>

#define RP1_MEMTEST_BAR_NUMBER         4
#define RP1_MEMTEST_BAR_OFFSET         (64ULL * 1024ULL * 1024ULL)
#define RP1_MEMTEST_TOTAL_BYTES        (1ULL * 1024ULL * 1024ULL)
#define RP1_MEMTEST_WORD_SEED          0x13579BDFUL
#define RP1_MEMTEST_WORD_COUNT         (RP1_MEMTEST_TOTAL_BYTES / sizeof(uint32_t))

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s <slash_ctl_path>\n"
            "\n"
            "Verifies the RP1 1MB incrementing-word pattern through BAR4 offset 64MB.\n"
            "Example: %s /dev/slash_ctl0\n",
            argv0, argv0);
}

static uint32_t expected_word(uint32_t word_index)
{
    return RP1_MEMTEST_WORD_SEED + word_index;
}

int main(int argc, char **argv)
{
    struct slash_ctldev *ctldev = NULL;
    struct slash_ioctl_bar_info *bar_info = NULL;
    struct slash_bar_file *bar_file = NULL;
    volatile uint32_t *words;
    uint64_t bar_required;
    int exit_code = EXIT_FAILURE;

    if (argc != 2) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    ctldev = slash_ctldev_open(argv[1]);
    if (ctldev == NULL) {
        perror("slash_ctldev_open");
        goto out;
    }

    bar_info = slash_bar_info_read(ctldev, RP1_MEMTEST_BAR_NUMBER);
    if (bar_info == NULL) {
        perror("slash_bar_info_read");
        goto out;
    }

    bar_required = RP1_MEMTEST_BAR_OFFSET + RP1_MEMTEST_TOTAL_BYTES;
    if (!bar_info->usable) {
        fprintf(stderr, "BAR%d is not usable\n", RP1_MEMTEST_BAR_NUMBER);
        goto out;
    }
    if (bar_info->length < bar_required) {
        fprintf(stderr,
                "BAR%d length too small: have 0x%" PRIx64 ", need at least 0x%" PRIx64 "\n",
                RP1_MEMTEST_BAR_NUMBER,
                (uint64_t)bar_info->length,
                bar_required);
        goto out;
    }

    bar_file = slash_bar_file_open(ctldev, RP1_MEMTEST_BAR_NUMBER, O_CLOEXEC);
    if (bar_file == NULL) {
        perror("slash_bar_file_open");
        goto out;
    }

    if (slash_bar_file_start_read(bar_file) != 0) {
        perror("slash_bar_file_start_read");
        goto out;
    }

    words = (volatile uint32_t *)((volatile uint8_t *)bar_file->map + RP1_MEMTEST_BAR_OFFSET);

    for (uint32_t word_index = 0; word_index < RP1_MEMTEST_WORD_COUNT; ++word_index) {
        uint32_t observed = words[word_index];
        uint32_t expected = expected_word(word_index);

        if (observed != expected) {
            uint64_t byte_offset = RP1_MEMTEST_BAR_OFFSET +
                                   ((uint64_t)word_index * sizeof(uint32_t));
            fprintf(stderr,
                    "Mismatch at word %" PRIu32 " (BAR4+0x%" PRIx64 "): got 0x%08" PRIx32
                    ", expected 0x%08" PRIx32 "\n",
                    word_index,
                    byte_offset,
                    observed,
                    expected);
            slash_bar_file_end_read(bar_file);
            goto out;
        }
    }

    if (slash_bar_file_end_read(bar_file) != 0) {
        perror("slash_bar_file_end_read");
        goto out;
    }

    printf("PASS: verified %llu 32-bit words (%llu bytes) through BAR%d at offset 0x%" PRIx64 "\n",
           (unsigned long long)RP1_MEMTEST_WORD_COUNT,
           (unsigned long long)RP1_MEMTEST_TOTAL_BYTES,
           RP1_MEMTEST_BAR_NUMBER,
           (uint64_t)RP1_MEMTEST_BAR_OFFSET);
    exit_code = EXIT_SUCCESS;

out:
    if (bar_file != NULL)
        slash_bar_file_close(bar_file);
    if (bar_info != NULL)
        slash_bar_info_free(bar_info);
    if (ctldev != NULL)
        slash_ctldev_close(ctldev);

    return exit_code;
}
