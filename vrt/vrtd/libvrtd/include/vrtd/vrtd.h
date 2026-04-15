/**
 * The MIT License (MIT)
 * Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * @file vrtd.h
 * @brief C client API for the V80 Runtime Daemon (vrtd).
 *
 * This library (libvrtd) provides a client interface to the VRT daemon (vrtd),
 * which multiplexes access to SLASH-managed FPGA devices
 * with permission control and multi‑tenancy.
 *
 * Stack overview:
 *   slash (kernel module) <- libslash <- vrtd <- libvrtd <- libvrtdpp <- libvrt
 *
 * Most functions return a #vrtd_ret code. On success, functions return
 * #VRTD_RET_OK and populate their output parameters.
 */

#ifndef LIBVRTD_VRTD_H
#define LIBVRTD_VRTD_H

#include <slash/ctldev.h>
#include <vrtd/wire.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @def VRTD_STANDARD_PATH
 * @brief Default UNIX domain socket path for the vrtd daemon.
 */
#define VRTD_STANDARD_PATH "/run/vrtd.sock"

struct vrtd_buffer;


/**
 * @brief Connect to the vrtd UNIX domain socket.
 *
 * Creates a SOCK_SEQPACKET connection to the vrtd daemon at @p path.
 *
 * @param path Absolute path to the vrtd socket (e.g. ::VRTD_STANDARD_PATH).
 *             Must not be NULL.
 * @return On success, a non‑negative file descriptor to the socket. The caller
 *         owns this descriptor and must close it with @c close().
 * @return On failure, returns -1 and sets @c errno.
 */
int vrtd_connect(const char *path);


/**
 * @brief Send a raw vrtd protocol request and receive the response.
 *
 * This is a low‑level escape hatch for issuing arbitrary protocol opcodes.
 * Most users should prefer higher‑level helpers (e.g., vrtd_get_* functions).
 *
 * @param fd            Connected vrtd socket file descriptor.
 * @param opcode        Protocol opcode to send (see @ref vrtd_opcode in wire.h).
 * @param body          Pointer to request body buffer (may be NULL if @p body_size == 0).
 * @param body_size     Size of request body in bytes.
 * @param resp_buf      Buffer to receive the response body (may be NULL if no body expected).
 * @param resp_bufsz    Size of @p resp_buf in bytes.
 * @param resp_fd       Optional; if non‑NULL and the response carries a file
 *                      descriptor (e.g., GET_BAR_FD), the received FD will be
 *                      stored here. Otherwise ignored.
 * @param req_fd        Optional; if non‑NULL and @p *req_fd >= 0, the FD will
 *                      be sent to the daemon via SCM_RIGHTS.
 *
 * @return #VRTD_RET_OK on success; otherwise a #vrtd_ret error code.
 *
 * @warning The request size must not exceed the protocol limit
 *          (e.g., @c VRTD_MSG_MAX_SIZE - sizeof(struct vrtd_req_header)).
 * @note    On success, @p resp_buf contains exactly the response body bytes.
 * @note    @p resp_fd and @p req_fd are optional.
 */
enum vrtd_ret vrtd_raw_request(
    int fd,
    uint16_t opcode,
    const void *body, uint16_t body_size,
    void *resp_buf, size_t resp_bufsz,
    int *resp_fd,
    const int *req_fd
);


/**
 * @brief Query the number of available devices.
 *
 * @param fd                 Connected vrtd socket file descriptor.
 * @param num_devices_out    Output pointer to receive the device count.
 *
 * @return #VRTD_RET_OK on success; otherwise a #vrtd_ret error code.
 * @pre @p num_devices_out must not be NULL.
 */
enum vrtd_ret vrtd_get_num_devices(
    int fd,
    uint32_t *num_devices_out
);

/**
 * @brief Get information about a device (name + PCI info).
 *
 * @param fd          Connected vrtd socket file descriptor.
 * @param dev         Device index (0‑based).
 * @param info_out    Output device info (name + PCI metadata).
 *
 * @return #VRTD_RET_OK on success; otherwise a #vrtd_ret error code.
 * @pre @p info_out must not be NULL.
 */
enum vrtd_ret vrtd_get_device_info(
    int fd,
    uint32_t dev,
    struct vrtd_device_info *info_out
);

/**
 * @brief Look up a device index by PCI BDF.
 *
 * @param fd        Connected vrtd socket file descriptor.
 * @param bdf       PCI BDF string (e.g., "0000:65:00.0").
 * @param dev_out   Output device index.
 *
 * @return #VRTD_RET_OK on success; otherwise a #vrtd_ret error code.
 * @pre @p bdf and @p dev_out must not be NULL.
 */
enum vrtd_ret vrtd_get_device_by_bdf(
    int fd,
    const char *bdf,
    uint32_t *dev_out
);

/**
 * @brief Retrieve information about a device BAR (Base Address Register).
 *
 * Complementary to vrtd_get_bar_fd(); this returns metadata only.
 *
 * @param fd             Connected vrtd socket file descriptor.
 * @param dev            Device index (0‑based).
 * @param bar            BAR index.
 * @param bar_info_out   Output pointer for BAR info (layout, permissions, etc.).
 *
 * @return #VRTD_RET_OK on success; otherwise a #vrtd_ret error code.
 * @pre @p bar_info_out must not be NULL.
 */
enum vrtd_ret vrtd_get_bar_info(
    int fd,
    uint32_t dev,
    uint8_t bar,
    struct slash_ioctl_bar_info *bar_info_out
);

/**
 * @brief Obtain a file descriptor for a device BAR, suitable for @c mmap().
 *
 * Complementary to vrtd_get_bar_info(); this returns a handle to the BAR memory.
 *
 * @param fd       Connected vrtd socket file descriptor.
 * @param dev      Device index (0‑based).
 * @param bar      BAR index.
 * @param fd_out   Output pointer to receive the BAR file descriptor.
 * @param len_out  Output pointer to receive the BAR length in bytes.
 *
 * @return #VRTD_RET_OK on success; otherwise a #vrtd_ret error code.
 * @pre @p fd_out and @p len_out must not be NULL.
 * @note The caller owns the returned FD and should close it when no longer needed
 *       (or use vrtd_open_bar_file()/vrtd_close_bar_file()).
 */
enum vrtd_ret vrtd_get_bar_fd(
    int fd,
    uint32_t dev,
    uint8_t bar,
    int *fd_out,
    uint64_t *len_out
);

/**
 * @brief Open a BAR and map it into the process address space.
 *
 * Convenience helper that requests a BAR FD and performs @c mmap() into
 * @p bar_file_out->map with length @p bar_file_out->len.
 *
 * @param fd             Connected vrtd socket file descriptor.
 * @param dev            Device index (0‑based).
 * @param bar            BAR index.
 * @param bar_file_out   Output structure receiving the BAR FD, length and mapping.
 *
 * @return #VRTD_RET_OK on success; otherwise a #vrtd_ret error code.
 * @pre @p bar_file_out must not be NULL.
 * @post On success, @p bar_file_out->fd is valid and @p bar_file_out->map is
 *       a writable shared mapping of size @p bar_file_out->len.
 * @warning The caller must later call vrtd_close_bar_file() to unmap and close.
 */
enum vrtd_ret vrtd_open_bar_file(
    int fd,
    uint32_t dev,
    uint8_t bar,
    struct slash_bar_file *bar_file_out
);

/**
 * @brief Unmap and close resources acquired by vrtd_open_bar_file().
 *
 * Safe to call with NULL and safe to call multiple times; on first successful
 * call it unmaps, closes the FD, and clears @p bar_file_out->map.
 *
 * @param bar_file_out  Pointer previously filled by vrtd_open_bar_file().
 */
void vrtd_close_bar_file(
    struct slash_bar_file *bar_file_out
);

/**
 * @brief Query QDMA capabilities for a device.
 *
 * Thin wrapper around the vrtd QDMA GET_INFO opcode. On success,
 * fills @p info_out with the kernel's view of the QDMA device.
 *
 * @param fd        Connected vrtd socket file descriptor.
 * @param dev       Device index (0‑based).
 * @param info_out  Output pointer for QDMA capability information.
 *
 * @return #VRTD_RET_OK on success; otherwise a #vrtd_ret error code.
 * @pre @p info_out must not be NULL.
 */
enum vrtd_ret vrtd_qdma_get_info(
    int fd,
    uint32_t dev,
    struct slash_qdma_info *info_out
);

/**
 * @brief Create a QDMA qpair on a device.
 *
 * On success, @p qpair_inout is updated with the kernel‑assigned qid.
 *
 * @param fd           Connected vrtd socket file descriptor.
 * @param dev          Device index (0‑based).
 * @param qpair_inout  In/out QDMA qpair parameters (see slash_qdma_qpair_add).
 *
 * @return #VRTD_RET_OK on success; otherwise a #vrtd_ret error code.
 * @pre @p qpair_inout must not be NULL.
 */
enum vrtd_ret vrtd_qdma_qpair_add(
    int fd,
    uint32_t dev,
    struct slash_qdma_qpair_add *qpair_inout
);

/**
 * @brief Start, stop, or delete a QDMA qpair.
 *
 * Convenience wrappers around the QDMA qpair OP opcode.
 */
enum vrtd_ret vrtd_qdma_qpair_start(
    int fd,
    uint32_t dev,
    uint32_t qid
);

enum vrtd_ret vrtd_qdma_qpair_stop(
    int fd,
    uint32_t dev,
    uint32_t qid
);

enum vrtd_ret vrtd_qdma_qpair_del(
    int fd,
    uint32_t dev,
    uint32_t qid
);

/**
 * @brief Obtain a read/write file descriptor for a QDMA qpair.
 *
 * The descriptor can be used with read()/write() for C2H/H2C data transfer.
 *
 * @param fd        Connected vrtd socket file descriptor.
 * @param dev       Device index (0‑based).
 * @param qid       Qpair identifier as returned by vrtd_qdma_qpair_add().
 * @param flags     OR of O_CLOEXEC and 0 (other flags are rejected by the daemon).
 * @param fd_out    Output pointer to receive the qpair file descriptor.
 *
 * @return #VRTD_RET_OK on success; otherwise a #vrtd_ret error code.
 * @pre @p fd_out must not be NULL.
 */
enum vrtd_ret vrtd_qdma_qpair_get_fd(
    int fd,
    uint32_t dev,
    uint32_t qid,
    uint32_t flags,
    int *fd_out
);

/**
 * @brief Open a buffer (allocation + QDMA qpair) and obtain its FD.
 *
 * Requests a device memory allocation and creates a QDMA qpair for it.
 * The returned file descriptor is owned by the caller and should be closed
 * when no longer needed.
 *
 * @param fd         Connected vrtd socket file descriptor.
 * @param dev        Device index (0‑based).
 * @param alloc_type Allocation type (one of enum vrtd_alloc_type).
 * @param alloc_dir  QDMA direction (one of enum vrtd_alloc_dir).
 * @param alloc_arg  Allocation argument (HBM region index for HBM).
 * @param size_in     Requested size in bytes.
 * @param buffer_out  Output pointer to receive the allocated buffer handle.
 *
 * @return #VRTD_RET_OK on success; otherwise a #vrtd_ret error code.
 * @pre @p buffer_out must not be NULL.
 * @note The returned buffer must be released with @c vrtd_buffer_destroy().
 */
enum vrtd_ret vrtd_buffer_open(
    int fd,
    uint32_t dev,
    uint32_t alloc_type,
    uint32_t alloc_dir,
    uint64_t alloc_arg,
    uint64_t size_in,
    struct vrtd_buffer **buffer_out
);

/**
 * @brief Open a raw buffer (QDMA qpair at caller-specified device address) via vrtd.
 *
 * Bypasses the allocator entirely — the caller is responsible for ensuring the
 * address is valid and not in use.  Requires the @c raw-mem-access permission.
 *
 * @param fd          Connected vrtd socket file descriptor.
 * @param dev         Device index (0‑based).
 * @param phys_addr   Caller-specified device physical address.
 * @param size        Size in bytes.
 * @param alloc_dir   One of #vrtd_alloc_dir.
 * @param buffer_out  Output parameter set to the new buffer handle on success.
 *
 * @return #VRTD_RET_OK on success; otherwise a #vrtd_ret error code.
 * @pre @p buffer_out must not be NULL.
 * @note The returned buffer must be released with @c vrtd_buffer_destroy().
 */
enum vrtd_ret vrtd_buffer_open_raw(
    int fd,
    uint32_t dev,
    uint64_t phys_addr,
    uint64_t size,
    uint32_t alloc_dir,
    struct vrtd_buffer **buffer_out
);

/**
 * @brief Close a buffer (release allocation + QDMA qpair) via vrtd.
 *
 * On success or failure, the local buffer is destroyed and must not be used.
 *
 * @param buffer  Buffer handle returned by @c vrtd_buffer_open().
 *
 * @return #VRTD_RET_OK on success; otherwise a #vrtd_ret error code.
 * @pre @p buffer must not be NULL.
 */
enum vrtd_ret vrtd_buffer_close(
    struct vrtd_buffer *buffer
);

/**
 * @brief Perform a design writer transfer for a device.
 *
 * The input FD is sent to the daemon via SCM_RIGHTS. On success the daemon
 * takes ownership of the FD and blocks until the transfer completes.
 *
 * @param fd         Connected vrtd socket file descriptor.
 * @param dev        Device index (0‑based).
 * @param input_fd   Input file descriptor to read from.
 *
 * @return #VRTD_RET_OK on success; #VRTD_RET_BUSY if a transfer is in progress;
 *         otherwise a #vrtd_ret error code.
 * @pre @p input_fd must be a valid, readable file descriptor.
 */
enum vrtd_ret vrtd_design_write(
    int fd,
    uint32_t dev,
    int input_fd
);

/**
 * @brief Open a file and perform a design writer transfer for a device.
 *
 * Convenience helper that opens @p path read-only and passes the FD to the
 * daemon via vrtd_design_write(). The FD is closed before returning.
 *
 * @param fd         Connected vrtd socket file descriptor.
 * @param dev        Device index (0‑based).
 * @param path       Path to the input file to transfer.
 *
 * @return #VRTD_RET_OK on success; #VRTD_RET_BUSY if a transfer is in progress;
 *         otherwise a #vrtd_ret error code.
 * @pre @p path must not be NULL.
 */
enum vrtd_ret vrtd_design_write_file(
    int fd,
    uint32_t dev,
    const char *path
);

/**
 * @brief Perform a PCIe hotplug operation for a device.
 *
 * For board-level operations (RESCAN, RESET_SEQUENCE), @p function is ignored.
 * For PF-level operations (REMOVE, TOGGLE_SBR, HOTPLUG), @p function selects
 * the PCI physical function (0-7).
 *
 * @param fd       Connected vrtd socket file descriptor.
 * @param dev      Device index (0-based).
 * @param op       One of vrtd_device_hotplug_op.
 * @param function PCI function number (0-7) for PF-level ops.
 *
 * @return #VRTD_RET_OK on success; otherwise a #vrtd_ret error code.
 */
enum vrtd_ret vrtd_device_hotplug_op(
    int fd,
    uint32_t dev,
    uint8_t op,
    uint8_t function
);

enum vrtd_ret vrtd_device_hotplug_rescan(
    int fd,
    uint32_t dev
);

enum vrtd_ret vrtd_device_hotplug_remove(
    int fd,
    uint32_t dev,
    uint8_t function
);

enum vrtd_ret vrtd_device_hotplug_toggle_sbr(
    int fd,
    uint32_t dev,
    uint8_t function
);

enum vrtd_ret vrtd_device_hotplug_hotplug(
    int fd,
    uint32_t dev,
    uint8_t function
);

/**
 * @brief Get the clock rate for a device region.
 *
 * @param fd           Connected vrtd socket file descriptor.
 * @param dev          Device index (0‑based).
 * @param region       One of vrtd_clock_region.
 * @param rate_hz_out  Output pointer for the current rate in Hz.
 *
 * @return #VRTD_RET_OK on success; otherwise a #vrtd_ret error code.
 * @pre @p rate_hz_out must not be NULL.
 */
enum vrtd_ret vrtd_clock_get_rate(
    int fd,
    uint32_t dev,
    uint32_t region,
    uint32_t *rate_hz_out
);

/**
 * @brief Set the clock rate for a device region.
 *
 * @param fd            Connected vrtd socket file descriptor.
 * @param dev           Device index (0‑based).
 * @param region        One of vrtd_clock_region.
 * @param rate_hz_in    Requested rate in Hz.
 * @param rate_hz_out   Output pointer for the achieved rate in Hz.
 *
 * @return #VRTD_RET_OK on success; otherwise a #vrtd_ret error code.
 * @pre @p rate_hz_out must not be NULL.
 */
enum vrtd_ret vrtd_clock_set_rate(
    int fd,
    uint32_t dev,
    uint32_t region,
    uint32_t rate_hz_in,
    uint32_t *rate_hz_out
);


struct vrtd_buffer {
    int sock_fd;
    uint32_t dev;

    uint32_t alloc_type;
    uint32_t alloc_dir;
    uint64_t alloc_arg;

    uint64_t size;
    uint64_t phys_addr;
    int qpair_fd;
    void *buf;
};

enum vrtd_ret vrtd_buffer_create_raw(
    int sock_fd,
    uint32_t dev,
    uint32_t alloc_type,
    uint32_t alloc_dir,
    uint64_t alloc_arg,
    uint64_t size,
    uint64_t phys_addr,
    int qpair_fd,
    struct vrtd_buffer **buffer_out
);

/**
 * @brief Destroy a local buffer handle.
 *
 * This does not notify the daemon. Use @c vrtd_buffer_close() to release
 * the server-side allocation.
 */
enum vrtd_ret vrtd_buffer_destroy(
    struct vrtd_buffer *buffer
);

enum vrtd_ret vrtd_buffer_sync_to_device(
    struct vrtd_buffer *buffer,
    uint64_t offset,
    uint64_t size
);

enum vrtd_ret vrtd_buffer_sync_from_device(
    struct vrtd_buffer *buffer,
    uint64_t offset,
    uint64_t size
);


/**
 * @brief Query sensor information for a device.
 *
 * Retrieves all sensor readings (temperature, power, voltage, current) for
 * the specified device.  The daemon queries sensors on-demand via AMI.
 *
 * The response is a variable-length message: a uint32_t sensor count
 * followed by that many vrtd_sensor_entry structs.  The caller provides
 * a buffer large enough to hold the expected response.
 *
 * @param fd             Connected vrtd socket file descriptor.
 * @param dev            Device index (0-based).
 * @param entries_out    Output buffer for sensor entries.
 * @param max_entries    Maximum number of entries @p entries_out can hold.
 * @param num_entries_out Output pointer for the actual number of entries returned.
 *
 * @return #VRTD_RET_OK on success; otherwise a #vrtd_ret error code.
 * @pre @p entries_out and @p num_entries_out must not be NULL.
 */
enum vrtd_ret vrtd_get_sensor_info(
    int fd,
    uint32_t dev,
    struct vrtd_sensor_entry *entries_out,
    uint32_t max_entries,
    uint32_t *num_entries_out
);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // LIBVRTD_VRTD_H
