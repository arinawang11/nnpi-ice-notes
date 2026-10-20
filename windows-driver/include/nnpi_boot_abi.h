/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_BOOT_ABI_H
#define NNPI_BOOT_ABI_H
#include "nnpi_info_abi.h"
#define NNPI_BOOT_ABI_VERSION 1U
#include "nnpi_image_identity.h"
#define NNPI_BOOT_IMAGE_BYTES NNPI_APPROVED_IMAGE_BYTES
#define NNPI_BOOT_CHUNK_MAX 1048576U
#define NNPI_BOOT_SHA256_HEX NNPI_APPROVED_IMAGE_SHA256
#define NNPI_BOOT_BEGIN_IOCTL  0x8000e034U /* function 0x80d */
#define NNPI_BOOT_CHUNK_IOCTL  0x8000e038U
#define NNPI_BOOT_SEAL_IOCTL   0x8000e03cU
#define NNPI_BOOT_SUBMIT_IOCTL 0x8000e040U
#define NNPI_BOOT_QUERY_IOCTL  0x80006044U
/* All requests: exact 64-byte header. CHUNK appends chunk_bytes bytes; size
 * remains 64. BEGIN: session_id=0, offset=0, chunk_bytes=0, image_bytes fixed,
 * current query G/E. QUERY: all fields after size zero. Other commands echo
 * the kernel-issued session/G/E; image_bytes fixed. SEAL/SUBMIT offset=0.
 * No retries after lost replies. Query is CPU-only, including outside D0. */
struct nnpi_boot_request {
    uint32_t version, size;
    uint64_t session_id, generation, epoch, image_bytes, offset;
    uint32_t chunk_bytes, reserved[3];
};
#define NNPI_BOOT_LOAD_EMPTY 0U
#define NNPI_BOOT_LOAD_LOADING 1U
#define NNPI_BOOT_LOAD_SEALED 2U
#define NNPI_BOOT_LOAD_SUBMITTED 3U
#define NNPI_BOOT_LOAD_FAILED 4U
#define NNPI_BOOT_FAIL_NONE 0U
#define NNPI_BOOT_FAIL_IDENTITY 1U
#define NNPI_BOOT_FAIL_IMAGE 2U
#define NNPI_BOOT_FAIL_HASH 3U
#define NNPI_BOOT_FAIL_CAPSULE 4U
#define NNPI_BOOT_FAIL_BUDGET 5U
#define NNPI_BOOT_FAIL_BASELINE 6U
#define NNPI_BOOT_FAIL_MMIO 7U
#define NNPI_BOOT_FAIL_TIMEOUT 8U
#define NNPI_BOOT_FAIL_PROTOCOL 9U
#define NNPI_BOOT_FAIL_CLOSING 10U
#define NNPI_BOOT_FAIL_RESET 11U
/* phase uses NNPI_BS_* from nnpi_boot_session.h. DriverReady never releases
 * DMA. failed_stage identifies the first failure; reset_status is separate.
 * reserved must be zero. No addresses/pointers or register selectors. */
struct nnpi_boot_response {
    uint32_t version, size;
    uint64_t session_id, generation, epoch, image_bytes, loaded_bytes;
    uint32_t load_state, phase, failed_stage, ntstatus;
    uint32_t budget_consumed, hostready_writes, sysinfo_halves, image_halves;
    uint32_t host_before, card_before, command_before, response_before;
    uint32_t card_last, observations, payload_valid, boot_started;
    uint32_t driver_ready, published_dma, stopped_dma, closing;
    uint32_t reset_used, reset_returned, reset_status, worker_active;
    uint32_t sha256_verified, capsule_rejected, reserved[2];
    uint8_t sha256[32];
};
typedef char nnpi_boot_req_size[(sizeof(struct nnpi_boot_request)==64)?1:-1];
typedef char nnpi_boot_resp_size[(sizeof(struct nnpi_boot_response)==192)?1:-1];
#endif
