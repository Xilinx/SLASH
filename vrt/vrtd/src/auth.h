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
 * @file auth.h
 * @brief Role-based authorization checks for vrtd client requests.
 *
 * Every request received from a client passes through a corresponding
 * @c auth_request_* function before execution.  These functions consult the
 * client's assigned @c struct @c role to decide whether the operation is
 * permitted.
 *
 * The permission model works as follows:
 *   - Each client is assigned a merged role at connection time (based on
 *     UID/GID lookups against the daemon configuration).
 *   - The role contains:
 *       - A set of allowed device indices (or a wildcard "allow any" flag).
 *       - A BAR-level access policy controlling which BARs may be mmap'd.
 *       - Boolean flags for query, PCIe hotplug, and other operation classes.
 *   - An @c auth_request_* function returns 0 if the request is authorized,
 *     or populates the client's outbound buffer with an ACCESS_DENIED response
 *     and returns a non-zero value to short-circuit the handler.
 *
 * Every auth function follows the same signature convention:
 * @code
 *   int auth_request_<operation>(struct client *client,
 *                                const struct vrtd_req_<operation> *req_body);
 * @endcode
 */

#ifndef VRTD_AUTH_H
#define VRTD_AUTH_H

#include "serve.h"

/**
 * @brief Authorize a GET_DEVICE_INFO request.
 *
 * Checks that the client's role permits querying and accessing the
 * specified device index.
 *
 * @param client   The requesting client (carries the role and response buffer).
 * @param req_body The parsed request body containing the target device index.
 * @return 0 if authorized, non-zero if denied (response buffer populated).
 */
int auth_request_get_device_info(
    struct client *client,
    const struct vrtd_req_get_device_info *req_body
);

/**
 * @brief Authorize a GET_DEVICE_BY_BDF request.
 *
 * Checks that the client's role permits querying devices by PCI
 * Bus/Device/Function address.
 *
 * @param client   The requesting client.
 * @param req_body The parsed request body containing the target BDF.
 * @return 0 if authorized, non-zero if denied.
 */
int auth_request_get_device_by_bdf(
    struct client *client,
    const struct vrtd_req_get_device_by_bdf *req_body
);

/**
 * @brief Authorize a GET_NUM_DEVICES request.
 *
 * Checks that the client's role permits device enumeration queries.
 *
 * @param client   The requesting client.
 * @param req_body The parsed request body (may be empty for this query).
 * @return 0 if authorized, non-zero if denied.
 */
int auth_request_get_num_devices(
    struct client *client,
    const struct vrtd_req_get_num_devices *req_body
);

/**
 * @brief Authorize a GET_BAR_INFO request.
 *
 * Checks that the client's role permits accessing the specified device
 * and querying BAR metadata.
 *
 * @param client   The requesting client.
 * @param req_body The parsed request body containing device and BAR indices.
 * @return 0 if authorized, non-zero if denied.
 */
int auth_request_get_bar_info(
    struct client *client,
    const struct vrtd_req_get_bar_info *req_body
);

/**
 * @brief Authorize a GET_BAR_FD request.
 *
 * Checks that the client's role permits mmap access to the requested BAR
 * on the specified device, per the role's per-device bar-access policy.
 *
 * @param client   The requesting client.
 * @param req_body The parsed request body containing device and BAR indices.
 * @return 0 if authorized, non-zero if denied.
 */
int auth_request_get_bar_fd(
    struct client *client,
    const struct vrtd_req_get_bar_fd *req_body
);

/**
 * @brief Authorize a QDMA_GET_INFO request.
 *
 * Checks that the client's role permits querying QDMA subsystem
 * information on the specified device.
 *
 * @param client   The requesting client.
 * @param req_body The parsed request body containing the target device index.
 * @return 0 if authorized, non-zero if denied.
 */
int auth_request_qdma_get_info(
    struct client *client,
    const struct vrtd_req_qdma_get_info *req_body
);

/**
 * @brief Authorize a QDMA_QPAIR_ADD request.
 *
 * Checks that the client's role permits adding a QDMA queue pair
 * on the specified device.
 *
 * @param client   The requesting client.
 * @param req_body The parsed request body containing device index and queue parameters.
 * @return 0 if authorized, non-zero if denied.
 */
int auth_request_qdma_qpair_add(
    struct client *client,
    const struct vrtd_req_qdma_qpair_add *req_body
);

/**
 * @brief Authorize a QDMA_QPAIR_OP request (start/stop/delete).
 *
 * Checks that the client's role permits queue pair lifecycle operations
 * on the specified device.
 *
 * @param client   The requesting client.
 * @param req_body The parsed request body containing device index and queue ID.
 * @return 0 if authorized, non-zero if denied.
 */
int auth_request_qdma_qpair_op(
    struct client *client,
    const struct vrtd_req_qdma_qpair_op *req_body
);

/**
 * @brief Authorize a QDMA_QPAIR_GET_FD request.
 *
 * Checks that the client's role permits obtaining a file descriptor for
 * a QDMA queue pair on the specified device (fd is passed via SCM_RIGHTS).
 *
 * @param client   The requesting client.
 * @param req_body The parsed request body containing device index and queue ID.
 * @return 0 if authorized, non-zero if denied.
 */
int auth_request_qdma_qpair_get_fd(
    struct client *client,
    const struct vrtd_req_qdma_qpair_get_fd *req_body
);

/**
 * @brief Authorize a BUFFER_OPEN request.
 *
 * Checks that the client's role permits allocating a DMA buffer
 * (HBM or DDR) on the specified device.
 *
 * @param client   The requesting client.
 * @param req_body The parsed request body containing device index, size, and memory type.
 * @return 0 if authorized, non-zero if denied.
 */
int auth_request_buffer_open(
    struct client *client,
    const struct vrtd_req_buffer_open *req_body
);

/**
 * @brief Authorize a BUFFER_OPEN_RAW request.
 *
 * Checks that the client's role permits opening a raw DMA buffer
 * (bypassing the allocator) on the specified device.
 *
 * @param client   The requesting client.
 * @param req_body The parsed request body containing device index and address.
 * @return 0 if authorized, non-zero if denied.
 */
int auth_request_buffer_open_raw(
    struct client *client,
    const struct vrtd_req_buffer_open_raw *req_body
);

/**
 * @brief Authorize a BUFFER_CLOSE request.
 *
 * Checks that the client's role permits deallocating a DMA buffer
 * and that the buffer belongs to this client.
 *
 * @param client   The requesting client.
 * @param req_body The parsed request body containing device index and buffer address.
 * @return 0 if authorized, non-zero if denied.
 */
int auth_request_buffer_close(
    struct client *client,
    const struct vrtd_req_buffer_close *req_body
);

/**
 * @brief Authorize a DESIGN_WRITE request (FPGA bitstream programming).
 *
 * Checks that the client's role permits writing a bitstream to the
 * specified device.  The bitstream file descriptor is received via
 * SCM_RIGHTS ancillary data.
 *
 * @param client   The requesting client.
 * @param req_body The parsed request body containing the target device index.
 * @return 0 if authorized, non-zero if denied.
 */
int auth_request_design_write(
    struct client *client,
    const struct vrtd_req_design_write *req_body
);

/**
 * @brief Authorize a DEVICE_HOTPLUG_OP request (PCIe SBR toggle).
 *
 * Checks that the client's role has the @c pcie_hotplug permission
 * for the specified device.
 *
 * @param client   The requesting client.
 * @param req_body The parsed request body containing the target device index.
 * @return 0 if authorized, non-zero if denied.
 */
int auth_request_device_hotplug_op(
    struct client *client,
    const struct vrtd_req_device_hotplug_op *req_body
);

/**
 * @brief Authorize a CLOCK_OP request (get/set clock frequency).
 *
 * Checks that the client's role permits clock control operations
 * on the specified device.
 *
 * @param client   The requesting client.
 * @param req_body The parsed request body containing device index and clock parameters.
 * @return 0 if authorized, non-zero if denied.
 */
int auth_request_clock_op(
    struct client *client,
    const struct vrtd_req_clock_op *req_body
);

/**
 * @brief Authorize a GET_SENSOR_INFO request.
 *
 * Checks that the client's role permits querying sensor information
 * on the specified device.  This is a query-only operation.
 *
 * @param client   The requesting client.
 * @param req_body The parsed request body containing the target device index.
 * @return 0 if authorized, non-zero if denied.
 */
int auth_request_get_sensor_info(
    struct client *client,
    const struct vrtd_req_get_sensor_info *req_body
);

#endif // VRTD_AUTH_H
