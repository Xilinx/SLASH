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
 * @file wire.h
 * @brief On‑wire protocol for vrtd (V80 Runtime Daemon).
 *
 * Transport:
 *  - UNIX domain sockets (AF_UNIX, SOCK_SEQPACKET). Messages are record‑oriented.
 *  - File descriptors may be sent out‑of‑band using SCM_RIGHTS.
 *
 * Framing:
 *  - Each message = { header, body }.
 *  - Total size (header + body) MUST be <= VRTD_MSG_MAX_SIZE.
 *
 * Sequencing:
 *  - Requests carry a client‑chosen @ref vrtd_req_header::seqno that is echoed
 *    unmodified by the server in @ref vrtd_resp_header::seqno.
 *
 * Versioning/Extensibility:
 *  - Unknown opcodes result in VRTD_RET_BAD_REQUEST.
 *  - New fields may be added at the *end* of messages; older peers must ignore
 *    trailing bytes up to @ref vrtd_resp_header::size.
 *
 * Security:
 *  - Server enforces permissions; failures surface as VRTD_RET_AUTH_ERROR.
 */

#ifndef VRTD_WIRE_H
#define VRTD_WIRE_H

#include <stdint.h>

#include <slash/uapi/slash_interface.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum total size (header + body) for any vrtd message in bytes. */
#define VRTD_MSG_MAX_SIZE 4096

/**
 * @brief Operations the client can request from the server.
 * @note Unknown/unsupported opcodes yield VRTD_RET_BAD_REQUEST.
 */
enum vrtd_opcode {
    /** Query the number of SLASH devices. */
    VRTD_REQ_GET_NUM_DEVICES,

    /** Query basic information about a device. */
    VRTD_REQ_GET_DEVICE_INFO,

    /** Query metadata about a device BAR. */
    VRTD_REQ_GET_BAR_INFO,

    /** Obtain a device BAR file descriptor via SCM_RIGHTS. */
    VRTD_REQ_GET_BAR_FD,

    /** Query QDMA capabilities of a device. */
    VRTD_REQ_QDMA_GET_INFO,

    /** Create a QDMA qpair on a device. */
    VRTD_REQ_QDMA_QPAIR_ADD,

    /** Apply an operation (start/stop/del) to a QDMA qpair. */
    VRTD_REQ_QDMA_QPAIR_OP,

    /** Obtain a read/write file descriptor for a QDMA qpair. */
    VRTD_REQ_QDMA_QPAIR_GET_FD,

    /** Perform a design writer transfer by passing an input fd via SCM_RIGHTS. */
    VRTD_REQ_DESIGN_WRITE,

    /** Get or set a clock rate for the service/user region. */
    VRTD_REQ_CLOCK_OP,

    /** Open a buffer (allocation + QDMA qpair) and return a qpair fd. */
    VRTD_REQ_BUFFER_OPEN,

    /** Close a buffer (release allocation + QDMA qpair). */
    VRTD_REQ_BUFFER_CLOSE,

    /** Query a device index by PCI BDF. */
    VRTD_REQ_GET_DEVICE_BY_BDF,

    /** Perform a PCIe hotplug operation for a device. */
    VRTD_REQ_DEVICE_HOTPLUG_OP,

    /** Query sensor information for a device via AMI. */
    VRTD_REQ_GET_SENSOR_INFO,

    /** Open a raw buffer (QDMA qpair at caller-specified device address, bypassing allocator). */
    VRTD_REQ_BUFFER_OPEN_RAW,
};

/**
 * @brief Return codes for vrtd operations.
 *
 * @warning VRTD_RET_BAD_LIB_CALL and VRTD_RET_BAD_CONN are **client‑local**
 *          and are never returned by the server on the wire.
 */

enum vrtd_ret {
    VRTD_RET_OK,
    VRTD_RET_BAD_LIB_CALL, ///< Bad library call to libvrtd. This code will not be returned on the wire.
    VRTD_RET_BAD_CONN, ///< libvrtd could not connect to vrtd. This code will not be returned on the wire.
    VRTD_RET_BAD_REQUEST, ///< Malformed request.
    VRTD_RET_INVALID_ARGUMENT, ///< Invalid argument.
    VRTD_RET_NOEXIST, ///< Requested resource does not exist.
    VRTD_RET_INTERNAL_ERROR, ///< Internal error in the vrtd daemon. Check the vrtd log.
    VRTD_RET_AUTH_ERROR, ///< User does not have permission to execute request.
    VRTD_RET_BUSY, ///< Requested resource is busy.
};

/**
 * @brief Allocation types for buffer requests.
 */
enum vrtd_alloc_type {
    VRTD_ALLOC_TYPE_DDR = 0,
    VRTD_ALLOC_TYPE_HBM = 1,
    VRTD_ALLOC_TYPE_HBM_VNOC = 2,
};

/**
 * @brief Direction for data transfers for allocated buffer.
 */
enum vrtd_alloc_dir {
    VRTD_ALLOC_DIR_BIDIRECTIONAL = 0,
    VRTD_ALLOC_DIR_HOST_TO_DEVICE = 1,
    VRTD_ALLOC_DIR_DEVICE_TO_HOST = 2,
};

#define VRTD_PCI_BDF_LEN 32

struct vrtd_pci_info {
    char bdf[VRTD_PCI_BDF_LEN];
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_device_id;
} __attribute__((packed));

struct vrtd_req_header {
    uint16_t size; ///< Size of the request body (not including the header).
    uint16_t opcode; ///< See @ref vrtd_opcode.
    uint32_t seqno; ///< Sequence number (this will simply be echoed by the server in the response header).
} __attribute__((packed));

struct vrtd_resp_header {
    uint16_t size; ///< Size of the response body (not including the header).
    uint16_t ret; ///< See @ref vrtd_ret.
    uint32_t seqno; ///< Sequence number (this is simply echoed from the request header).
} __attribute__((packed));

/**
 * @brief Placeholder body to avoid empty-struct ABI pitfalls across C/C++.
 * @note Must be set to zero by clients; servers must ignore its value.
 */
struct vrtd_req_get_num_devices {
    uint8_t zero;
} __attribute__((packed));


struct vrtd_resp_get_num_devices {
    uint32_t num_devices; ///< Number of SLASH devices known to the server. They are identified by numbers in the range [0, n).
} __attribute__((packed));


struct vrtd_req_get_device_info {
    uint32_t dev_number; ///< The device for which to get info. An index in the range [0, n).
} __attribute__((packed));

struct vrtd_device_info {
    char name[128]; ///< The name of the device.
    struct vrtd_pci_info pci; ///< PCIe metadata (BDF and IDs).
} __attribute__((packed));

struct vrtd_resp_get_device_info {
    struct vrtd_device_info info;
} __attribute__((packed));

struct vrtd_req_get_device_by_bdf {
    char bdf[VRTD_PCI_BDF_LEN]; ///< PCI BDF string (e.g., 0000:65:00.0)
} __attribute__((packed));

struct vrtd_resp_get_device_by_bdf {
    uint32_t dev_number; ///< Device index (0-based).
} __attribute__((packed));

struct vrtd_req_get_bar_info {
    uint32_t dev_number; ///< The device for which to get info. An index in the range [0, n).
    uint8_t bar_number; ///< The BAR for which to get info. An index in the range [0, 6).
} __attribute__((packed));

struct vrtd_resp_get_bar_info {
    struct slash_ioctl_bar_info bar_info; ///< The structure with BAR information.
} __attribute__((packed));

struct vrtd_req_get_bar_fd {
    uint32_t dev_number; ///< The device for who's BAR to get a file descriptor. An index in the range [0, n).
    uint8_t bar_number; ///< The BAR for which to get a file descriptor. An index in the range [0, 6).
} __attribute__((packed));

/**
 * @brief Response to VRTD_REQ_GET_BAR_FD.
 *
 * The BAR file descriptor is sent out-of-band via SCM_RIGHTS in the same
 * message and is present only when @ref vrtd_resp_header::ret == VRTD_RET_OK.
 */
struct vrtd_resp_get_bar_fd {
    uint64_t len; ///< Size of the BAR address space; suitable for mmap.
} __attribute__((packed));

/**
 * @brief Request QDMA capability information for a device.
 *
 * Complementary to @c slash_qdma_info; this wraps the libslash QDMA
 * info query and exposes it over the vrtd protocol.
 */
struct vrtd_req_qdma_get_info {
    uint32_t dev_number; ///< The device for which to get QDMA info. An index in the range [0, n).
} __attribute__((packed));

struct vrtd_resp_qdma_get_info {
    struct slash_qdma_info info; ///< QDMA capabilities for the device.
} __attribute__((packed));

/**
 * @brief Request creation of a QDMA qpair.
 *
 * The @c slash_qdma_qpair_add payload is passed through to the kernel
 * and the resulting qid is returned in the response.
 */
struct vrtd_req_qdma_qpair_add {
    uint32_t dev_number; ///< Device index (0-based).
    struct slash_qdma_qpair_add add; ///< Qpair creation parameters.
} __attribute__((packed));

struct vrtd_resp_qdma_qpair_add {
    struct slash_qdma_qpair_add add; ///< Echoed qpair parameters with qid filled in.
} __attribute__((packed));

/**
 * @brief Request an operation on an existing QDMA qpair.
 *
 * @ref op uses the same numeric values as @c SLASH_QDMA_QUEUE_OP_START and friends.
 */
struct vrtd_req_qdma_qpair_op {
    uint32_t dev_number; ///< Device index (0-based).
    uint32_t qid;        ///< Qpair identifier as returned by qpair_add.
    uint32_t op;         ///< One of SLASH_QDMA_QUEUE_OP_{START,STOP,DEL}.
} __attribute__((packed));

struct vrtd_resp_qdma_qpair_op {
    uint8_t zero; ///< Placeholder to avoid empty-struct ABI issues.
} __attribute__((packed));

/**
 * @brief Request a read/write file descriptor for a QDMA qpair.
 *
 * The qpair FD is sent out-of-band via SCM_RIGHTS when
 * @ref vrtd_resp_header::ret == VRTD_RET_OK.
 */
struct vrtd_req_qdma_qpair_get_fd {
    uint32_t dev_number; ///< Device index (0-based).
    uint32_t qid;        ///< Qpair identifier as returned by qpair_add.
    uint32_t flags;      ///< Only O_CLOEXEC is currently honored.
} __attribute__((packed));

struct vrtd_resp_qdma_qpair_get_fd {
    uint8_t zero; ///< Placeholder; all data is carried via SCM_RIGHTS.
} __attribute__((packed));

/**
 * @brief Request a buffer (allocation + QDMA qpair) and a qpair FD.
 *
 * The qpair FD is sent out-of-band via SCM_RIGHTS when
 * @ref vrtd_resp_header::ret == VRTD_RET_OK.
 */
struct vrtd_req_buffer_open {
    uint32_t dev_number; ///< Device index (0-based).
    uint32_t alloc_type; ///< One of enum vrtd_alloc_type.
    uint32_t alloc_dir;  ///< One of enum vrtd_alloc_dir.
    uint64_t alloc_arg;  ///< Allocation argument (HBM region index for HBM).
    uint64_t size;       ///< Requested size in bytes.
} __attribute__((packed));

struct vrtd_resp_buffer_open {
    uint64_t size; ///< Allocated size in bytes (rounded up to subregion).
    uint64_t phys_addr; ///< Device physical address of the allocation.
} __attribute__((packed));

/**
 * @brief Request closing a buffer (release allocation + QDMA qpair).
 */
struct vrtd_req_buffer_close {
    uint32_t dev_number; ///< Device index (0-based).
    uint64_t phys_addr;  ///< Device physical address of the allocation.
    uint64_t size;       ///< Allocated size in bytes.
} __attribute__((packed));

struct vrtd_resp_buffer_close {
    uint8_t zero; ///< Placeholder to avoid empty-struct ABI issues.
} __attribute__((packed));

/**
 * @brief Request a raw buffer (QDMA qpair at caller-specified device address).
 *
 * Bypasses the allocator entirely — the caller is responsible for ensuring the
 * address is valid and not in use.  Requires the @c raw-mem-access permission.
 *
 * The qpair FD is sent out-of-band via SCM_RIGHTS when
 * @ref vrtd_resp_header::ret == VRTD_RET_OK.
 */
struct vrtd_req_buffer_open_raw {
    uint32_t dev_number; ///< Device index (0-based).
    uint32_t alloc_dir;  ///< One of enum vrtd_alloc_dir.
    uint64_t phys_addr;  ///< Caller-specified device physical address (bypasses allocator).
    uint64_t size;       ///< Size in bytes.
} __attribute__((packed));

struct vrtd_resp_buffer_open_raw {
    uint8_t zero; ///< Placeholder; all data is carried via SCM_RIGHTS.
} __attribute__((packed));

/**
 * @brief Request a design writer transfer.
 *
 * The input file descriptor is sent out-of-band via SCM_RIGHTS.
 */
struct vrtd_req_design_write {
    uint32_t dev_number; ///< Device index (0-based).
} __attribute__((packed));

struct vrtd_resp_design_write {
    uint8_t zero; ///< Placeholder; all data is carried via SCM_RIGHTS.
} __attribute__((packed));

enum vrtd_device_hotplug_op {
    VRTD_DEVICE_HOTPLUG_OP_RESCAN = 0,
    VRTD_DEVICE_HOTPLUG_OP_REMOVE = 1,
    VRTD_DEVICE_HOTPLUG_OP_TOGGLE_SBR = 2,
    VRTD_DEVICE_HOTPLUG_OP_HOTPLUG = 3,
    VRTD_DEVICE_HOTPLUG_OP_RESET_SEQUENCE = 4,
};

/**
 * @brief Request a PCIe hotplug operation for a device.
 *
 * For board-level operations (RESCAN, RESET_SEQUENCE), only dev_number
 * and op are required; the function field is ignored.
 *
 * For PF-level operations (REMOVE, TOGGLE_SBR, HOTPLUG), the function
 * field selects the PCI physical function (0-7).  These operations are
 * SLASH-agnostic shortcuts to the kernel hotplug interface.
 */
struct vrtd_req_device_hotplug_op {
    uint32_t dev_number; ///< Device index (0-based).
    uint8_t op;          ///< One of vrtd_device_hotplug_op.
    uint8_t function;    ///< PCI function number (0-7) for PF-level ops.
} __attribute__((packed));

struct vrtd_resp_device_hotplug_op {
    uint8_t zero; ///< Placeholder to avoid empty-struct ABI issues.
} __attribute__((packed));

enum vrtd_clock_region {
    VRTD_CLOCK_REGION_SERVICE = 0,
    VRTD_CLOCK_REGION_USER = 1,
};

enum vrtd_clock_op {
    VRTD_CLOCK_OP_GET = 0,
    VRTD_CLOCK_OP_SET = 1,
};

/**
 * @brief Request a clock operation (get/set) for a region.
 */
struct vrtd_req_clock_op {
    uint32_t dev_number; ///< Device index (0-based).
    uint32_t rate_hz;    ///< Desired rate for SET; ignored for GET.
    uint8_t op;         ///< One of vrtd_clock_op.
    uint8_t region;     ///< One of vrtd_clock_region.
} __attribute__((packed));

struct vrtd_resp_clock_op {
    uint32_t rate_hz; ///< Current/achieved rate for GET/SET.
} __attribute__((packed));

/**
 * @brief Maximum number of sensor entries that fit in a single response message.
 */
#define VRTD_SENSOR_MAX_ENTRIES \
    ((VRTD_MSG_MAX_SIZE - sizeof(struct vrtd_resp_header) - sizeof(uint32_t)) \
     / sizeof(struct vrtd_sensor_entry))

/**
 * @brief A single sensor reading.
 *
 * Each entry corresponds to one (sensor-name, sensor-type) pair.
 * For example, "vccint" may produce separate entries for temperature,
 * voltage, current, and power.
 */
struct vrtd_sensor_entry {
    char name[64];   ///< Sensor name (e.g., "vccint").
    uint8_t type;    ///< Sensor type bitmask (1=temp, 2=current, 4=voltage, 8=power).
    uint8_t status;  ///< Sensor status (0x01 = OK, see AMI sensor status codes).
    int8_t unit_mod; ///< Unit modifier exponent (e.g., -3 for milli-).
    uint8_t _pad;    ///< Reserved, must be zero.
    int32_t value;   ///< Sensor reading (apply 10^unit_mod to get base unit value).
} __attribute__((packed));

/**
 * @brief Request sensor information for a device.
 *
 * The daemon opens an AMI handle, discovers sensors, reads their values,
 * and returns them all in the response.
 */
struct vrtd_req_get_sensor_info {
    uint32_t dev_number; ///< Device index (0-based).
} __attribute__((packed));

/**
 * @brief Response to VRTD_REQ_GET_SENSOR_INFO.
 *
 * Contains a variable number of sensor entries.  The actual response size
 * is sizeof(num_sensors) + num_sensors * sizeof(struct vrtd_sensor_entry).
 */
struct vrtd_resp_get_sensor_info {
    uint32_t num_sensors; ///< Number of sensor entries following.
    struct vrtd_sensor_entry sensors[]; ///< Variable-length array of sensor entries.
} __attribute__((packed));

#ifdef __cplusplus
} // extern "C"
#endif

#endif // VRTD_WIRE_H
