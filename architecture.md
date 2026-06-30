# System-emulated accelerators

Idea: A user-space daemon emulates the behavior of the SLASH driver and the underlying hardware, so that user applications can be tested within the broader software setup of SLASH, but without physical hardware.

Instead of exposing control files, the emulation daemon exposes UNIX domain sockets (`AF_UNIX`/`SOCK_SEQPACKET`) with identical names, and instead of IOCTLs, all operations are messages sent over these sockets. Where an IOCTL is supposed to return a file descriptor, the daemon's response is instead a return value of zero (i.e. success), and the file descriptor is instead transferred as ancillary data as `SCM_RIGHTS`. Where an IOCTL argument struct is supposed to contain file descriptors, the sender instead transfers the corresponding file descriptors as ancillary data and references these file descriptors by index.

The difference between the driver's ABI and its emulation (IOCTLs vs socket datagrams) will be resolved in libslash: When opening a top-level device file/socket, users will also have to set a flag whether the opened file is a control file or a UNIX domain socket. This information will then handled by libslash accordingly and also forwarded to newly created constructs.

The following document describes the requirements for the system emulation daemon, as well as some necessary changes to the kernel driver, libslash, and the entire stack that depends on it.

## Nomenclature

This endeavour introduces a new concept called "system emulation", independent of the existing "FPGA emulation" and "FPGA simulation" concepts. "System emulation" is the emulation of the entire accelerator in the host system, i.e. both the FPGA, it's memory, it's connection via PCIE, and how these components are handled by the user application, VRT, and VRTD. Contrarily, "FPGA emulation" and "FPGA simulation" describe ways to model the behavior of the programmable logic, i.e. the real FPGA, in software. How the behavior of the FPGA is modelled doesn't matter much for the system emulation that we want to introduce, and system emulation can be combined both with FPGA emulation or FPGA simulation. A process that emulates the system behavior of one or more accelerators is therefore called a "system emulation daemon", and a process that models the behavior of an FPGA, either by emulation or simulation, is called a "model process."

## Accelerator state and life cycle

* The daemon manages multiple system-emulated accelerators
* Each accelerator has five components who's state needs to be tracked:

* *The main and staging VBIN files*
    * The main VBIN contains the last successfully launched model program
        * Used when (re)starting the model process and the staging VBIN file is empty or corrupted
    * The staging VBIN is written by the user to reconfigure the accelerator
        * Replaces the main VBIN file during BAR or full accelerator RESCAN
    * Remain stored even when the accelerator and its model process is torn down
    * Only cleaned during daemon startup and shutdown
        * Emulates a "cold reboot"

* *The model process*
    * Models the behavior of the FPGA
    * Executes the `vpp_emu` or `vpp_sim` executable from the main VBIN file
    * Communicates with the daemon via a ZeroMQ protocol

* *The PF0 stub*
    * In the real world: Board management, handled by the AMI driver
    * Removal and rescan however still done by the slash driver
        * Accelerator only fully torn down if PF0 has been removed
    * Thus: Existance has to be tracked by the daemon as a single flag

* *The QDMA subsystem (PF1)*
    * Manages memory transfers between the host and the card
        * I.e. the user application and the model process
    * Owns:
        * A QDMA-specific UNIX domain socket (`slash_qdma_ctl<N>`)
        * A listener, with a pool of threads managing connections
        * To be extended

* *The BAR and device info subsystem (PF2)*
    * Manages read and write access to the BAR
        * Reading and setting registers
        * Starting and fetching the state of kernels
    * Owns:
        * A BAR/control-specific UNIX domain socket (`slash_ctl<N>`)
        * A listener, with a pool of threads managing connections
        * To be extended

* An accelerator is identified by its "board BDF"
    * I.e. the full PCI BDF identifier without the function suffix
    * For example, "0000:61:00", not "0000:61:00.2"
* An accelerator is "absent" if no components are present, including the VBIN files
    * Only technically a state, since that's the state if the board BDF was never used by any accelerator during the runtime of the daemon
* An accelerator is "inactive" if only the main and staging VBIN files exist
    * This state is reached if an accelerator with the given board BDF existed
    * but was then shut down
* An accelerator is "fully active" if the both the model process and all PF subsystems are up
    * The main and staging VBIN files are a requirement to run the model process
    * Reached after a RESCAN operation
* An accelerator is "partially active" or "partial" if the model process is running, but at least one of the PF subsystems is down
    * Reached after a REMOVE operation on some, but not all PF subsystems

### Configuration

TODO

### Life cycle operations (Hotplugging)

* Managed out-of-band of the accelerator subsystems
* Via a dedicated `hotplug` socket
* Operations:
    * RESCAN
    * REMOVE (Targeted to a specific PF)
    * HOTPLUG/TOGGLE_SBR
* The launch of the system emulation daemon automatically triggers a RESCAN operation
* In system emulation, the TOGGLE_SBR operation is identical to a hotplug operation
    * Only difference: Blocks for 1s to emulate the link training
* All operations synchronized with one lock
    * Result: Only one life cycle operation in flight at a time

* RESCAN:
    * (Re)loads the daemon configuration file
    * Iterates over all accelerator configurations
    * (Re)instantiates all configurations who's board BDF does not conflict with a (partially) active accelerator
    * Active accelerators remain running
        * Even if no matching configuration entry exists
    * Also restores all removed PFs of partial accelerators
        * New/changed configurations are not applied when restoring a partial accelerator
        * Instead, the configuration from the original instantiation is used
    * Exact (re)instantiation and per-PF behavior specified below
* REMOVE:
    * Removes a specific PF
        * Exact per-PF behavior specified below
    * If the REMOVE removes the last active PF, the model process is torn down too
        * Includes PF0, which also has to be REMOVEd to stay accurate
    * The main and staging VBIN remain as they are
        * To be used when/if a RESCAN needs them
* HOTPLUG:
    * Same as REMOVE'ing the targeted PF and then running a RESCAN
        * But as one operation
* TOGGLE_SBR:
    * Same as REMOVE'ing all PFs of all devices as the same bus, RESCAN'ing, and then waiting 1s
    * Again, one operation on the lock

#### Accelerator instantiation

* Executed during daemon startup, or as part of a RESCAN operation if no (partial) accelerator with the configured BDF exists
* If no current VBIN buffer file for the BDF exists:
    * Copy the "default" VBIN, instantiate an empty staging VBIN buffer
    * The default VBIN contains a model that supports round-trip BAR/HBM/DDR read/writes, but no executable kernels
    * Built and shipped as part of the system emulation daemon, but can be changed in the configuration
* If the staging VBIN file is not empty:
    * Try to unpack and interpret the staging VBIN
    * Try to launch the model in the staging VBIN
    * If successful, replace the main VBIN with the staging VBIN and use the new model process
    * In either case, clear the staging VBIN
* If the staging VBIN file is empty or the attempt to use it failed:
    * Try to unpack and interpret the main VBIN
    * Try to launch the model in the main VBIN
    * If successful, use the newly launched model process
    * If not, the accelerator instantiation has failed
* Setup the QDMA and BAR sockets plus listeners and thread pools to handle communication

#### Removal and restoration of the BAR subsystem (PF2)

* REMOVE:
    * Stop accepting new connections, unlink socket, close existing connections, drain workers
    * TODO: Expand on how to tear down internal state
* RESCAN'ing on a partial accelerator with a running model process but no running PF2 triggers a "reconfiguration":
    * First, the daemon tries to unpack and launch the new model that has been previously written to the staging VBIN
    * If successful, the old model process is stopped, and the staging VBIN and model process replace the old ones
        * The connection of the QDMA subsystem is transparently swapped to the new model process
    * If not, the old model process remains active
    * In either case, the staging VBIN is emptied

#### Removal and restoration of the QDMA subsystem

* REMOVE:
    * Stop accepting new connections, unlink socket, close existing connections, drain workers
    * Also "forgets" any previously known qpairs
    * No need to remove buffers since they aren't owned by the QDMA subsystem anyway
* RESCAN:
    * (Re)intializes the qpairs list
    * Picks up the connection to the model process
    * Sets up worker pool and listener
    * Creates named UNIX socket

#### Removal and restoration of the PF0 stop

* PF0 is merely marked as up or down, no other effects

### Writing the staging VBIN

* In real hardware:
    * Hardware DCPs/PDIs are written in sequence to the board management via QDMA
* Difference between emulation and hardware: Accelerator model is reconfigured with the full VBIN, not just DCPs/PDIs
    * And only VBINs with either "emulation" or "simulation" target platforms, with a `vpp_emu` or `vpp_sim` executable
* Full VBIN is necessary because:
    * The daemon needs the system map
        * to tell whether the target platform is emulation or simulation
        * to reverse the register -> address mapping (if the `vpp_emu` dialect of the ZeroMQ protocol doesn't change)
    * The simulation model is shipped as a separate shared object
        * The `vpp_sim` executable is only a wrapper for it

* VRTD writes the VBIN into the staging VBIN file via QDMA
    * Writes in chunks of up to 64KiB
        * Always at device address 0x102100000
    * Each written chunk is appended to the staging VBIN file
* Once the VBIN is written, VRTD REMOVEs at least PF2 (slash_ctl) and RESCANs
    * On RESCAN, the currently staged VBIN's model is launched
    * If successful, the staged VBIN replaces the old active VBIN

### Failure handling and forced user disconnects

TODO:
* How are model process deaths/timeouts to be accounted for?
* How will forced disconnects look like to the user?
* What failure states are there?

### Accepted inaccuracies (compared to real hardware)

* Multiple issues around the non-atomicity of the reconfiguration process:
    * Simultaneous writing to the reconfiguration buffer and RESCANing leads to a race
        * More precisely: Writing some, but not all chunks and then launching a RESCAN
            * Result: PF2 restoration may evaluate incomplete data
        * Must be avoided by the user
            * The daemon can't safeguard against it apart from rejecting incomplete models
    * Reconfiguration only becomes active once the BAR subsystem has been restored
        * In hardware, the new behavior might already be available earlier

* The contents of the HBM and DDR do not persist across reconfiguration
    * I.e.: When the model processes are replaced, the new model process will not have the memory contents of the old one
    * This requires some (efficient) dumping and importing mechanism in emulation/simulation VBINs
    * To be done later

## User-facing UNIX domain socket protocol

* Follows a "request/response" pattern
    * Each operation is initiated by the user process
    * Answered by the daemon
* Each datagram first contains the `struct slash_emu_socket_header`
    ``` C
    struct slash_emu_socket_header {
        __u32 ioctl_op;         /**< The IOCTL operation to emulate */
        __u32 sequence_id;      /**< A monotonically increasing sequence number */
        __u32 return_value;     /**< The return value of the IOCTL, can be set arbitrarly for requests */
        __u32 pad;              /**< Padding */
    };
    ```
    * Both for requests from the user to the daemon, and for the response from the daemon to the user
* First, the exact same ID that would have been used to dispatch the IOCTL
    * No need to identify the device, since every device file is represented by a separate socket
* Followed by a monotonically increasing sequence number
    * Set by the user in the request
    * Mirrored back in the response
    * Used by the user to match responses to requests
* Then, the return value of the IOCTL operation
    * Only meaningful in a response
    * In a request, the return value is disregarded by the daemon
        * May thus have an arbitrary value
* Header is then followed by the corresponding IOCTL argument struct
    * Input fields must be set by the user for the request
    * Daemon sets the output fields
        * Leaving the input fields as is
    * Returns the full argument struct in the response
* On error, the `return_value` in the response is set to the expected `-errno`.
* On success, the `return_value` is >=0.
* If one or more input or output fields are file descriptors:
    * The sender transfers all file descriptors they want to use as one ancillary data message via SCM_RIGHTS
    * Uses indices to the list of transferred FDs instead of FDs in the IOCTL argument struct
* If the return value of the original IOCTL is a file descriptor:
    * Instead, the daemon also sends the FD as ancillary data via SCM_RIGHTS
        * potentially with other output FDs
    * The return value is the index of the FD in the transferred list of FDs
    * In practice/with the existing IOCTLs, the return value is always zero

* In the following, all IOCTLs from the original specification are listed
    * If not specified otherwise, either above or in the listing, the original kernel ABI specification applies.

### Implication for future IOCTLs/ABI designs

* IOCTLs may pass file descriptors, both from the user to the kernel and back
* But they may not pass pointers to user's virtual memory space
    * Reason: These can't be meaningfully transferred between processes over a UNIX domain socket

### Entry-level socket names

Just like the real driver, the daemon exposes multiple files/sockets for different accelerators and subsystems. These are:
* `slash_ctl<N>`: Provides BAR enumeration, MMIO access, and PCI device identity.
    * One socket per accelerator
* `slash_qdma_ctl<N>`: Manages DMA queue pairs for bulk data movement between host and card memory, as well as reconfiguration.
    * One socket per accelerator
* `slash_hotplug`: Provides privileged control over the lifecycle of SLASH cards
    * Single, daemon-level instance
    * Emulates remove, rescan, secondary bus reset
    * In practice: Tears down emulated accelerators, reloads the configuration file

* Base directory, uid/gid of each file, and mode of each socket are configurable or given as CLI arguments
    * Default is `/run/slash_emu`, `vrtd:vrt`, 600

## BAR access and device information (`slash_ctl<N>`)

TODO

## QDMA subsystem (`slash_qdma_ctl<N>`)

* Top-level control socket exposed as `slash_qdma_ctl<N>`, one for each accelerator.
* Resources managed by the daemon, for each accelerator
    * Qpairs
        * No inherent meaning for the daemon
        * Only a state machine per qpair to check that the user manages qpairs correctly
    * Transfer sessions
        * Created with the `QPAIR_GET_FD` IOCTL
        * Leads to the creation of a new, anonymous UNIX domain socket
        * Managed by a new worker thread
        * Executes memory transfers between the user and the model server on the user's behalf
    * A handle to the (locked) ZeroMQ socket
* Host buffers need no inherent management by the daemon
    * They are created as memfds and passed to the user
    * But the daemon can (and must) instantly forget about them
        * Memfds don't need to be managed by the daemon
        * If the daemon keeps a reference to them, they are not released once the client closes their last FD to them
    * They will be passed back to the daemon as part of a transfer IOCTL later

### Mechanisms

* First, a user has to create and start some qpairs
    * Again, these are only entries in a state tracking table
    * Possible state transitions:
        * Initial -[QPAIR_ADD]-> Stopped
        * Stopped, Started -[START]-> Started
        * Started -[GET_FD]-> Used
        * Used -[last close on FD]-> Started
        * Started, Stopped -[STOP]-> Stopped
        * Started, Stopped -[DEL]-> Removed from the list
* Then, a user can create a transfer FD
    * Internally handled as a transfer session
    * Needs to use at least one qpair
        * All qpairs have to be in the "started" state
    * Starts a new worker thread and a socket pair
        * One end is used by the worker thread, the other is passed to the user
    * Worker thread is active until the user closes the socket on their side
* Mechanism of a transfer operation
    * User:
        * Sends the source or target FDs it wants to use as ancillary data to the transfer session worker
        * Then, sends the transfer IOCTL request
            * Using indices to the list of transferred FDs instead of FDs in the request
    * Daemon:
        * Receives ancillary data and IOCTL request
        * If any of the referenced qpairs has been marked as "stopped" or removed from the qpairs list:
            * Transfer fails with `-ENODEV`
            * Correctly models the behavior of the driver, which does not invalidate a "transfer session" if an underlying qpair is stopped/removed
        * For H2C (sub-)transfers to DDR/HBM:
            * Read the data from the referenced FD via `pread`
            * send it to the model server via a "populate" request
        * For C2H (sub-)transfers from DDR/HBM:
            * Requests the data from the model server and receive it
            * write the data to the referenced FD using `pwrite`
        * For H2C (sub-)transfers to the reconfiguration aperture:
            * Read the data from the referenced FD via `pread`
            * Append the data to the staging VBIN file (see section "reconfiguration")
        * Responds to the IOCTL
        * Closes the transferred file descriptors
    * Return value on success: The total number of bytes transferred
* Transfers between the daemon and the model process have to be serialized
    * Globally, for all transfer sessions connected to the same model process
    * So that only one transfer session ever has an open request with the model process
    * Reads and writes to and from the user's target file are not part of this critical section

### Accepted inaccuracies

* The FDs used in a transfer IOCTL don't have to be necessarily created by `BUF_CREATE`
    * Slight emulation error accepted for now, could be fixed in the future, but not necessary

### Necessary functional changes to other components

* The info IOCTL now also has to return the BDF of the accelerator
    * Necessary for the emulation daemon since it does not also export `/sys/` files
    * Should also make the discovery for VRTD easier
    * Requires changes in:
        * The kernel driver (needs to report the BDF)
        * The libslash library (needs to forward this information)
        * VRTD (needs to change the discovery mechanism to use BDF returned by QDMA)
* The memory ranges of HBM/DDR/Reconfiguration region should be added to the kernel ABI header
* The reconfiguration writing protocol should be part of the kernel ABI documentation

### Messages over the socket:

The QDMA subsystem accepts datagrams on two kinds of endpoint:

* **CTL** — the top-level `slash_qdma_ctl<N>` socket, the emulated equivalent of the
  `/dev/slash_qdma_ctl<N>` control fd.
* **XFER** — a per-transfer-session anonymous socket, the emulated equivalent of a qpair I/O fd
  returned by `QPAIR_GET_FD`. Created and serviced by a dedicated worker thread.

Each opcode's `ioctl_op` field (in `struct slash_emu_socket_header`) carries the original IOCTL
command number. The table below states which endpoints accepts which operations:

| Opcode | Cmd (`'v'`) | CTL | XFER |
| --- | --- | :---: | :---: |
| `SLASH_QDMA_IOCTL_INFO` | `0x50` | ✓ | ✗ |
| `SLASH_QDMA_IOCTL_QPAIR_ADD` | `0x51` | ✓ | ✗ |
| `SLASH_QDMA_IOCTL_Q_OP` | `0x52` | ✓ | ✗ |
| `SLASH_QDMA_IOCTL_QPAIR_GET_FD` | `0x53` | ✓ | ✗ |
| `SLASH_QDMA_IOCTL_BUF_CREATE` | `0x54` | ✓ | ✓ |
| *(reserved)* | `0x55` | ✗ | ✗ |
| `SLASH_QDMA_QPAIR_IOCTL_TRANSFER` | `0x56` | ✗ | ✓ |

The following again lists all operations and the differences between the real and the emulated behavior.
If not stated otherwise, the behavior and contracts from the real kernel ABI apply to the emulation too.

* `SLASH_QDMA_IOCTL_INFO`
    * Addition: The BDF is now returned too
        * Set to the BDF of the QDMA PF, i.e. PF 1
    * Full IOCTL argument struct and opcode definition:
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
* `SLASH_QDMA_IOCTL_QPAIR_ADD`
    * As specified in the kernel ABI
* `SLASH_QDMA_IOCTL_Q_OP`
    * As specified in the kernel ABI
* `SLASH_QDMA_IOCTL_QPAIR_GET_FD`
    * Opcode and argument struct as specified in the kernel ABI
    * On success, the `return_value` is zero, and the new FD is transferred as ancillary data as SCM_RIGHTS
    * The returned FD points to an anonymous UNIX domain socket
        * Used to communicate with the worker of the newly created transfer session
        * Also emulates the IOCTLs of the original control file via datagrams
* `SLASH_QDMA_IOCTL_BUF_CREATE`
    * Opcode and argument struct as specified in the kernel ABI
    * On success, the `return_value` is zero, and the new FD is transferred as ancillary data as SCM_RIGHTS
    * The returned FD merely points to a memfd
        * Supports the same user-visible operations as the kernel buffer returned by the driver
        * Immediately closed by the daemon after responding, so that the memfd is automatically released once the user stops using them
    * Granule is the default page size as returned by `getpagesize`
    * The transfer hint is `SLASH_QDMA_TRANSFER_HINT_SINGLE_QPAIR`
* `SLASH_QDMA_QPAIR_IOCTL_TRANSFER`
    * Opcode and argument struct as specified in the kernel ABI
    * Before sending the request, the user has to send the desired FDs as ancillary data to the daemon
    * Then, they should use the index of an FD in the list of transferred FDs in `xfers[i].buf_fd`
        * Instead of the actual FD
    * Mechanism defined above

## Future work

This list contains future features of the system emulation daemon that would make it more useful, but also requires more work and possibly changes across the SLASH software stack. These features are therefore explicitly not part of this sprint, but implementation and testing agents should consider leaving space to make the implementation of these features possible in the future.

* (Virtual) network setups
    * The primary reason why system emulated accelerators are implemented like this in the first place
    * The accelerator configuration should also cover network topologies to persistently connect accelerators into (virtual) networks
* Hardened model process isolation
    * The model executable is technically untrusted user code
    * Should therefore be as isolated as possible
    * However, certain holes need to left open, for example since simulation needs some Vivado libraries
* Persisting/transferred HBM/DDR contents between model instances
    * The contents of buffers are owned and stored by the model process
    * When the model process terminates, the buffer contents are cleaned too
    * This is technically incorrect, since HBM/DDR contents should persist across PL reconfigurations
    * Thus: Some dumping and re-exporting mechanism, or daemon-owned memory is necessary
    * However: Most applications don't reuse buffers across reconfigurations
        * Thus a feature that can't be deferred
* Support for emulation models
    * Currently, emulation models expect kernels and registers to be referenced by name, not by address
    * Either a change in the emulation models or a mechanism to reverse the name -> address mapping necessary
* Non-polling BAR
    * The DMABUF-based BAR interaction model requires that the daemon continuously polls the BAR
    * Bad for performance, hard to implement clear-on-read or action-on-write registers
    * However, a different interface that implements reads and writes as file reads or writes requires a kernel module refactor.
