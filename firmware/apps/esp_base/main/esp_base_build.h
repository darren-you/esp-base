// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Stored immediately after esp_app_desc_t in the first image segment.
 * This is package identity, not the bootloader/eFuse secure_version.
 * No pointers, target-dependent integers or implicit padding on the wire. */
typedef struct {
    uint32_t magic;
    uint32_t size;
    uint64_t build_number;
    uint32_t schema_version;
    uint32_t local_validation;
    char version[32];
    char source_commit_sha[41];
    char input_sha256[65];
    char run_id[128];
    char target[16];
    uint8_t reserved[14];
} esp_base_build_t;

_Static_assert(sizeof(esp_base_build_t) == 320, "Build descriptor size changed");
_Static_assert(offsetof(esp_base_build_t, build_number) == 8, "Build number offset changed");
_Static_assert(offsetof(esp_base_build_t, version) == 24, "Build version offset changed");
_Static_assert(offsetof(esp_base_build_t, run_id) == 162, "Build run offset changed");

extern const esp_base_build_t esp_base_build;
