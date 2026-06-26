# System-emulated accelerators

Idea: A user-space daemon emulates the behavior of the SLASH driver and the underlying hardware, so that user applications can be tested within the broader software setup of SLASH.

Instead of exposing control files, the emulation daemon exposes UNIX domain sockets with identical names, and instead of IOCTLs, all operations are messages sent over the socket. Where an IOCTL is supposed to return a file descriptor, the daemon's response is instead a return value of zero (i.e. success), and the file descriptor is instead transferred as ancillary data as `SCM_RIGHTS`. Where the inputs of an IOCTL are supposed to be file descriptors, the client instead transfers the corresponding file descriptors as ancillary data and references the file descriptors in the sent list by index.

The difference between the driver's ABI and its emulation (IOCTLs vs socket datagrams) is resolved in libslash: When opening a top-level handle, users also have to set a flag whether the opened file is a control file or a UNIX domain socket. This information is then handled by libslash accordingly and also forwarded to newly created constructs.

The following document describes the requirements for the system emulation daemon, as well as some necessary changes to the kernel driver, libslash, and the entire stack that depends on it.

## General notes

This endeavour introduces a new concept "system emulation", independent of the existing "FPGA emulation" and "FPGA simulation" concepts. "System emulation" is the emulation of the entire accelerator in the host system, i.e. how it is handled by the user application, VRT, and VRTD. Contrarily, "FPGA emulation" and "FPGA simulation" describe ways to predict the behavior of a group of kernels on the FPGA in software. How the actual FPGA's behavior is predicted doesn't matter much for the system emulation that we want to introduce, and system emulation can be combined both with FPGA emulation or FPGA simulation.

This emulation does not cover the board management via the primary function 0. This is handled by the ami driver.

What can be implemented thread-safe should be implemented thread-safe.

## Message format

``` C
struct slash_emu_socket_header {
    __u32 ioctl_id;         /**< The IOCTL to emulate */
    __u32 sequence_id;      /**< A monotonically increasing sequence number */
    __u32 return_value;     /**< The return value of the IOCTL, can be set arbitrarly for requests */
    __u32 pad;              /**< Padding */
};
```

* Each datagram first contains the `struct slash_emu_socket_header`
    * Both for requests from the user to the daemon, and for the response from the daemon to the user
    * The return value may have an arbitrary value in a request
        * Disregarded by the daemon
    * Is set to the ioctl return value in the response
* Header is then followed by the corresponding IOCTL argument struct
    * Input fields must be set by the user for the request
    * Daemon sets the output fields
        * Leaving the input fields as is
    * Returns the full argument struct in the response
* On error, the `return_value` in the response is set to the `-errno`.
* On success, the `return_value` is zero.
* If one or more input fields are file descriptors
    * The user sends all file descriptors they want to use as one array of FDs via SCM_RIGHTS
    * Uses indices to the list of transferred FDs instead of FDs
* If the return value of the original IOCTL is a file descriptor, the return value is still zero
    * Instead, the daemon sends the FD as ancillary data via SCM_RIGHTS

* In the following, all IOCTLs from the original specification are listed
    * If not specified otherwise, either above or in the listing, the original kernel ABI specification applies.

## Rule for future IOCTLs/ABI designs

* IOCTLs may freely pass file descriptors around
* But they may not pass pointers to user's virtual memory space
    * These can't be meaningfully transferred between processes over a UNIX domain socket

## Exposed files

Just like the real driver, the daemon exposes multiple files/sockets for different accelerators and subsystems. These are:
* `slash_ctl<N>`: Provides BAR enumeration, MMIO access, and PCI device identity.
    * One socket per accelerator
* `slash_qdma_ctl<N>`: Manages DMA queue pairs for buld data movement between hsot and card memory, as well as reconfiguration.
    * One socket per accelerator
* `slash_hotplug`: Provides privileged control over the lifecycle of SLASH cards
    * Single, daemon-level instance
    * Emulates remove, rescan, secondary bus reset
    * In practise: Tears down emulated accelerators, reloads the configuration file

* Base directory, uid/gid of each file, and mode of each socket are configurable or given as CLI arguments
    * Default is `/run/slash_emu`, `vrtd:vrt`, 600

## Reconfiguration/FPGA model execution

* In this context, "reconfiguration" stands for the reconfiguration of the FPGA
    * I.e. changing the executed program
* Difference between emulation and hardware: Accelerator model is reconfigured with the full VBIN, not just DCPs/PDIs
    * And only VBINs with either "emulation" or "simulation" target platforms
    * Such VBINs contain a `vpp_emu` or `vpp_sim` executable
    * These are launched by the daemon
        * Requiring special isolation, to be implemented later
    * Daemon and model communicate via ZeroMQ
    * Register reads and writes, as well as buffer reads and writes go via ZeroMQ to the model
* Full VBIN is necessary because:
    * The daemon needs the system map
        * At the very least to tell whether the target platform is emulation or simulation
    * The simulation model is actually shipped as a shared object
        * The `vpp_sim` executable is only a wrapper for it

* At daemon startup, an accelerator does not have an active model
    * All interactions with the model (reads/writes to BARs/DDR/HBM) go into the void
        * Must not fail, since they won't fail with a real, newly booted accelerator
* Each accelerator has a reconfiguration buffer
    * VRTD writes the VBIN into the reconfiguration buffer via QDMA
    * Writes in chunks, always with device address 0x102100000 and chunk lengths up to 0x10000
    * Each chunk is appended to the reconfiguration buffer
* Once the reconfiguration buffer is written, VRTD removes PF2 (slash_ctl) and executes a RESCAN
* Remove and rescan as described below in the Hotplugging section

## Hotpluging/Resets

* `hotplug` socket
    * Implements the IOCTLs of the `hotplug` device file as datagrams

### SLASH_HOTPLUG_IOCTL_REMOVE/SLASH_HOTPLUG_IOCTL_RESCAN

* Each remove ioctl only removes one physical function.
    * Partial removals therefore have to be handled well

* PF0 (Board management)
    * Board management PF, originally handled by the AMI driver
    * Removal however still done via slash driver
    * Thus: Existance tracked by the daemon
        * First remove successful
        * Following removes fail
        * Accelerator is only fully torn down once PF0 is also removed
        * PF0 is restored on a rescan
* PF1 (QDMA)
    * REMOVE:
        * Tears down the `slash_qdma<N>` socket and worker thread that listens to it
        * Reconfiguration buffer persists until full accelerator shutdown
    * RESCAN on a partial accelerator:
        * New worker thread with `slash_qdma<N>` socket is set up
        * Connected to existing accelerator
* PF2 (BARs/info)
    * REMOVE:
        * Tears down the `slash_ctldev<N>` socket and worker thread that listens to it
        * Model remains running in the background
    * RESCAN on a partial accelerator:
        * If the reconfiguration buffer contains data:
            * Unpack the reconfiguration buffer
            * Clear the reconfiguration buffer
            * Interpret the system map
            * Launch the new model
            * If successful:
                * Shut down the old model
                * Use the new model
        * If no new model was started, reconnect to the old model
        * Create a new `slash_ctldev<N>` socket and worker thread that listens to it

* Simulataneous writing to the reconfiguration buffer and RESCAN leads to a race
    * PF2 restoration may evaluate incomplete data
    * Must be avoided by the user, the daemon can't safeguard against it apart from rejecting incomplete models


### Type definitions:

``` C
#define SLASH_HOTPLUG_BDF_LEN 32

struct slash_hotplug_device_request {
    __u32 size;                        /* ABI version: set to sizeof(struct) */
    char  bdf[SLASH_HOTPLUG_BDF_LEN]; /* NUL-terminated PCI BDF, *including function*, e.g. "0000:03:00.0" */
};
```

## BAR access and device information

TODO

## QDMA subsystem (accelerator memory)

* Top-level control socket exposed as `slash_qdma<N>`, one for each accelerator.
* Resources managed by the daemon:
    * Qpairs
        * No inherent meaning for the daemon
        * Only some state machines to check that the user manages qpairs correctly
    * Transfer sessions
        * Created with the `QPAIR_GET_FD` IOCTL
        * Leads to the creation of a new, anonymous UNIX domain socket
        * Managed by a new worker thread
        * Executes memory transfers between the user and the model server on the user's behalf
* Host buffers need no inherent management by the daemon
    * They are created as memfds and passed to the user
    * But the daemon can (and must) instantly forget about them
        * Memfds don't need to be managed by the daemon
        * If the daemon keeps a reference to them, they are not released once the client closes their last FD to them
    * They will be passed back to the daemon as part of a transfer IOCTL later

### Necessary functional changes to other components

* The info IOCTL now also has to return the BDF of the accelerator
    * Necessary for the emulation daemon since it does not also export `/sys/` files
    * Should also make the discovery for VRTD easier
    * Requires changes in:
        * The kernel driver (needs to report the BDF)
        * The libslash library (needs to forward this information)
        * VRTD (needs to change the discovery mechanism to use BDF returned by QDMA)

### Messages over the socket:

* `SLASH_QDMA_IOCTL_INFO`
    ``` C
    #define SLASH_PCI_BDF_LEN 32

    struct slash_qdma_info {
        __u32 size;        /* [in/out] ABI version */
        __u32 qsets_max;   /* [out] Max queue sets (currently always 0) */
        __u32 msix_qvecs;  /* [out] MSI-X vectors for queues (currently always 0) */
        __u32 vf_max;      /* [out] Max VFs (currently always 0) */
        __u32 caps;        /* [out] Capability bitmask (currently always 0) */
        char  bdf[SLASH_PCI_BDF_LEN]; /* [out] PCI BDF string, NUL-terminated, e.g. "0000:61:00.1" */
    };

    #define SLASH_QDMA_IOCTL_INFO _IOWR('v', 0x50, struct slash_qdma_info)
    ```
    * Addition: The BDF is now returned too
        * Set to the BDF of the QDMA PF, i.e. PF 1
* `SLASH_QDMA_IOCTL_QPAIR_ADD`
    ``` C
    struct slash_qdma_qpair_add {
        __u32 size;          /* [in/out] ABI version */
        __u32 mode;          /* [in]  Queue mode: 0=MM (Memory Mapped), 1=ST (Streaming, not yet supported) */
        __u32 dir_mask;      /* [in]  Direction bitmask (see below) */
        __u32 h2c_ring_sz;   /* [in]  H2C descriptor ring CSR table index: 0–15 */
        __u32 c2h_ring_sz;   /* [in]  C2H descriptor ring CSR table index: 0–15 */
        __u32 cmpt_ring_sz;  /* [in]  Completion ring CSR table index: 0–15 */
        __u32 qid;           /* [out] Kernel-assigned queue pair ID */
    };
    
    #define SLASH_QDMA_IOCTL_QPAIR_ADD _IOWR('v', 0x51, struct slash_qdma_qpair_add)
    ```
* `SLASH_QDMA_IOCTL_Q_OP`
    ``` C
    struct slash_qdma_qpair_op {
        __u32 size; /* [in/out] ABI version */
        __u32 qid;  /* [in]     Queue pair ID from QPAIR_ADD */
        __u32 op;   /* [in]     Operation: 0=START, 1=STOP, 2=DEL */
    };
    #define SLASH_QDMA_IOCTL_Q_OP _IOWR('v', 0x52, struct slash_qdma_qpair_op)
    ```
    * The daemon must track the state machine of each qpair
        * Allows users to find qpair handling errors before moving to hardware
        * Even it may not be necessary to emulate the remaining operations
* `SLASH_QDMA_IOCTL_QPAIR_GET_FD`
    ``` C
    #define SLASH_QDMA_FD_MAX_QPAIRS 2u

    struct slash_qdma_qpair_fd_request {
        __u32 size;        /* [in/out] ABI version */
        __u32 qid;         /* [in]     Legacy single qpair ID; used when qpair_count == 0 */
        __u32 flags;       /* [in]     fd flags: only O_CLOEXEC is honoured */
        __u32 qpair_count; /* [in]     Number of qpair_ids (1..SLASH_QDMA_FD_MAX_QPAIRS); 0 = use qid */
        __u32 qpair_ids[SLASH_QDMA_FD_MAX_QPAIRS]; /* [in] qpair IDs; index == qpair_index */
    };

    #define SLASH_QDMA_IOCTL_QPAIR_GET_FD _IOWR('v', 0x53, struct slash_qdma_qpair_fd_request)
    ```
    * On success, the `return_value` is zero, and the new FD is transferred as ancillary data as SCM_RIGHTS
    * The returned FD points to an anonymous UNIX domain socket
        * Also emulates the IOCTLs of the original control file via datagrams
* `SLASH_QDMA_IOCTL_BUF_CREATE`
    ``` C
    struct slash_qdma_buf_create {
        __u32 size;          /* [in/out] ABI version */
        __u32 flags;         /* [in]  Only O_CLOEXEC is honoured */
        __u64 length;        /* [in]  Buffer length in bytes (page multiple) */
        __u32 granule;       /* [out] Bytes per SGL descriptor (host page size) */
        __u32 transfer_hint; /* [out] enum slash_qdma_transfer_hint */
    };

    #define SLASH_QDMA_IOCTL_BUF_CREATE _IOWR('v', 0x54, struct slash_qdma_buf_create)
    ```
    * On success, the `return_value` is zero, and the new FD is transferred as ancillary data as SCM_RIGHTS
    * The returned FD is merely a memfd
        * Supports the same user-visible operations as the kernel buffer returned by the driver
        * Immediately closed by the daemon after responding, so that the memfd is automatically released once the user stops using them
* `SLASH_QDMA_QPAIR_IOCTL_TRANSFER`
    ``` C
    struct slash_qdma_subxfer {
        __u32 qpair_index; /* [in] Index into the fd's bound qpairs */
        __u32 direction;   /* [in] 1=H2C (write), 2=C2H (read) */
        __s32 buf_fd;      /* [in] Kernel buffer fd from BUF_CREATE */
        __u32 pad0;        /* padding */
        __u64 buf_offset;  /* [in] Byte offset within the buffer */
        __u64 dev_addr;    /* [in] Device-side (endpoint) address */
        __u64 length;      /* [in] Number of bytes to transfer */
    };

    struct slash_qdma_transfer {
        __u32 size;   /* [in/out] ABI version */
        __u32 count;  /* [in] Number of sub-transfers (1..SLASH_QDMA_FD_MAX_QPAIRS) */
        struct slash_qdma_subxfer xfers[SLASH_QDMA_FD_MAX_QPAIRS];
    };

    #define SLASH_QDMA_QPAIR_IOCTL_TRANSFER _IOWR('v', 0x56, struct slash_qdma_transfer)
    ```
    * Before sending the request, the user has to send the desired FDs as ancillary data to the daemon
    * Then, they should use the index of an FD in the list of transferred FDs in `xfers[i].buf_fd`
        * Instead of the actual FD
    * Implementation in the daemon
        * The daemon receives the request, interprets it
        * The daemon fetches the FDs
        * For H2C transfers to DDR/HBM:
            * Read the data via `read`
            * send it to the model server via a "populate" request
        * For C2H transfers from DDR/HBM:
            * Requests the data from the model server
            * receive the data
            * write the data using `write`
        * For H2C transfers to the reconfiguration aperture:
            * Read the data via `read`
            * Append the data to the reconfiguration buffer (see section "reconfiguration")
        * Responds to the IOCTL
        * Closes the transferred file descriptors
    * Implication: The FDs used in a transfer don't have to be necessarily created by `BUF_CREATE`
        * Slight emulation error accepted for now, could be fixed in the future
    * Multiple transfers, qpairs, and channels are handled with the correct functionality
        * But only sequentially
        * No performance advantage from using multiple transfers, qpairs, channels under emulation