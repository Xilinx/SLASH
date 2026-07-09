# System-emulated accelerators

Idea: A user-space daemon emulates the behavior of the SLASH driver and the underlying hardware, so that user applications can be tested within the broader software setup of SLASH, but without physical hardware.

Instead of exposing control files, the emulation daemon exposes UNIX domain sockets (`AF_UNIX`/`SOCK_SEQPACKET`) with identical names, and instead of IOCTLs, all operations are messages sent over these sockets. Where an IOCTL is supposed to return a file descriptor, the daemon's response is instead a return value of zero (i.e. success), and the file descriptor is instead transferred as ancillary data as `SCM_RIGHTS`. Where an IOCTL argument struct is supposed to contain file descriptors, the sender instead transfers the corresponding file descriptors as ancillary data and references these file descriptors by index.

The difference between the driver's ABI and its emulation (IOCTLs vs socket datagrams) will be resolved in libslash: Whenever a user wants to execute an operation, libslash has to query the type of the underlying file and either execute an IOCTL or a datagram exchange depending on the type.

The following document describes the requirements for the system emulation daemon, as well as some necessary changes to the kernel driver, libslash, and the entire stack that depends on it.

## Nomenclature

This endeavour introduces a new concept called "system emulation", independent of the existing "FPGA emulation" and "FPGA simulation" concepts. "System emulation" is the emulation of the entire accelerator in the host system, i.e. both the FPGA, its memory, its connection via PCIE, and how these components are handled by the user application, VRT, and VRTD. Contrarily, "FPGA emulation" and "FPGA simulation" describe ways to model the behavior of the programmable logic, i.e. the real FPGA, in software. How the behavior of the FPGA is modelled doesn't matter much for the system emulation that we want to introduce, and system emulation can be combined both with FPGA emulation or FPGA simulation. A process that emulates the system behavior of one or more accelerators is therefore called a "system emulation daemon", and a process that models the behavior of an FPGA, either by emulation or simulation, is called a "model process."

## Scope

This architecture and the sprint that it describes is only supposed to implement a minimum viable product. As such the following features are not to be implemented in this sprint, but the implementation should leave the necessary space to implement them in the future:

* (Virtual) network setups
    * The primary reason why system emulated accelerators are implemented like this in the first place
    * The accelerator configuration should also cover network topologies to persistently connect accelerators into (virtual) networks
* Non-polling BAR
    * The dmabuf-based BAR interaction model requires that the daemon continuously polls the BAR
    * Bad for performance, hard to implement clear-on-read or action-on-write registers
    * However, a different interface that implements reads and writes as file reads or writes requires a kernel module refactor.
* Support for emulation models
    * Currently, emulation models provide no way to asynchronously check the state of a computation kernel
        * As such, the current polling approach is not able to cater to FPGA emulation models
    * Requires a non-trivial modification to the emulation model code
* Hardened model process isolation
    * The model executable is technically untrusted user code
    * Should therefore be as isolated as possible
    * However, certain holes need to left open, for example since simulation needs some Vivado libraries
    * To be implemented in the future
* Persisting/transferred HBM/DDR contents between model instances
    * The contents of buffers are owned and stored by the model process
    * When the model process terminates, the buffer contents are cleaned too
    * This is technically incorrect, since HBM/DDR contents should persist across PL reconfigurations
    * Thus: Some dumping and re-exporting mechanism, or daemon-owned memory is necessary
    * However: Most applications don't reuse buffers across reconfigurations
        * Thus a feature that can be deferred

## Accelerator state and life cycle

* The daemon manages multiple system-emulated accelerators
* An accelerator is identified by its "board BDF"
    * I.e. the full PCI BDF identifier without the function suffix
    * For example, "0000:61:00", not "0000:61:00.2"
* Each accelerator has six components whose state needs to be tracked:

* *The main and staging VBIN files*
    * The main VBIN contains the last successfully launched model program
        * Used when (re)starting the model process and the staging VBIN file is empty or corrupted
    * The staging VBIN is written by the user to reconfigure the accelerator
        * Replaces the main VBIN file during BAR or full accelerator RESCAN

* *The model process*
    * Models the behavior of the FPGA
    * Executes the `vpp_emu` or `vpp_sim` executable from the main VBIN file
    * The daemon communicates with it via a ZeroMQ protocol
        * The protocol is specified in the reference documentation
        * Request/Response based
        * No concurrent requests in flight possible
        * Thus: Each ZeroMQ socket is locked with a global lock
            * Each request must fetch its response before releasing the lock
            * Potentially with a queue of waiting threads

* *The model control worker threads*
    * Orchestrate the execution of the compute kernels described in the system map
    * One worker thread per compute kernel, one for the clock wizard
    * Run for the entire lifetime of the model process
        * Effect: The daemon never loses track of a kernel's state
        * Set up and torn down together with their model process
    * Drive the model process via the ZeroMQ socket
    * Subsystem interface: BARs emulated by memfds
        * One for the user region, one for the service layer, one for the clock wizard

* *The PF0 stub*
    * In the real world: Board management, handled by the AMI driver
    * Removal and rescan however still done by the slash driver
        * Accelerator only fully torn down if PF0 has been removed
    * Thus: Existence has to be tracked by the daemon as a single flag

* *The QDMA subsystem (PF1)*
    * Manages memory transfers between the host and the card
        * I.e. the user application and the model process
    * Bridges the QDMA control device and the "populate" and "fetch" ZeroMQ verbs

* *The BAR and device info subsystem (PF2)*
    * Provide access to device information and BAR memfds
    * Also, removing and rescaning this PF triggers a reconfiguration during the lifetime of the accelerator

* An accelerator is "absent" if no components are present, including the VBIN files
    * Only technically a state, since that's the state if the board BDF was never used by any accelerator during the runtime of the daemon
* An accelerator is "inactive" if only the main and staging VBIN files exist
    * This state is reached if an accelerator with the given board BDF existed
    * but was then shut down
* An accelerator is "fully active" if the both the model process, the model control workers, and all PF subsystems are up
    * The main and staging VBIN files are a requirement to run the model process
    * Reached after a RESCAN operation
* An accelerator is "partially active" or "partial" if the model process and the model control workers are running
    * but at least one of the PF subsystems is down
    * Reached after a REMOVE operation on some, but not all PF subsystems

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
* These operations only orchestrate the per-subsystem setup and teardown
    * The exact behavior lives in each subsystem's own section
    * The model process and VBIN behavior lives in "Model process and reconfiguration"

* RESCAN:
    * (Re)loads the daemon configuration file
    * Iterates over all accelerator configurations
    * (Re)instantiates all configurations whose board BDF does not conflict with a (partially) active accelerator
        * Instantiation launches the model process and then sets up every subsystem
        * See "Model process and reconfiguration" and the per-subsystem sections
    * Active accelerators remain running
        * Even if no matching configuration entry exists
    * Also restores all removed PFs of partial accelerators
        * New/changed configurations are not applied when restoring a partial accelerator
        * Instead, the configuration from the original instantiation is used
        * Restoring PF2 on a running model process triggers a reconfiguration (see "Model process and reconfiguration")
* REMOVE:
    * Removes a specific PF, per the teardown in that PF's subsystem section
    * If the REMOVE removes the last active PF, the model process and its control workers are torn down too
        * Includes PF0, which also has to be REMOVEd to stay accurate
    * The main and staging VBIN remain as they are
        * To be used when/if a RESCAN needs them
* HOTPLUG:
    * Same as REMOVE'ing the targeted PF and then running a RESCAN
        * But as one operation on the lock
* TOGGLE_SBR:
    * Same as REMOVE'ing all PFs of all devices as the same bus, RESCAN'ing, and then waiting 1s
    * Again, one operation on the lock

* Instantiation and teardown order across subsystems:
    * Instantiation: launch the model process (with its control workers), then set up the QDMA and BAR subsystems
    * Teardown: tear down each PF subsystem on REMOVE; the model process and control workers follow once the last PF is gone
    * PF0 is merely marked as up or down, with no other effects

## User-facing UNIX domain socket protocol

* Follows a "request/response" pattern
    * Each operation is initiated by the user process
    * Answered by the daemon
* Each datagram first contains the `struct slash_sysemu_socket_header`
    ``` C
    struct slash_sysemu_socket_header {
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
* To be documented in the kernel ABI reference document

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
    * Default is `/run/slash_sysemu`, `vrtd:vrt`, 600

## Model process and reconfiguration

* This section defines the exact lifecycle of the model process and the main/staging VBIN files
* The top-level life cycle operations only reference this behavior

### State

* *The main and staging VBIN files*
    * The main VBIN contains the last successfully launched model program
        * Used when (re)starting the model process and the staging VBIN file is empty or corrupted
    * The staging VBIN is written by the user to reconfigure the accelerator
        * Replaces the main VBIN file during BAR or full accelerator RESCAN
    * Remains stored when the accelerator and its model process is torn down
    * Only cleaned during daemon startup and shutdown
        * Emulates a "cold reboot"
* *The model process*
    * Models the behavior of the FPGA
    * Executes the `vpp_emu` or `vpp_sim` executable from the main VBIN file
    * The daemon communicates with it via a ZeroMQ protocol
        * The protocol is specified in the reference documentation
        * Request/Response based
        * No concurrent requests in flight possible
        * Thus: Each ZeroMQ socket is locked with a global lock
            * Each request must fetch its response before releasing the lock
            * Potentially with a queue of waiting threads

### Launching the model process and reconfiguration

* The reconfiguration can be triggered by:
    * The full instantiation of the accelerator
    * The restoration of the the BAR/device info subsystem (PF2)
* Procedure defined as follows:
    * If no current VBIN buffer file for the BDF exists:
        * Copy the "default" VBIN, instantiate an empty staging VBIN buffer
        * The default VBIN contains a model that supports round-trip BAR/HBM/DDR read/writes, but no executable kernels
        * Built and shipped as part of the system emulation daemon, but can be changed in the configuration
    * If the staging VBIN file is not empty:
        * Try to unpack and interpret the staging VBIN
        * Try to launch the model in the staging VBIN
        * If successful, replace the main VBIN with the staging VBIN and use the new model process
        * In either case, clear the staging VBIN
    * If the no model process is currently running, and the staging VBIN file is either empty or using the staging VBIN has failed:
        * Try to unpack and interpret the main VBIN
        * Try to launch the model in the main VBIN
        * If successful, use the newly launched model process
        * If not, the reconfiguration has failed
            * An error is logged, and the accelerator is torn down again
            * That's the expected behavior for an accelerator with a corrupted configuration
* If setting up a new model process was successful:
    * Tear down the existing model control workers
    * Initialize and start the new model control workers
* Effect:
    * If a model process is already running and the staging VBIN is either empty or corrupted
        * The old model remains running
        * Reconfiguration is essentially a no-op

### Teardown

* The model process and its model control workers are torn down once the last PF has been REMOVE'd
* The main and staging VBIN files remain stored for a later RESCAN

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

### Failure handling

* General rule: If the model process fails, the accelerator should "disappears" to the user
    * Consistent with a real-world accelerator or the PCIe connection failing
* If the model process terminates, the ZeroMQ socket is closed, or a ZeroMQ request times out
    * The model process is assumed dead
    * The daemon tears down the accelerator
* ZeroMQ timeouts can be relatively short (~10s)
    * Since the daemon executes no "blocking" requests on the ZeroMQ socket
* libslash has to account for this scenario:
    * Always expect that a request send or response receive may fail
    * In these cases: Return -ENODEV to emulate the missing device

### Accepted inaccuracies

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

## Model control worker subsystem

* Orchestrate the execution of the compute kernels of the model and the clock wizard
* Runs for the entire lifetime of the model process, independently of the BAR subsystem
    * The BAR subsystem may be REMOVE'd and restored while the kernels keep running
* Dependency chain: model process -[ZeroMQ socket]-> kernel worker threads -[memfds]-> BAR subsystem
    
* The current driver creates custom dmabufs for each BAR
    * Returns a FD to such a dmabuf on request
    * User process mmaps it into its virtual memory space
    * Reads and writes from the mapped memory space
        * With SYNC ioctls as brackets
    * Direct MMIO, each read and write immediately hit the device
* The emulation daemon instead provides an anonymous memfd
    * The daemon holds an FD to the memfd
    * Instead of starting and ending transactions with `DMA_BUF_IOCTL_SYNC`,
        * Use `flock` with a shared lock for reads and exclusive locks for write
    * Has to be handled correctly by libslash:
        * `struct slash_bar_file` needs a flag whether it is emulated or real
        * `slash_bar_file_sync` needs to use flock for emulated bar files
    * Requires that the file description held by the daemon and sent to the user are distinct
        * Otherwise, locks from separate processes will not collide
* Issue: The daemon is not notified when the user writes to the memfd
    * Thus: A lot of polling needed
* Long term solution: A rebuilt BAR interface where every register access is a read/write syscall
    * Comes with a small overhead, but that's an accepted cost
    * However, requires a major refactor of the driver
    * Thus deferred for after the MVP daemon is done

### User region BAR / Compute kernel control

* BAR/memfd size: 128 MiB
* The system map provides information on which kernels exist, and which registers they have
* One worker thread per compute kernel
    * Also tracks the state of the compute kernel
* Emulation is explicitly not supported in this sprint
    * Thus, only support for the address-based `vpp_sim` dialect of the ZeroMQ protocol needed.
* "Kernel idle" loop:
    * Poll the control register in the memfd in regular intervals
    * If ap_start is set (Written by the user/VRT)
        * Fetch the parameters from the BAR memfd (according to the system map)
        * Reset the control register to zero
    * Checking the control register, fetching parameters, and resetting is one CPU transaction
        * Otherwise, potential TOCTOU races
    * Send the parameter registers to the model server, then send the control register value
    * Transition to the "kernel busy" state
* "Kernel busy" loop:
    * Fetch the control register value of the model process in regular intervals
    * If ap_done is set:
        * Fetch the output/return/`ap_vld` registers from the model server
        * Write the read control register and the output/return/`ap_vld` registers to the memfd
        * So that the user/VRT sees the finished results
    * Transition to the "kernel idle" loop
* This emulation is rather crude
    * Does not support COR registers, does not preserve read/write ordering, support other control states, etc.
    * However, suffices for most compute kernels and thus the MVP daemon
    * When the read/write-based BAR interface is implemented, each read/write can go directly to the model server

#### Control register bits to implement:

| Bit | Field |
|-----|-------|
| 0 | `ap_start` |
| 1 | `ap_done` |

Other bits exist, but are not implemented in this sprint.

#### Accepted inaccuracies

* Again, the daemon gets no notification when the user accesses a register, and in which order
* Thus, anything that depends on these events is not modelled correctly
    * Clear-on-handshake (i.e. the control register) does not instantly lead to an operation
    * Clear-on-read registers are not cleared by the daemon
    * Write-only registers are not write-only, the user can change their values
        * output, return, and `ap_vld` registers are set once the kernel is complete
        * not latched/write protected

### Service Layer

* BAR/memfd size: 128 MiB
* The service layer isn't used by any software consumer right now
    * Exporting a memfd without a worker listening to it does the job

### Clock wizard

* BAR/memfd size: 512 KiB
* BAR 4 exposes two independent Xilinx Clocking-Wizard register windows
    * User region wizard at BAR 4 + `0x00000000` (user-logic clock)
    * Service region wizard at BAR 4 + `0x00010000` (service/infrastructure clock)
* The only client is the vrtd clock driver (`vrt/vrtd/src/clock.c`)
    * mmaps BAR 4 and does direct MMIO, just like the user-region BAR
    * Frequency is derived as `f_out = (prim_in_hz * M) / (D * O)`
        * `prim_in_hz` is a fixed 100 MHz constant in the driver, never read back from the BAR
    * Both windows are only ever driven through output `clk_out1` (index 0)
    * Highest register offset used is `REG26` at `0x3FC` within a window
        * The service window therefore reaches up to `0x103FC`
        * Thus BAR 4 must be at least ~64 KiB; the 512 KB above leaves ample headroom
* The FPGA model is clock-agnostic, so no real frequency synthesis is emulated
    * Only the register interface must be modelled well enough that the driver does not error out
* Two access patterns exist, both pure MMIO against the memory-backed dmabuf:
    * GET (read current rate): only reads the M/D/O divider registers and decodes a frequency
        * A zero-initialized BAR decodes cleanly to 100 MHz (the driver clamps every divider to a minimum of 1), so a GET before any SET never divides badly or returns zero
    * SET (program a rate): writes the M/D/O and tail registers, writes the reconfig trigger at offset `0x14`, then polls the lock bit
        * Polls `REG4` (offset `0x33C`) bit 0 for up to 200 ms, at 100 µs intervals
        * If the lock bit never reads 1, the driver returns `-ETIMEDOUT`, which surfaces to the application as an error
* The client does not validate the achieved frequency
    * Therefore the *only* functional requirement is that the lock bit becomes observable as 1 after a reconfig
* Complication: the lock register aliases a data register
    * `REG4` (offset `0x33C`) is both the lock-status register on read and the low half of the `clk_out1` divider leaf pair (`0x338`/`0x33C`) on write
    * On real hardware these are distinct read/write registers sharing an address
    * In a memory-backed dmabuf they are the same cell, so after a SET the lock bit reads back as `(O / 4) & 1`, which is frequently 0
* Emulation in the polling-BAR model:
    * Back both wizard windows with ordinary read/write memfd memory so all divider writes round-trip and GET reads back a plausible value
    * In the BAR 4 poll loop, pin bit 0 of `0x0033C` (user) and `0x1033C` (service) to 1 on every cycle
        * The driver's 200 ms / 100 µs lock poll tolerates the daemon re-asserting the bit a poll cycle or two later
        * Forcing this bit only perturbs the low bit of the decoded frequency, which the client ignores
    * No need to interpret the reconfig trigger (`0x14`) or model any M/D/O semantics
* Long-term, once BAR access becomes read/write syscalls (see above), the two lock/status addresses can instead return `value | 1` on read while writes land in the data shadow
    * Removes the aliasing race entirely and keeps the frequency readback exact

## BAR access and device information subsystem (`slash_ctl<N>`)

* First functionality: Providing information about the accelerator
* Second functionality: Giving access to the BARs of PF2:

### Removal and restoration

* REMOVE:
    * Stop accepting new connections
    * Unlink socket
    * Signal connection worker threads to close their connection
    * The BAR memfds remain, so the model control workers keep running
        * Effect: The daemon never loses track of the compute kernel's state
* RESCAN (restoration):
    * On a running model process, trigger a reconfiguration (see "Model process and reconfiguration")
        * If successful, might lead to a tear-down and setup of the model process and model control workers
    * (Re)create the named socket, listener thread, and worker pool

### Forced user disconnects

* If the PF is REMOVEd while a user process still has an open FD
    * Sending requests or receiving responses will fail
    * Since the connection has been close by the other party
* libslash has to account for this scenario:
    * Always expect that a request send or response receive may fail
    * In these cases: Return -ENODEV to emulate the missing device

### Implementation notes on IOCTLs

* Generally follows the same format as other subsystems:
    * Named UNIX domain socket (`AF_UNIX`/`SOCK_SEQPACKET`)
    * Request/Response format
    * Supporting the IOCTLs of the kernel ABI as requests
    * If not stated otherwise, the contracts stated in the kernel ABI specification apply

* `SLASH_CTLDEV_IOCTL_GET_BAR_INFO`:
    * BARs 0, 2, and 4 are always present and usable for MMIO, also never "in_use"
        * Other BARs are not present
    * Start address is zero
        * Would be the physical start address of the PCIe bus
        * However, nothing uses it, so synthesizing a plausible address is not necessary
    * Lengths are given for each BAR in the section "Model control worker subsystem"
* `SLASH_CTLDEV_IOCTL_GET_BAR_FD`:
    * Re-open the memfd of the requested BAR:
        * Look up the FD held by the daemon
        * Open `/proc/self/fd/<n>`
        * Necessary since `flock`s from the user and the daemon would not collide otherwise
    * Send the new, opened FD to the user as ancillary data
    * Send the response as a datagram
    * Close the new FD on the daemon's side
        * So that the file description is fully owned by the user
* `SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO`
    * BDF based on the configured board BDF, with function index 2
    * vendor_id=subsystem_vendor_id=0x10EE for AMD/Xilinx
    * device_id=0x50B6 for PF2
    * subsystem_device_id=0x000e

## QDMA subsystem (`slash_qdma_ctl<N>`)

* Top-level control socket exposed as `slash_qdma_ctl<N>`, one for each accelerator.
* Resources managed by the daemon, for each accelerator
    * Qpairs
        * Only a state machine per qpair to check that the user manages qpairs correctly
    * Transfer sessions
        * Created with the `QPAIR_GET_FD` IOCTL
        * Leads to the creation of a new, anonymous UNIX domain socket
        * Managed by a new worker thread
        * Executes memory transfers between the user and the model server on the user's behalf
    * A handle to the (locked) ZeroMQ socket
* Host buffers need no inherent management by the daemon
    * They are created as memfds and passed to the user
    * But the daemon must close them after passing them to the user
        * If the daemon keeps a reference to them, they are not released once the client closes their last FD to them
    * They will be passed back to the daemon as part of a transfer IOCTL later

### Removal and restoration

* REMOVE:
    * Stop accepting new connections, unlink socket, close existing connections, drain workers
    * Also "forgets" any previously known qpairs
    * No need to remove buffers since they aren't owned by the QDMA subsystem anyway
* RESCAN:
    * (Re)intializes the qpairs list
    * Picks up the connection to the model process
    * Sets up worker pool and listener
    * Creates named UNIX socket

### Forced user disconnects

* If the PF is REMOVEd while a user process still has an open FD
    * Sending requests or receiving responses will fail
    * Since the connection has been close unilateraly
* libslash has to account for this scenario:
    * Always expect that a request send or response receive may fail
    * In these cases: Return -ENODEV to emulate the missing device

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
* Streaming modes are not supported
    * They are also not supported by the driver, so this is not a big loss

### Messages over the socket:

The QDMA subsystem accepts datagrams on two kinds of endpoint:

* **CTL** — the top-level `slash_qdma_ctl<N>` socket, the emulated equivalent of the
  `/dev/slash_qdma_ctl<N>` control fd.
* **XFER** — a per-transfer-session anonymous socket, the emulated equivalent of a qpair I/O fd
  returned by `QPAIR_GET_FD`. Created and serviced by a dedicated worker thread.

Each opcode's `ioctl_op` field (in `struct slash_sysemu_socket_header`) carries the original IOCTL
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
        * Deliberate difference, the real driver returns `V80`
* `SLASH_QDMA_QPAIR_IOCTL_TRANSFER`
    * Opcode and argument struct as specified in the kernel ABI
    * Before sending the request, the user has to send the desired FDs as ancillary data to the daemon
    * Then, they should use the index of an FD in the list of transferred FDs in `xfers[i].buf_fd`
        * Instead of the actual FD
    * Mechanism defined above

## Necessary functional changes to other components

* The QDMA info IOCTL now also has to return the BDF of the accelerator
    * Necessary for the emulation daemon since it does not also export `/sys/` files
    * Should also make the discovery for VRTD easier
    * Requires changes in:
        * The kernel driver (needs to report the BDF)
        * The libslash library (needs to forward this information)
* Device (re)discovery now needs to be done purely via the device files in `/dev/` or `/run/slash_sysemu/`
    * On the one hand: Now possible since the QDMA info IOCTL returns the BDF
    * On the other: Now necessary since the system emulation daemon does not provide `/sys/` files
    * Needs to be reimplemented in VRTD
    * After executing a RESCAN, glob all `slash_ctl*` and `slash_qdma_ctl*` files
    * Query their BDFs
    * Pair up the right device files
    * Note: Like with the real driver, path name indices may not be stable across remove/rescan/hotplug cycles
* The memory ranges of HBM/DDR/Reconfiguration region should be added to the kernel ABI header
* The reconfiguration writing protocol should be part of the kernel ABI documentation

## Implementation plan

### Team workflow

* The team lead drives the steps below strictly in order
    * A step is only started once the previous step has been signed off
* For each step, the lead spawns one implementation agent
    * The implementation agent builds the new components for that step
    * It writes and runs unit tests as it goes, to catch errors early
* The lead then hands the work to an adversary agent
    * The adversary's explicit goal is to find bugs and bad design decisions
    * It reports concrete feedback on how to improve the implementation
* The implementation and adversary agents then iterate back and forth
    * They continue until they converge on a solution both consider sound
* The lead then hands the work to a reviewing agent
* The reviewer checks that the implementation is correct and complete
    * It also checks that the work aligns with this architecture document
    * It uses the coverage reports to judge the rigor of the unit testing
* If the reviewer does not sign off, the step returns to the implementer and adversary
* The flagged issues are resolved before the reviewer is asked again
* A step is only complete once the reviewer signs off
    * Every step keeps the normal, ASan, and UBSan builds green
    * Every step leaves the full test suite passing before hand-off
    * Later steps reuse the components and test doubles built by earlier ones
* Commit to git after every completed step

### Steps

#### Step 1: Project scaffolding and test harness

* Create the `slash_sysemu` folder and the CMake project
* Set up the normal, ASan, and UBSan build directories
* Wire up GTest and a placeholder test target
* Set up gcov/lcov coverage reporting
* Add a minimal daemon entry point that starts and cleanly shuts down
* Complete once all three builds compile and a trivial test passes

#### Step 2: Socket transport and protocol framing

* Implement an `AF_UNIX`/`SOCK_SEQPACKET` message wrapper
* Serialize and deserialize `struct slash_sysemu_socket_header`
* Pass file descriptors as `SCM_RIGHTS` ancillary data
* Map between FD indices in argument structs and transferred FDs
* Provide request/response helpers with sequence-id matching
* Test over `socketpair`, including FD passing and truncation/error cases
* Complete once the framing round-trips headers, payloads, and FDs correctly

#### Step 3: Configuration and CLI

* Parse CLI arguments with CLI11 (base directory, uid/gid, mode)
* Parse the configuration file with libinih
* Represent the list of accelerator configurations and their board BDFs
* Resolve socket paths, ownership, and permissions with the documented defaults
* Validate and report configuration errors clearly
* Test with valid, malformed, and missing configuration inputs
* Complete once a sample configuration parses into a validated in-memory model

#### Step 4: VBIN and system map parsing

* Unpack a VBIN and locate its `vpp_emu`/`vpp_sim` executable and system map
* Parse the system map with libxml2
* Classify the target platform as emulation or simulation
* Extract the kernels, their registers, and the register->address mapping
* Reject corrupted or unsupported VBINs with clear errors
* Test with valid simulation VBINs and deliberately broken ones
* Complete once a system map yields the kernel/register model needed downstream

#### Step 5: ZeroMQ model client

* Implement the `vpp_sim` address-based dialect of the ZeroMQ protocol
* Enforce a single in-flight request via a global per-socket lock
* Queue waiting threads and release the lock only after the response
* Apply a short (~10s) request timeout
* Surface transport failures and timeouts as a distinct error
* Build a reusable mock model server for testing
* Test that concurrent callers are correctly serialized against the mock
* Complete once the client drives the mock model correctly under contention

#### Step 6: Model process lifecycle and reconfiguration

* Manage the main and staging VBIN files per "Model process and reconfiguration"
* Provide the default VBIN and the empty-staging bootstrap path
* Launch the `vpp_sim` executable from the selected VBIN
* Implement the staging-then-main reconfiguration procedure and its failure handling
* Detect model process death and treat it as accelerator loss
* Tear the process down while preserving the VBIN files as specified
* Test with a fake model executable covering success and failure paths
* Complete once reconfiguration selects, launches, and replaces VBINs correctly

#### Step 7: BAR memfds

* Create sized memfds for the user region, service layer, and clock wizard
* Implement shared-lock reads and exclusive-lock writes via `flock`
* Expose helpers to read and write registers by offset
* Test concurrent readers and writers for correctness
* Complete once BAR memfds round-trip register access under locking

#### Step 8: Model control workers

* Spawn one worker per compute kernel and one for the clock wizard
* Implement the kernel idle and busy loops against the control register bits
* Fetch parameters, forward them, and write back outputs via the ZeroMQ client
* Pin the clock-wizard lock bit as described in the clock wizard section
* Track each kernel's state for the lifetime of the process
* Set up and tear down the workers together with the model process
* Test end-to-end against the mock model server and BAR memfds
* Complete once a simulated `ap_start` drives a full idle->busy->idle cycle

#### Step 9: BAR and device info subsystem (PF2)

* Create the named socket, listener thread, and worker pool
* Implement `GET_BAR_INFO`, `GET_BAR_FD`, and `GET_DEVICE_INFO` per spec
* Return BAR memfds to the user as ancillary data
* Implement REMOVE (stop, unlink, drop connections) and RESCAN restoration
* Keep the BAR memfds and model workers alive across REMOVE
* Test the IOCTLs, FD passing, and forced disconnects
* Complete once a client can enumerate and map BARs across remove/rescan

#### Step 10: QDMA subsystem (PF1)

* Create the control socket, listener thread, and worker pool
* Implement the qpair state machine and its transitions
* Implement `INFO`, `QPAIR_ADD`, `Q_OP`, `QPAIR_GET_FD`, and `BUF_CREATE`
* Create per-session transfer workers over anonymous sockets
* Implement `TRANSFER` for H2C/C2H to HBM/DDR and H2C to the reconfiguration aperture
* Serialize model transfers globally per model process
* Implement REMOVE and RESCAN, forgetting qpairs on REMOVE
* Test transfers, qpair validation, and forced disconnects against the mock model
* Complete once data round-trips host<->model and staging writes append correctly

#### Step 11: Accelerator lifecycle and hotplug

* Model the accelerator state machine (absent/inactive/active/partial)
* Create the daemon-level `slash_hotplug` socket
* Implement RESCAN, REMOVE, HOTPLUG, and TOGGLE_SBR under a single lock
* Enforce the instantiation and teardown ordering across subsystems
* Trigger a RESCAN automatically on daemon startup
* Emulate the 1s TOGGLE_SBR link-training delay
* Test each operation and the resulting subsystem states
* Complete once the full state machine is driven correctly from the hotplug socket

#### Step 12: libslash integration

* Query the file type before opening and open/connect to the file correctly
* Translate IOCTLs into datagrams and back for socket-backed devices
* Pass FDs by index via `SCM_RIGHTS` in both directions
* Return `-ENODEV` on any send or receive failure
* Forward the QDMA BDF from the new INFO field
* Test libslash against the running daemon
* Complete once a consumer works unchanged over both control files and sockets

#### Step 13: Kernel driver and VRTD changes

* Return the QDMA BDF from the driver's INFO IOCTL
* Add the HBM/DDR/reconfiguration memory ranges to the kernel ABI header
* Document the reconfiguration writing protocol in the ABI reference
* Switch VRTD device discovery to `/dev/` and `/run/slash_sysemu/` scanning
* Test discovery against both the real driver and the daemon
* Complete once VRTD discovers and drives emulated accelerators

#### Step 14: End-to-end integration

* Run a real `vpp_sim` model under the daemon
* Exercise BAR/HBM/DDR round trips, kernel execution, and the clock wizard
* Exercise reconfiguration via staging VBIN writes and RESCAN
* Exercise hotplug operations under concurrent user activity
* Run the suite under ASan and UBSan
* Complete once a representative application runs unmodified against the daemon

### Development guidelines for the system emulation daemon

* New folder in the git repo: `slash_sysemu`
    * `slash_sysemu/build/normal` to be used as the normal build directory
    * `slash_sysemu/build/asan` to be used as the build directory with ASan enabled
    * `slash_sysemu/build/ubsan` to be used as the build directory with UBSan enabled
    * `slash_sysemu/build/aubsan` to be used as the build directory with both ASan and UBSan enabled
* Programming language: C++20
    * Use as little raw pointer handling as necessary
    * If raw/external pointers have to be handled, create dedicated wrapping functionalities
* Usable dependencies:
    * libxml2
    * libzmq3
    * libjsoncpp
    * libsystemd
    * libinih
    * libcli11
    * POSIX threads
    * New dependencies need to be reviewed by the user
* Build management using CMake
* Unit testing with GTest
    * All units, components, and subsystems need rigorous testing
    * With options to build and run the test with the address and UB sanitizers
    * To be registered and executed with ctest (ctest is allowed for agent use)

## Sprint status and deferred items

*Snapshot as of 2026-07-09. This section is a running log so a
fresh session can resume the sprint without re-deriving context.*

### Progress

Steps 1–12 are DONE and committed; the system-emulation daemon and its libslash
integration are complete. **Steps 13–14 remain** (kernel driver + VRTD, then
end-to-end). The team workflow was: implementer builds and unit-tests → adversary
hunts bugs and folds every probe into the GTest suite → they iterate to
convergence → reviewer signs off → lead commits. Every committed step kept the
normal/asan/ubsan/aubsan builds green and the full ctest suite passing. Commit
hashes are in `git log`; component detail is in "Reusable building blocks" below.

#### Completed steps (ledger)

* **Step 1** — Scaffolding: CMake C++20, four build dirs `build/{normal,asan,ubsan,aubsan}` + a coverage build, GTest via ctest, lcov `coverage` target, daemon entry seam.
* **Step 2** — SEQPACKET transport + protocol framing (`transport.*`, `protocol.h`): `Result<T>`/`ErrorKind`, `UniqueFd`, SCM_RIGHTS FD passing, FD-index collect/rewrite/resolve, `send_request` seq-id matching.
* **Step 3** — Config + CLI (`config.*`): CLI11 + libinih, `BoardBdf`, `AcceleratorConfig`, socket-path helpers, defaults `/run/slash_sysemu`.
* **Step 4** — VBIN + system map (`vbin.*`, `system_map.*`): `unpack_vbin` (tar/gzip, traversal-safe, DoS-bounded, RAII `TempDir`), libxml2 system-map parse → `Kernel`/`Register`/`FunctionalArg`/`Connection`; `VbinError` taxonomy; hardware VBINs rejected.
* **Step 5** — ZeroMQ model client (`model_client.*`) + scriptable `MockModelServer` (`tests/mock_model_server.*`): vpp_sim address-keyed dialect, frame 0 byte-identical to the vrt reference client, one in-flight request under a per-socket mutex, ~10 s timeout.
* **Step 6** — Model lifecycle + reconfiguration (`vbin_store.*`, `model_process.*`, `reconfigure.*`, abstract `worker_controller.h`): per-BDF main/staging store, `posix_spawn`ed vpp_sim, death detection, the staging→main reconfiguration algorithm with rollback. `fake_model` + fixture VBINs are the shared harness.
* **Step 7** — BAR memfds (`bar_memfd.*`): `memfd_create` + mmap windows, flock brackets, `read_u32`/`write_u32`/`update_u32`, `reopen()` (distinct file description), internal per-BAR mutex; `BarSet`/`make_standard_bars()`.
* **Step 8** — Model control workers (`model_control_workers.*`): per-kernel idle→busy→idle FSM (bits 0 `ap_start` / 1 `ap_done`) + clock-wizard lock-bit pin, over `ModelClient` against injected BAR memfds. Address identity: memfd offset == model address == `base_address + register.offset`.
* **Step 9** — PF2 subsystem (`ctl_subsystem.*`, `ctl_ioctls.h`): `slash_ctl<N>` socket, `GET_BAR_INFO`/`GET_BAR_FD`/`GET_DEVICE_INFO`, SCM_RIGHTS BAR-fd passing, idempotent setup/remove over a borrowed `BarSet`. Established the UAPI-header rule (`SLASH_UAPI_INCLUDE_DIR`, no hand-mirrored structs) followed by Steps 10–12.
* **Step 10** — PF1 QDMA subsystem (`qdma_subsystem.*`, `qdma_ioctls.h`): `slash_qdma_ctl<N>` socket, CTL + per-`GET_FD` XFER endpoints, qpair state machine, H2C/C2H `TRANSFER` to model (HBM/DDR) or reconfig-aperture append. **ABI change:** `slash_qdma_info` extended with `char bdf[SLASH_PCI_BDF_LEN]` (sizeof 20→52) so userspace discovers the PF1 BDF without sysfs.
* **Step 11** — Accelerator lifecycle + hotplug (`accelerator.*`, `hotplug_subsystem.*`, `hotplug_ioctls.h`, `daemon.*`): per-BDF state machine (Absent/Inactive/Active/Partial), instantiate/teardown ordering, quiesce-first PF2 restore, model-death generation guard, `slash_hotplug` socket serialising RESCAN/REMOVE/HOTPLUG/TOGGLE_SBR under one `lifecycle_mu_`.
* **Post-Step-11 hardening/packaging** — sd-event/systemd refactor of `run_daemon()` (systemd owns runtime dir, socket identity/mode, journal logging; health-gated watchdog); rename `slash_emu`→`slash_sysemu` (exe `slash_sysemud`); from-source default VBIN (`src/model/` → `default.vbin`, installed to `/usr/lib/slash-sysemu/default.vbin`); deb/rpm packaging (`slash-sysemu.service`, disabled-by-default preset, sysusers, `slash-sim-emu` metapackage pulls in both `slash-sysemu` and `vrtd`). FPGA-emulation tokens (`vpp_emu`, `Emulation` platform) deliberately left untouched.
* **Step 12** — libslash socket integration: shared UAPI protocol header `driver/libslash/include/slash/uapi/slash_sysemu.h`; C90 client transport `driver/libslash/src/sock_transport.{h,c}`; socket paths in `ctldev.c`/`qdma.c`/`hotplug.c` selected by `S_ISSOCK` at open; BAR sync over sockets via `flock(2)`; any transport failure → `errno=ENODEV`. Consumer code is transport-agnostic across char devices and sockets. In-process `SysemuTestServer` + opt-in real-daemon E2E (`SLASH_E2E_*`).

#### Cross-cutting invariants (read before debugging)

Non-obvious, load-bearing contracts. Breaking one tends to produce UAFs,
deadlocks, or spurious `-ENODEV`:

1. **Model death.** `ModelProcess`'s `DeathCallback` runs on the monitor (reaper) thread and may ONLY post — never synchronously `teardown()`/destroy the `ModelProcess` or its `Accelerator` from inside it (UAF of the in-flight `std::function`). `HotplugSubsystem` posts `on_model_died(gen)` to a dedicated lifecycle thread.
2. **Generation guard.** The launch-generation counter is **owned by `HotplugSubsystem`** (daemon lifetime) and injected into every `Accelerator` — including RESCAN replacements, which replace the whole object. `on_model_died(gen)` no-ops unless `gen == generation()`, so a stale death task cannot tear down a re-adopted model. (A per-object counter caused a generation collision — see `project_death_generation_guard`.)
3. **Model serialization.** `ModelClient` holds one `std::mutex` across the whole send→recv (one request in flight; ZMQ REQ). QDMA keeps `pread`/`pwrite` OUTSIDE that lock so file I/O overlaps while model requests serialise.
4. **flock BAR sync.** flock only excludes across DISTINCT open file descriptions. The daemon `reopen()`s each BAR memfd (`/proc/self/fd/<n>`) before handing it to the client via SCM_RIGHTS; `BarMemfd`'s internal per-BAR mutex spans the whole flock bracket so kernel workers can share the user-region BAR. Same rule client-side in libslash.
5. **Borrow contract.** PF1/PF2 borrow `BarSet`/`ModelClient`/`VbinStore` by reference; `remove()` never touches them, so model workers keep polling across REMOVE. A PF2 restore quiesces PF1 first (before the old `ModelClient` is destroyed) — this keeps `qdma_subsystem.*` pristine (no `rebind_model`).
6. **`remove()` fd hygiene.** The `done`-flag check + `::shutdown(fd)` run **under** the connection-map mutex, and a worker sets `done=true` under that same lock before its `UniqueFd` closes — so `remove()` never `shutdown()`s a stale/reused fd number (all subsystems share one process fd table; this bit the Step-12 E2E).
7. **Reconfig aperture.** H2C to `dev_addr == 0x102100000` appends to the staging VBIN (not sent to the model, `dev_addr` not advanced). The constant currently lives in `qdma_ioctls.h` / `slash_sysemu.h`; **Step 13 moves it into the kernel UAPI header.**

#### Remaining work

* **Step 13 — kernel driver + VRTD changes — NOT STARTED.**
    * Driver fills the QDMA `INFO` BDF field libslash already forwards (Step 10 extended `slash_qdma_info` 20→52 bytes; the driver must populate `bdf`).
    * Move `kReconfigApertureAddr` (0x102100000) + the HBM/DDR/reconfig memory ranges from `qdma_ioctls.h`/`slash_sysemu.h` into the kernel UAPI header.
    * Document the reconfiguration writing protocol in the ABI reference.
    * Switch VRTD discovery to globbing `/dev/` + `/run/slash_sysemu/` (`slash_ctl*`/`slash_qdma_ctl*`), querying + pairing BDFs (path indices are not stable across remove/rescan/hotplug).
* **Step 14 — end-to-end integration — IN PROGRESS.** Real `vpp_sim` under the daemon; BAR/HBM/DDR round trips, kernel execution, clock wizard, reconfiguration via staging writes + RESCAN, hotplug under concurrent activity; run under ASan/UBSan.
    * First harness: `00_axilite_raw` (see "Raw example E2E app" under building blocks) exercises the whole flow through libslash alone. As of 2026-07-09 it reaches server-side reconfiguration cleanly (discovery, VBIN staging transfer, and PF2 REMOVE all succeed). RESCAN then fails because the staged `vpp_sim` cannot load Vivado's XSI runtime (`libxv_simulator_kernel.so`); the daemon safely reverts to the default VBIN. Fixed by adding the Vivado runtime dir to the daemon env file (`LD_LIBRARY_PATH` → `.../Vivado/lib/lnx64.o`, `slash_sysemu/packaging/conf/slash_sysemu.env`). Verifying a full green sim run under the daemon (and extending raw apps to examples 01–07) is next-session work.

### Reusable building blocks now available

Component index (files under `slash_sysemu/src/` unless noted; behaviour detail
in the ledger + invariants above):

* **Transport** — `Result<T>`/`ErrorKind` (Transport vs Protocol; Transport → `-ENODEV`), `UniqueFd`, `send_message`/`recv_message`/`send_request`, FD-index helpers (`transport.*`, `protocol.h`).
* **Config** — `DaemonConfig`, `BoardBdf`, socket-path helpers (`config.*`).
* **VBIN/system map** — `unpack_vbin`, `SystemMap`/`Kernel`/`Register`/`FunctionalArg`/`Connection`, `VbinError`, RAII `TempDir` (`vbin.*`, `system_map.*`). `base_address + offset` = absolute address; `register_at()` reverses it.
* **Model client** — `ModelClient` (vpp_sim ZeroMQ dialect, serialized, timed-out) + scriptable `MockModelServer` test double (`model_client.*`, `tests/mock_model_server.*`).
* **Model lifecycle** — `VbinStore`, `ModelProcess`, `WorkerController` (abstract), `ModelInstance` (`vbin_store.*`, `model_process.*`, `worker_controller.h`, `reconfigure.*`). `fake_model` + fixture VBINs = shared harness. Death teardown MUST be posted off the monitor thread (invariant 1).
* **BARs** — `BarMemfd`, `BarSet`/`make_standard_bars()` (`bar_memfd.*`): UserRegion 128 MiB (idx 0), ServiceLayer 128 MiB (idx 2), ClockWizard 512 KiB (idx 4); flock brackets, `update_u32`, internal per-BAR mutex, `reopen()`.
* **Workers** — `ModelControlWorkers` (concrete `WorkerController`, `model_control_workers.*`): per-kernel FSM + clock-wizard lock-bit pin.
* **PF2** — `CtlSubsystem` + `ctl_ioctls.h` (`ctl_subsystem.*`): `slash_ctl<N>`, listener + per-connection workers, idempotent setup/remove over a borrowed `BarSet`.
* **PF1** — `QdmaSubsystem` + `qdma_ioctls.h` (`qdma_subsystem.*`): `slash_qdma_ctl<N>`, CTL + XFER endpoints, qpair FSM, `TRANSFER` routing over a borrowed model + vbin store.
* **Accelerator** — `Accelerator` (`accelerator.*`): per-BDF state machine, instantiate/teardown ordering, quiesce-first PF2 restore, generation guard.
* **Hotplug + daemon** — `HotplugSubsystem` + `hotplug_ioctls.h` (`hotplug_subsystem.*`): `slash_hotplug` socket, one `lifecycle_mu_`, owns the generation counter + off-monitor lifecycle thread. `run_daemon()` on `sd-event` (`daemon.*`): hotplug setup → startup RESCAN → READY → loop (signalfd + eventfd shutdown + health-gated watchdog) → REMOVE. systemd owns runtime dir/identity/mode/logging.
* **Default model** — `slash_sysemu_model` (`src/model/`) + CMake-built `default.vbin` (round-trip mem/regs, no kernels); independently pinned to `ModelClient` alongside `MockModelServer` — keep both green.
* **libslash socket transport** — `sock_transport.{h,c}` + ctldev/qdma/hotplug socket paths (`driver/libslash/`). `S_ISSOCK` at open selects the path; failure → `errno=ENODEV`; BAR sync = `flock(2)`. Transport-agnostic across `/dev/slash_*` and `/run/slash_sysemu/slash_*`.
* **Test server** — `SysemuTestServer` (`driver/libslash/tests/sysemu_test_server.*`): in-process SEQPACKET server for all three subsystems + fault injection (PeerClose/TruncatedReply/WrongSeq/WrongOp/DaemonError). Opt-in `libslash_e2e_test` drives a real daemon via `SLASH_E2E_*`.
* **Raw example E2E app** — `examples/00_axilite/00_axilite_raw.cpp` (target `00_axilite_raw`): a libslash-only counterpart to `00_axilite` that drives the daemon with **no VRT/VRTD** in between — the thinnest possible host-to-daemon integration test. Args: `<control-base-dir> <board-BDF> <vbin>`. Flow: enumerate `slash_ctl*`/`slash_qdma_ctl*` and match by BDF (indices are not stable) → QDMA H2C the whole VBIN to the reconfig aperture `0x102100000` → hotplug PF2 REMOVE + RESCAN → re-discover ctl/qdma → QDMA H2C input to HBM `0x4000000000` → launch kernels over BAR0 AXI-Lite (accumulate_0 @ BAR0 `0x0`, increment_0 @ `0x10000`; ap_ctrl_hs CTRL@`0x00` = ap_start bit0 / ap_done bit1, size@`0x10`, increment buffer ptr @`0x18`/`0x1c`, accumulate out_r@`0x18` + out_r_ctrl@`0x1c`) → poll ap_done → check result. Register/address constants are hard-coded against the design's `system_map.xml` (no VBIN untar / XML parse). **Build with `-DSLASH_USE_REPO=ON`** so it links the socket-capable repo libslash; a stale installed libslash without the Step-12 socket transport makes every `*_open()` on a socket fail with `ENXIO`.

### Deferred / outstanding items

* **MVP scope exclusions** (see the "Scope" section) remain deferred by design:
  virtual network setups, non-polling BAR, FPGA-emulation-model support,
  hardened model-process isolation, and persisting HBM/DDR across
  reconfigurations.
* **QDMA ABBA lock-ordering hazard (pre-existing, Step 10) — DEFERRED.**
  `QdmaSubsystem` has two mutexes, `workers_mtx_` and `qpairs_mtx_`, acquired in
  *inconsistent order* on two paths: `handle_qpair_get_fd`'s stop-racing path takes
  `workers_mtx_` → `qpairs_mtx_` (`qdma_subsystem.cpp` ~587-597), while
  `session_loop`'s exit takes `qpairs_mtx_` → `workers_mtx_` (~335-354). That is a
  classic ABBA deadlock potential if both fire simultaneously. The window is
  extremely narrow (the `qpairs_mtx_` holds are very brief) and neither the full
  aubsan suite nor TSan-sensitive paths trip it today, so it has never been observed.
  It is **pre-existing** — present since Step 10 and *not* introduced or worsened by
  the Step-12 fd-reuse fix (that fix only touched `remove()` and the done-setting
  paths, none of which hold both locks). Surfaced by the reviewer during the
  Step-12 fd-reuse-fix sign-off; the lead + user decided on 2026-07-08 to keep that
  change scoped and defer this. **Suggested remedy:** in `handle_qpair_get_fd`,
  release `workers_mtx_` before acquiring `qpairs_mtx_` and re-check the atomic stop
  flag after acquiring `qpairs_mtx_` (safe because the stop check is atomic), so both
  paths observe a single `qpairs_mtx_`-then-`workers_mtx_` (or non-nested) order.
* `main.cpp` (2 lines) is not unit-tested (daemon entry point); acceptable.
* Residual cosmetic `geninfo mismatch` warnings on test files under GCC 13 +
  lcov 2.0; downgraded via `--ignore-errors`, not a production concern.
* **lcov 2.0 `--list` is unreliable on this GCC 13 toolchain** (reports ~8-9%
  line / 0% function and an impossible 107% for headers). Trust `gcov -b` on the
  `.gcda` directly for true per-file figures (surfaced during the Step 4 review).
* **Step 4 optional hardening (non-blocking):** the two VBIN DoS-guard branches
  (`kMaxMemberBytes` tar-member cap, `kMaxArchiveBytes` gzip-bomb expansion cap)
  are correct but not yet exercised by a test; a crafted oversized tar `size`
  field / a highly compressible gzip payload would cover them in a later pass.
* **Step 6 nits (non-blocking):** (a) `vbin_store.cpp` bootstrap branch
  "main exists + staging *missing* → create empty staging" is a behavioral (not
  fault-injection) branch that lacks a direct test — a one-line test would close
  it. (b) `fake_model.cpp`'s `--never-bind` mode is currently unused dead
  test-support (ZMQ ipc connect succeeds lazily, so the `hang_model` probe
  timeout already covers the surfaced failure); prune or wire it to the
  connect-failure branch later.
* **Step 7 nit (non-blocking):** the `const` overloads `BarSet::by_kind() const`
  / `by_index() const` are uncovered (and `by_kind(ServiceLayer)` on the
  non-const path); a couple of lines in `BarSetTest.LookupByKindAndIndex` would
  close them. Relevant if Step 9 accesses a `const BarSet`.
* **Sanitizer build-dir hazard (found resuming Step 7):** a `build/aubsan` dir
  was once configured with EMPTY sanitizer flags — a silent non-sanitized
  "green". Always configure the sanitizer dirs via the README options
  (`-DENABLE_ASAN=ON` / `-DENABLE_UBSAN=ON`; aubsan = both) and, before trusting
  a sanitizer run, PROVE instrumentation is active by confirming `-fsanitize=`
  appears in the dir's `flags.make` (asan=address, ubsan=undefined,
  aubsan=address+undefined). Do not rely on the build-dir name.

### Working agreements / environment notes (for the next session)

* **Team:** `slash-emu-sprint` with teammates `implementer`, `adversary`,
  `reviewer`. The team and its task list are recreated per session if they were
  torn down; the 14 steps map 1:1 to tasks.
* **Standing rules:** four build dirs incl. `aubsan`; tests registered and run
  via ctest; the adversary must add every probe/check to the GTest suite (no
  throwaway programs); always verify from a clean `rm -rf build/<name>` build
  before reporting green (a stale-binary false-green slipped through once in
  Step 3); coverage via the lcov `coverage` target only; the lead commits after
  each reviewer sign-off. **Always build/test with a fixed `-j16` (e.g.
  `cmake --build build/<name> -j16`, `ctest -j16`), never `-j"$(nproc)"`.**
* **Tooling:** `lcov` 2.0 + `genhtml` + `gcov` are installed. `gcovr` is present
  at `~/miniforge3/bin/gcovr` but is **not** sanctioned tooling and was not
  installed intentionally for this project — do not use it; consider removing it.
  Do not self-install packages; ask the user.
* Deps confirmed installed: libxml2 2.9.14 (`/usr/include/libxml2`), libzmq
  4.3.5, jsoncpp 1.9.5 (`/usr/include/jsoncpp`), libsystemd 255, inih/INIReader
  55, CLI11 (`/usr/include/CLI/CLI.hpp`), pthreads.
* **Raw-example / live-daemon testing gotchas (found 2026-07-09):** (a) the
  system-installed libslash on this host predates the Step-12 socket transport,
  so examples must be built `-DSLASH_USE_REPO=ON` (or the repo libslash
  reinstalled) or `*_open()` on a daemon socket returns `ENXIO`; (b) the daemon
  runs as user `slash-sysemu` under systemd with an empty `Environment=`, so
  launching a **sim** VBIN's `vpp_sim` needs Vivado's xsim runtime on
  `LD_LIBRARY_PATH` — now wired via `slash_sysemu.env` (and the local install);
  an **emu** VBIN would need the analogous Vitis-HLS runtime.
