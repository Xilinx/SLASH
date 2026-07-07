# System-emulated accelerators

Idea: A user-space daemon emulates the behavior of the SLASH driver and the underlying hardware, so that user applications can be tested within the broader software setup of SLASH, but without physical hardware.

Instead of exposing control files, the emulation daemon exposes UNIX domain sockets (`AF_UNIX`/`SOCK_SEQPACKET`) with identical names, and instead of IOCTLs, all operations are messages sent over these sockets. Where an IOCTL is supposed to return a file descriptor, the daemon's response is instead a return value of zero (i.e. success), and the file descriptor is instead transferred as ancillary data as `SCM_RIGHTS`. Where an IOCTL argument struct is supposed to contain file descriptors, the sender instead transfers the corresponding file descriptors as ancillary data and references these file descriptors by index.

The difference between the driver's ABI and its emulation (IOCTLs vs socket datagrams) will be resolved in libslash: When opening a top-level device file/socket, users will also have to set a flag whether the opened file is a control file or a UNIX domain socket. This information will then handled by libslash accordingly and also forwarded to newly created constructs.

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
    * Default is `/run/slash_emu`, `vrtd:vrt`, 600

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
        * `slash_bar_file_sync` needs to use fsync for emulated bar files
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
* Device (re)discovery now needs to be done purely via the device files in `/dev/` or `/run/slash_emu/`
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

* Create the `slash_emu` folder and the CMake project
* Set up the normal, ASan, and UBSan build directories
* Wire up GTest and a placeholder test target
* Set up gcov/lcov coverage reporting
* Add a minimal daemon entry point that starts and cleanly shuts down
* Complete once all three builds compile and a trivial test passes

#### Step 2: Socket transport and protocol framing

* Implement an `AF_UNIX`/`SOCK_SEQPACKET` message wrapper
* Serialize and deserialize `struct slash_emu_socket_header`
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

* Add the control-file-vs-socket flag at device open and propagate it
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
* Switch VRTD device discovery to `/dev/` and `/run/slash_emu/` scanning
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

* New folder in the git repo: `slash_emu`
    * `slash_emu/build/normal` to be used as the normal build directory
    * `slash_emu/build/asan` to be used as the build directory with ASan enabled
    * `slash_emu/build/ubsan` to be used as the build directory with UBSan enabled
    * `slash_emu/build/aubsan` to be used as the build directory with both ASan and UBSan enabled
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

*Snapshot as of 2026-07-06. This section is a running log so a
fresh session can resume the sprint without re-deriving context.*

### Progress

Steps are executed strictly in order via the team workflow (implementer builds
and unit-tests → adversary hunts bugs and folds every probe into the suite →
they iterate to convergence → reviewer signs off → the lead commits). Each
committed step keeps the normal/asan/ubsan/aubsan builds green and the full
ctest suite passing.

* **Step 1 — Project scaffolding and test harness — DONE** (commit `fcc3121b`)
    * CMake C++20 project under `slash_emu/`; four build dirs
      `build/{normal,asan,ubsan,aubsan}` plus a coverage build (coverage is
      mutually exclusive with the sanitizers).
    * GTest via FetchContent v1.17.0, `gtest_discover_tests`, run through ctest.
    * gcov/lcov `coverage` custom target (extracts `src/*`; HTML under the
      build dir).
    * Minimal daemon entry point: `run_daemon()` with a `std::atomic<bool>`
      shutdown flag and an injectable `SignalInstaller` seam so the
      sigaction-failure path is unit-tested. `-Wall -Wextra -Werror` on all
      targets.
* **Step 2 — Socket transport and protocol framing — DONE** (commit `cb74d5d9`)
    * `src/protocol.h`: `struct slash_emu_socket_header` (16 bytes,
      static_assert-checked).
    * `src/transport.{h,cpp}`: `Result<T>` / `Result<void>` with an `ErrorKind`
      that distinguishes **Transport** (peer-closed / OS failures; later mapped
      to `-ENODEV`) from **Protocol** (framing) errors; `UniqueFd` RAII owner;
      `send_message`/`recv_message` over `SOCK_SEQPACKET` with SCM_RIGHTS FD
      passing, `MSG_CMSG_CLOEXEC`, and `MSG_TRUNC`/`MSG_CTRUNC` detection;
      FD-index collect/rewrite/resolve helpers (`kMaxFdsPerMessage` cap, atomic
      rewrite); `send_request` with sequence-id + ioctl_op matching.
    * 46 tests; 100% src line/function coverage.
* **Step 3 — Configuration and CLI — DONE** (commit `5fe0b4a8`)
    * `src/config.{h,cpp}`: `BoardBdf` value type (rejects function suffix and
      trailing garbage); `AcceleratorConfig` (typed `BoardBdf` + optional
      `vbin_path` reserved for Step 6); `DaemonConfig`; CLI11 `parse_cli`;
      libinih `parse_config_file`; `socket_path_ctl/_qdma_ctl/_hotplug`
      helpers. Defaults `/run/slash_emu`, `vrtd:vrt` (fallback to current
      uid/gid with a warning when absent), mode `0600`.
    * 115 tests; 98.9% src line / 100% function coverage (the 4 uncovered
      lines are the vrtd/vrt fallback, reachable only where those accounts are
      absent).

* **Step 4 — VBIN and system map parsing — DONE**
    * `src/vbin.{h,cpp}`: `unpack_vbin` reads the container (raw tar or
      gzip-detected via magic), safely extracts into an RAII `TempDir`
      (rejects absolute paths and `..` traversal, verifies tar checksums,
      detects truncation, bounds member/archive size against decompression
      bombs, handles GNU longnames, ustar prefix/name joining, and skips PAX
      `x`/`g` headers), then locates `system_map.xml` and the
      platform-appropriate `vpp_emu`/`vpp_sim`. Multi-member gzip streams are
      fully consumed while trailing non-gzip bytes are tolerated. Hardware
      VBINs are rejected with a clear error. Dedicated `VbinError` taxonomy
      (Io/Archive/Contents/Parse), kept separate from transport's `ErrorKind`.
    * `src/system_map.{h,cpp}`: libxml2 (RAII `XmlDocPtr`/`XmlCharPtr`) parse
      of `<SystemMap>` → `Platform`, `ClockFrequency`, `Kernel`
      (name/base_address/range + `Register` offset/access/bit_width, sorted
      `FunctionalArg`s, port→memory `Connection`s), and `QdmaConnection`s.
      `Kernel.base_address + Register.offset` gives the absolute address and
      `register_at()` reverses it — the register→address mapping downstream
      needs. Numbers parse as decimal unless an explicit `0x`/`0X` prefix
      selects hex (no base-0 octal trap); attribute whitespace is trimmed;
      leading `+`/`-` rejected; 32-bit width overflow is an error, not a silent
      truncation. Duplicate kernel names and duplicate register offsets within a
      kernel are rejected. Offset-0 control-register absence and zero bit_width
      are deliberately accepted (documented Step 8 concerns).
    * Tests: 33 `VbinTest` + 40 `SystemMapTest` (both real sample VBINs, raw
      tar, gzip incl. multi-member/corrupt/truncated edges, traversal/absolute
      rejection, GNU longname, Hardware rejection, every XML parse-error branch,
      octal-trap/overflow/duplicate/XXE/billion-laughs probes). Coverage:
      system_map.cpp 98.9% lines / 99.3% branches, vbin.cpp 89.0% lines /
      82.6% branches (remainder are justified I/O fault-injection and DoS-guard
      branches). Reviewer signed off; all four builds green.

* **Step 5 — ZeroMQ model client — DONE**
    * `src/model_client.{h,cpp}`: `ModelClient` implements the `vpp_sim`
      address-keyed ZeroMQ dialect (`start`/`populate`/`fetch buffer`/`fetch
      scalar`/`reg`/`exit`) over `ZMQ_REQ`→`ZMQ_REP` on an `ipc://` (or
      `tcp://`) endpoint, using the libzmq C API wrapped in RAII (LINGER=0, no
      ctx_term hang) and jsoncpp for frames. Frame 0 is encoded with the default
      `Json::StreamWriterBuilder` to be **byte-for-byte identical** to the vrt
      reference client (`vrt/src/utils/zmq_server.cpp`), so a real `vpp_sim`
      interoperates; `populate` sends a second raw frame with the invariant
      `size == payload length` (the sim reads exactly `size` bytes). A single
      `std::mutex` is held across the whole send→recv cycle: one request in
      flight, callers serialized (ZMQ sockets are not thread-safe and REQ
      forbids overlap). Configurable ~10s timeout (`ZMQ_RCVTIMEO`/`SNDTIMEO`,
      injectable for tests). Reuses `transport.h`'s `Result`/`ErrorKind`:
      timeout / dead-or-closed socket / moved-from-or-unconnected client →
      `Transport` (→ -ENODEV, "model assumed dead"); malformed-but-delivered
      reply (not "OK"/"ERR"/JSON, wrong type, byte >255, buffer length
      mismatch) → `Protocol`. Never throws across the API.
    * `tests/mock_model_server.{h,cpp}`: reusable scriptable `ZMQ_REP` mock
      `vpp_sim` (in-memory addr→byte + scalar maps, request recording, injectable
      faults: WrongReply/ErrReply/MalformedJson/OversizedByte/ShortBuffer/Delay/
      Silence/ExtraFrame/JsonStringReply/Close). Built as a static helper lib —
      **Steps 8 and 10 reuse it**. Serialization is proven by a functional
      per-thread keyed-readback concurrency test (removing the client mutex makes
      it fail with ~1500 crossed replies), NOT by the mock's `max_in_flight`
      (its REP loop is single-threaded, so that is only a sanity check).
    * 40 `ModelClient*` tests. Coverage: model_client.cpp 92.9% lines / 95.2%
      branches (remainder are justified libzmq-API-failure defensive branches).
      Reviewer signed off; normal/asan/ubsan/aubsan all green (253/253).
    * Env note: TSan is unusable on this WSL2 host (runtime abort); the
      mutex-removal experiment gives equivalent (functional) race assurance.

* **Step 6 — Model process lifecycle and reconfiguration — DONE**
    * `src/vbin_store.{h,cpp}`: per-BDF on-disk store at `<base>/<bdf>/` with
      `main.vbin` + `staging.vbin`. `bootstrap` (copy default→main + empty
      staging if absent, without clobbering an existing main or a pending
      staging), `append_staging`/`read_staging`/`clear_staging` (truncate, keep
      file), `replace_main_with_staging` (atomic rename + recreate empty
      staging), and `cold_reboot_cleanup` (removes the dir — Step 11 only; the
      files PERSIST across per-accelerator teardown).
    * `src/model_process.{h,cpp}`: RAII owner of one launched `vpp_sim`. Unpacks
      the selected VBIN (its own `TempDir` copy), derives the `ipc://<dir>/
      zmq.socket` endpoint (with an up-front `AF_UNIX sun_path` length guard →
      clear Io error), launches via `posix_spawn` (`addchdir_np` to the
      extraction dir, `SETSIGDEF`/`SETSIGMASK`), and treats launch success as
      spawn + `ModelClient` connect + the one-time global `start` probe. A
      single monitor thread is the sole reaper (`waitpid`); teardown escalates
      `exit`→SIGTERM→SIGKILL with bounded waits. **Death detection** (process
      exit / socket close / probe timeout) fires a death callback exactly once,
      suppressed on intentional teardown. **Step 6→11 contract (documented on
      `DeathCallback`):** `on_death` runs on the monitor thread; a caller must
      NOT synchronously `teardown()` OR destroy the `ModelProcess` from within
      it (destroy → UAF of the in-flight `std::function`) — Step 11 must post
      teardown/destroy to another thread. A self-join guard (detach-not-join
      when `teardown()` runs on the monitor thread) turns the would-be deadlock
      into safe behavior as a backstop.
    * `src/worker_controller.h`: abstract `WorkerController` (paired
      `start(ModelClient&, const SystemMap&)` / `stop()`, nullable
      `shared_ptr`); Step 8 implements the real one. `start` must NOT re-send
      the global sim-clock `start` — the `ModelProcess` owns that one-time call.
    * `src/reconfigure.{h,cpp}`: `ModelInstance::reconfigure()` implements the
      "Launching the model process and reconfiguration" algorithm branch for
      branch (no-main→bootstrap; staging-nonempty→launch→adopt→replace-main→
      clear-staging; staging-fail→clear-staging→(fall back to main if idle |
      keep old if running); main-fail→Failed; running+empty/corrupt-staging→
      no-op). Adoption does worker stop→swap→start with rollback (full teardown
      of the new process) on worker-start failure. On a rename failure after a
      successful adoption it keeps the running process (NewProcess) and
      best-effort clears staging (only the on-disk main is left stale).
    * Additive config: `DaemonConfig::default_vbin_path` (+ `--default-vbin`),
      per-device `vbin_path` parsed from the config file, and
      `resolve_default_vbin(accel)` precedence (accelerator `vbin_path` >
      daemon default). Step 3 semantics unchanged; `config_test` green. Also a
      `VbinResult<void>` specialisation in `system_map.h`.
    * Tests: `fake_model` executable (reuses `MockModelServer`; serve/exit/hang
      variants + failure flags) and tar-packed fixture VBINs (default/staging/
      unlaunchable/hang/corrupt). ~55 Step-6 tests across `vbin_store_test`,
      `model_process_test`, `reconfigure_test`, and `DefaultVbinTest`. Coverage:
      reconfigure.cpp 96.7% lines, config additions 97.8%, model_process.cpp
      84.4%, vbin_store.cpp 83.8% (remainders are justified OS/IO
      fault-injection + spawn-failure branches). Reviewer signed off;
      normal/asan/ubsan/aubsan all green (308/308) with `-j16`; no zombies /
      leftover ipc sockets / temp dirs.

* **Step 7 — BAR memfds — DONE**
    * `src/bar_memfd.{h,cpp}`: `BarMemfd` — an anonymous `memfd_create(MFD_CLOEXEC)`
      window `ftruncate`d to size and persistently `mmap(MAP_SHARED)`ed
      (RAII munmap), the daemon-side register store. Access via `flock` brackets
      (LOCK_SH read / LOCK_EX write / LOCK_UN, replacing `DMA_BUF_IOCTL_SYNC`),
      with `read_u32`/`write_u32` (explicit little-endian, no aliasing UB),
      byte-range `read`/`write`, and an `update_u32` read-modify-write helper
      (the atomic control-register handshake Step 8's idle loop needs) — all
      overflow-safe bounds-checked (out-of-range → Protocol, OS failures →
      Transport, never throws). `reopen()` = `open("/proc/self/fd/<n>",
      O_RDWR|O_CLOEXEC)` returns a DISTINCT open file description (the fd Step 9
      hands the user via SCM_RIGHTS) so daemon-vs-user flocks actually collide.
      flock acquire AND LOCK_UN release retry on EINTR. A per-object internal
      `std::mutex` (behind `unique_ptr` to stay movable) spans the ENTIRE flock
      bracket, so multiple Step 8 kernel workers may safely share the one
      user-region BAR (without it, one thread's LOCK_UN would drop the whole-file
      flock another holds, breaking exclusion against the user).
    * `BarSet` + `make_standard_bars()`: the standard trio — UserRegion 128 MiB
      (idx 0), ServiceLayer 128 MiB (idx 2), ClockWizard 512 KiB (idx 4);
      `by_kind`/`by_index` lookups (nullptr for absent 1/3/5) feed Step 9's
      GET_BAR_INFO/GET_BAR_FD.
    * ~38 `bar_memfd` tests: size/round-trip/bounds/overflow, u32 endianness,
      cross-description flock collision (reopen) vs dup non-collision, shared-SH
      compatibility, exclusive serialization, same-object concurrent RMW exact
      count + no-tear, move/self-move safety under concurrency, EINTR retry, fd
      hygiene. Coverage: bar_memfd.cpp 83.9% lines (remainder are justified
      syscall-fault-injection + unreachable-default branches). Reviewer signed
      off; normal/asan/ubsan/aubsan all green (346/346) with `-j16`, sanitizers
      PROVEN active via `-fsanitize=` in flags.make (asan=address,
      ubsan=undefined, aubsan=address+undefined).
    * Step 7→8 handoff: `BarMemfd` is internally thread-safe for concurrent
      daemon-side access; Step 8 workers may share the user-region `BarMemfd`.

* **Step 8 — Model control workers — DONE**
    * `src/model_control_workers.{h,cpp}`: `ModelControlWorkers : public
      WorkerController` — one worker thread per compute kernel + one clock-wizard
      worker, spawned by `start(ModelClient&, const SystemMap&)` and joined by
      `stop()`/dtor. Does NOT re-send the global `start` verb (the `ModelProcess`
      owns that one-time sim-clock start). The BAR memfds (user region, clock
      wizard) are INJECTED by reference (owned by Step 9/11, persist across model
      restarts). `KernelState{Idle,Busy,Dead}` tracked per kernel for the process
      lifetime; `kernel_state()`/`kernel_count()`/`running()` introspection.
    * Kernel idle→busy→idle FSM: IDLE polls the control register at
      `base_address + 0`; on `ap_start` (bit0) it captures-and-resets the control
      word atomically via `update_u32`, forwards the WRITABLE parameter registers
      (read-only outputs are skipped so their stale memfd value never clobbers the
      model result), then the captured control value LAST → BUSY. BUSY polls
      `fetch_scalar(control)`; on `ap_done` (bit1) fetches every READABLE register
      back into the memfd and publishes the finished control word → IDLE. Only
      bits 0/1 are interpreted. Address identity: memfd offset == model address ==
      `base_address + register.offset` (matches `vrt/src/kernel.cpp`'s
      `baseAddr + offset` for both the hardware BAR and the sim `reg`/`fetch`).
      A kernel with no offset-0 register (`register_at(0)==nullptr`) spawns a
      worker that idles forever (never starts, never crashes).
    * Clock-wizard worker ORs bit0 into `0x0033C` (user) and `0x1033C` (service)
      each poll cycle via `update_u32` (preserves the other bits); no model
      interaction — just keeps the vrtd clock driver's lock poll from timing out.
    * Error taxonomy: any `ModelClient` `Transport` error → `KernelState::Dead` +
      clean worker exit (no spin on a dead socket); `Protocol` errors are
      non-fatal (retried/skipped). Interruptible `wait_or_stop()` (cv) so teardown
      never waits out a full poll interval. Nothing throws across the API.
    * TOCTOU/atomicity: the architecture's "check control + fetch params + reset =
      one CPU transaction" is met as an atomic capture-and-reset (`update_u32`)
      with the params read in separate flock brackets AFTER — benign because VRT
      writes all params BEFORE `ap_start` (documented at length in the .cpp);
      resolved WITHOUT extending Step 7's `BarMemfd`. The busy→idle publish is also
      `update_u32` and PRESERVES a re-armed `ap_start`, so a concurrent re-arm is
      never silently dropped.
    * Two adversary-found bugs fixed and folded into the suite as regression
      tests: (1) a heap-use-after-free — lock-free `kernel_state()`/
      `kernel_count()` reads racing `stop()`'s `kernels_.clear()`, fixed with a
      `std::shared_mutex` (unique for mutate, shared for read) + join-before-clear;
      (2) a dropped kernel re-arm — the `ap_done` publish blindly overwrote a
      pipelined `ap_start`, fixed by the preserve-via-`update_u32` above.
    * Tests: `tests/model_control_workers_test.cpp` (35 tests incl. 9 adversary
      probes). Full idle→busy→idle with params-before-control ordering asserted via
      `MockModelServer` records; multi-kernel + 6-kernel concurrent stress on the
      shared user-region BAR (`max_in_flight==1` proves serialisation holds);
      no-control-register kernel; zero-width/out-of-range register; autorestart
      `0x81` whole-word forward; ap_done-already-set; RW double-handling; 32-kernel
      clean teardown; clock-wizard both-windows pin / other-bits-preserved /
      re-assert-after-clear; model death (Close) during idle-forward / param-
      forward / mid-busy; Silence timeout; Protocol retry non-fatal; lifecycle
      (double-start reject, stop-without-start, idempotent stop, dtor teardown,
      restart); the two regression probes. Coverage: model_control_workers.cpp
      92.41% lines / 97.47% branches (`gcov -b`; remainder are justified
      OS-fault-injection + unreachable-default + nondeterministic output-fetch-
      death branches). Reviewer signed off after independent clean builds;
      normal/asan/ubsan/aubsan all green (381/381) with `-j16`, sanitizers PROVEN
      active via `-fsanitize=` in flags.make.
    * Step 8→9 handoff: the concrete `WorkerController` is now available for Step 6
      reconfiguration to drive; Step 9/11 construct it with the accelerator's
      `BarSet` user-region + clock-wizard memfds and manage its start/stop with the
      model process.

* **Step 9 — BAR/device-info subsystem (PF2) — DONE**
    * `src/ctl_subsystem.{h,cpp}`: `CtlSubsystem` serves the `slash_ctl<N>` named
      `AF_UNIX`/`SOCK_SEQPACKET` socket (the emulated `/dev/slash_ctl<N>` control
      device). One listener thread (`accept4` loop) + one worker thread per
      connection; each worker recv→dispatch-on-`ioctl_op`→send, mirroring
      `sequence_id`/`ioctl_op` and setting `return_value` = 0 / `-errno`.
      One-thread-per-connection chosen over a pool: PF2 concurrency is small and
      every request is short and non-blocking (no model I/O on this PF), which
      keeps shutdown trivial.
    * IOCTLs (per architecture): `GET_BAR_INFO` (0x30) — BARs 0/2/4 present
      (`usable=1`, `in_use` ALWAYS 0, `start_address=0`, length 128 MiB/128 MiB/
      512 KiB from the `BarSet` sizes); others absent. `GET_BAR_FD` (0x31) — a
      `BarMemfd::reopen()`ed DISTINCT open file description sent via `SCM_RIGHTS`,
      `return_value=0`, length filled, daemon's copy closed after send (no fd
      leak); absent → `-EINVAL`, no fd. `GET_DEVICE_INFO` (0x32) — bdf =
      `<board bdf>.2`, vendor=subsystem_vendor=`0x10EE`, device=`0x50B6`,
      subsystem_device=`0x000e`. Under-length payload → `-EINVAL` (worker
      survives); over-length tolerated (forward-compat); unknown op → `-ENOSYS`.
    * `src/ctl_ioctls.h`: thin wrapper over the OFFICIAL libslash UAPI header
      (`driver/libslash/include/slash/uapi/slash_interface.h`) — a deliberate
      project decision (no hand-mirrored structs, so no ABI drift). CMake adds a
      `SLASH_UAPI_INCLUDE_DIR` cache var (default `<repo>/driver/libslash/include`,
      overridable to an installed libslash) to `slash_emu_core`'s PUBLIC includes,
      with a configure-time `FATAL_ERROR` guard if the header is absent. The
      wrapper adds only the slash_emu-specific PF2 identity constants, clearly
      separated from the ABI include. **Steps 10–12 follow the same rule** (QDMA +
      hotplug structs from the same header).
    * Lifecycle: `setup()` binds → `chmod(mode)` → `chown(uid,gid)` (EPERM
      tolerated for an unprivileged daemon) → `listen`, unlinking any stale file
      first. `remove()` flips a stop flag, unlinks the socket, `shutdown(SHUT_RDWR)`
      + closes the listen fd (accept returns → listener joins), then
      `shutdown()`s every live connection fd (the architecture's forced user
      disconnect) and joins the workers. Connection fds are OWNED by their worker
      (`UniqueFd`); `remove()` only `shutdown()`s them, never closes out from under
      a worker. The `BarSet` is BORROWED (`const&`) and untouched by `remove()` —
      the model control workers keep polling the memfds. Idempotent setup/remove;
      dtor removes if active; setup→remove→setup cycles safe. The RESCAN
      reconfiguration-trigger is deliberately DEFERRED to Step 11 (this is the
      socket-lifecycle half only).
    * Race-freedom (the risk area): `remove()` joins the LISTENER before
      snapshotting the connection map, so no worker can register after the move-out
      window; the listener re-checks `stop_` under the connection mutex after
      `accept4` (closes the fd + breaks if stopping, never orphaning it); a reaper
      drops done-connection entries before a reused fd number can collide; map
      entries are moved out under the lock and joined outside it (no join-under-
      lock, no deadlock). Reviewer independently reasoned through and found no
      race, no fd/thread leak, no close-from-under-a-worker, no deadlock.
    * Tests: `tests/ctl_subsystem_test.cpp` (41 tests: 23 implementer + 18
      adversary). In-process SEQPACKET test client; covers bar_info present/absent
      across the full `__u8` range, bar_fd SCM_RIGHTS + fstat/mmap round-trip + the
      critical distinct-description flock-conflict proof for ALL three BARs + a
      400× daemon-fd-leak loop, device_info exactness, under-length `-EINVAL` +
      unknown-op survival, ownership/mode (mode always asserted; uid only when
      privileged), REMOVE forced-disconnect with memfds-still-work + RESTORE + 5×
      cycles, 16 concurrent clients, connect-storm racing remove at the
      accept-before-register window, fd-number-reuse, destructor-while-connected,
      fd/thread-leak checks. Coverage: ctl_subsystem.cpp 90.3% lines / 85.4%
      branches (`gcov -b`; remainder are justified OS-fault-injection paths).
      Reviewer signed off after independent clean builds; normal/asan/ubsan/aubsan
      all green (422/422) with `-j16`, sanitizers PROVEN active (libasan/libubsan
      confirmed linked), lifecycle stable 5× until-fail under aubsan.
    * Non-blocking nits: (a) `remove()` may `shutdown()` a done-worker's already-
      closed/reused fd number — benign (EBADF/ENOTSOCK ignored); could be tightened
      by skipping done entries. (b) a cosmetic gcov line-split artifact on the
      unknown-op default case.
    * Step 9→11 handoff: Step 11 constructs `CtlSubsystem` with the accelerator's
      resolved `slash_ctl<N>` path + ownership + board BDF + its `BarSet`, and
      drives `setup()`/`remove()` as part of the PF2 RESCAN/REMOVE lifecycle
      (calling `reconfigure()` before `setup()` on a PF2 restore).

* **Step 10 — QDMA subsystem (PF1) — DONE**
    * `src/qdma_subsystem.{h,cpp}`: `QdmaSubsystem` serves the `slash_qdma_ctl<N>`
      named `AF_UNIX`/`SOCK_SEQPACKET` socket (emulated `/dev/slash_qdma_ctl<N>`).
      Two endpoint kinds, dispatched by `(endpoint, ioctl_op)`: **CTL** (the
      top-level socket) accepts `INFO` (0x50), `QPAIR_ADD` (0x51), `Q_OP` (0x52),
      `QPAIR_GET_FD` (0x53), `BUF_CREATE` (0x54); **XFER** (a per-`GET_FD`
      anonymous `SEQPACKET` socketpair) accepts `BUF_CREATE` (0x54) and `TRANSFER`
      (0x56). Op on the wrong endpoint → `-EINVAL`; unknown op → `-ENOSYS`; workers
      survive both. Threading mirrors `CtlSubsystem`: one `accept4` listener + one
      worker per CTL connection; each `GET_FD` spawns a dedicated transfer-session
      worker on the daemon end of the socketpair.
    * Qpair state machine: `Initial -[ADD]-> Stopped`; `Stopped|Started -[START]->
      Started`; `Started -[GET_FD]-> Used`; `Used -[last close on FD]-> Started`;
      `Started|Stopped -[STOP]-> Stopped`; `Started|Stopped|Used -[DEL]-> removed`.
      A `STOP`/`DEL` may land underneath a live session (models the driver not
      invalidating a session); the transfer path then observes `-ENODEV`. Invalid
      transitions → `-EINVAL`. Multi-qpair `GET_FD` is all-or-nothing (validate all
      Started, then transition; revert on socketpair/teardown failure). **Lead
      decision:** a `GET_FD` list naming the same qid twice is rejected with
      `-EINVAL` (mirrors the driver; double-binding one qpair into a single session
      has no legitimate caller). Session workers are keyed by a monotonic id, NOT
      the fd, to dodge fd-reuse map collisions.
    * `TRANSFER` (per sub-transfer, 64 KiB chunks): H2C to HBM/DDR = `pread` the
      referenced fd → `ModelClient::populate`; C2H from HBM/DDR = `ModelClient::
      fetch_buffer` → `pwrite` the fd; H2C to the reconfiguration aperture
      (`dev_addr == kReconfigApertureAddr`, 0x102100000) = `pread` →
      `VbinStore::append_staging` (NOT sent to the model, `dev_addr` NOT advanced).
      `buf_fd` is an INDEX into the `SCM_RIGHTS` fd list (`resolve_fd_index`);
      `return_value` = total bytes. Model serialisation leans ENTIRELY on
      `ModelClient`'s per-socket mutex (one request in flight); `pread`/`pwrite`
      are kept OUTSIDE that lock so sessions' file I/O overlaps while model requests
      serialise. `INFO` returns `bdf = <board bdf>.1` + zeroed caps. `BUF_CREATE`
      returns a page-granule memfd (hint `SINGLE_QPAIR`, a deliberate V80-driver
      difference) via `SCM_RIGHTS`, closing the daemon's copy after send (no leak,
      no daemon-side reference).
    * `src/qdma_ioctls.h`: thin wrapper over the OFFICIAL libslash UAPI header
      (same `SLASH_UAPI_INCLUDE_DIR` rule as Step 9), adding only PF1 identity
      constants. **ABI change:** `char bdf[SLASH_PCI_BDF_LEN]` appended to
      `slash_qdma_info` (sizeof 20→52) so userspace discovers the PF1 BDF without
      sysfs (lead+user decision to extend the official struct now, not a local
      mirror). `kReconfigApertureAddr` lives here for now; **Step 13 moves it +
      the HBM/DDR/reconfig memory ranges into the kernel ABI header**.
    * Ownership: `ModelClient` + `VbinStore` are BORROWED and untouched by
      `remove()` (which forgets the qpair list, force-disconnects all CTL
      connections + transfer sessions, and joins); `setup()` re-inits the qpair
      list (RESCAN half). Idempotent setup/remove; dtor removes if active.
    * Tests: `tests/qdma_subsystem_test.cpp` (70 tests: 53 implementer + 16
      adversary + 1 user-requested end-to-end roundtrip), backed by the shared
      `MockModelServer` (address-keyed memory) + a real `VbinStore`. Covers the full
      state machine, both transfer directions incl. multi-chunk, the reconfig
      aperture path (dev_addr pinned, model untouched), all-or-nothing multi-qpair
      GET_FD + revert, wrong-endpoint/unknown-op routing, short/oversized/garbage
      payloads on every op, SCM_RIGHTS fd-leak audits on error paths, `Used`-
      underneath STOP/DEL, teardown/destructor races (remove while sessions busy /
      mid-transfer), 200× concurrent STOP/START-vs-transfer (no torn result), and a
      **full host→device→host roundtrip** (`RoundtripHostToDeviceToHostAEqualsB`):
      host buffers A/B allocated via `BUF_CREATE`, device buffer C in the model,
      A→C (H2C) then C→B (C2H) at 128 KiB (crosses the chunk boundary both ways),
      asserting `A == B` and that C holds A's bytes. Adversary found NO real bugs;
      all four lead-flagged suspicions (int32 return overflow, precondition/exec
      race, fd-number done-flagging, `connection_count` counting sessions) were
      investigated and confirmed safe/intended. Coverage: `qdma_subsystem.cpp`
      ~89% lines / ~85% branches (`gcov -b`; remainder are un-injectable OS-fault
      paths). Clean 4-dir builds green (492/492): normal `-j16`, asan/ubsan/aubsan
      `-j8` (WSL2 full-suite -j16 is memory-pressure flaky), `-fsanitize` PROVEN
      live in each `flags.make`.
    * Step 10→11 handoff: Step 11 constructs `QdmaSubsystem` with the accelerator's
      resolved `slash_qdma_ctl<N>` path + ownership + board BDF + its `ModelClient`
      and `VbinStore`, and drives `setup()`/`remove()` as part of the PF1
      RESCAN/REMOVE lifecycle. The reconfig-aperture H2C→`append_staging` path is
      the daemon-side half of Step 6 reconfiguration; Step 11 owns promoting the
      staged VBIN and restarting the model.

* **Step 11 — Accelerator lifecycle and hotplug — DONE**
    * `src/accelerator.{h,cpp}`: `Accelerator` is the per-BDF state machine
      (`AccelState` = Absent/Inactive/Active/Partial) that owns the `ModelInstance`
      + PF1 (`QdmaSubsystem`) + PF2 (`CtlSubsystem`) for one board and enforces
      instantiation/teardown ordering. `instantiate()`: ensure model → `setup_qdma`
      (PF1) → `setup_ctl` (PF2). `teardown()`: reverse. `restore_pf(Pf2)` (the
      reconfiguration path, driven from the QDMA staged-VBIN promotion) **quiesces
      PF1 first** (`qdma_->remove()` + reset while the old `ModelClient` is still
      alive) BEFORE `ensure_model()` destroys it, then rebuilds PF2 and reconstructs
      PF1 against the new model — this is what keeps `qdma_subsystem.*` PRISTINE at
      its Step-10 borrow-by-reference contract (no `rebind_model`, no snapshot-
      pointer UAF; device HBM/DDR is defined not to persist across reconfig, so
      dropping stale qpair state is intended, per the memory-range notes above).
    * **Model-death generation guard.** Each model launch mints a process-globally-
      unique generation from a `shared_ptr<atomic<uint64_t>>` counter owned by the
      `Accelerator` (survives `model_.reset()`); the per-launch `DeathCallback`
      captures its own generation and posts `on_model_died(gen)` to the lifecycle
      thread. `Accelerator::on_model_died(gen)` no-ops unless `gen == generation()`,
      so a stale death task for a process already replaced by HOTPLUG/RESCAN cannot
      tear down the healthy re-adopted model (the adversary's segfault, fixed).
      Honours the `DeathCallback` contract: the monitor thread only posts; teardown
      runs on the lifecycle thread.
    * `src/hotplug_subsystem.{h,cpp}` + `src/hotplug_ioctls.h`: the daemon-level
      `slash_hotplug` `AF_UNIX`/`SOCK_SEQPACKET` socket, serving RESCAN (`_IO('w',
      0x30)`, no-arg), REMOVE/TOGGLE_SBR/HOTPLUG (`_IOW('w', 0x31/0x32/0x33,
      slash_hotplug_device_request{u32 size; char bdf[32]})`) over the shared
      datagram transport. `hotplug_ioctls.h` is the thin wrapper over the OFFICIAL
      `<slash/uapi/slash_hotplug.h>` (real kernel driver `driver/slash_hotplug.c`;
      same `SLASH_UAPI_INCLUDE_DIR` rule). BDF targeting: `.0/.1/.2` suffix → one PF;
      bare board BDF → all PFs; empty BDF → the single tracked device (`-EOPNOTSUPP`
      if multiple, `-ENODEV` if none). A **single `lifecycle_mu_`** serialises
      RESCAN/REMOVE/HOTPLUG/TOGGLE_SBR and the posted death-teardown; a dedicated
      lifecycle thread/queue runs death callbacks off the monitor thread. TOGGLE_SBR
      emulates the 1 s link-training delay.
    * `src/daemon.{h,cpp}` + `main.cpp`: `run_daemon()` does
      `cold_reboot_cleanup_all` → `hotplug.setup()` → automatic startup RESCAN →
      `wait_for_shutdown()` → `hotplug.remove()` → `cold_reboot_cleanup_all`.
      Async-signal-safe self-pipe shutdown (handler = atomic CAS + `write` 1 byte;
      main thread `read`/`poll` with EINTR retry). **Bug #2 fix:** the shutdown flag
      is cleared at run-END (`close_shutdown_state`), not at reset, so a request that
      arrives before the loop starts is not lost (was a hang). Re-invocable across
      runs with no self-pipe fd leak.
    * `src/reconfigure.{h,cpp}`: `ModelInstance` ctor extended to take the per-launch
      `on_death_gen` callback + the shared generation counter; each launch mints its
      generation, wraps it into the `DeathCallback`, and records `current_gen_`
      (exposed via `current_generation()`). `adopt()` destroys the old
      `ModelProcess` synchronously.
    * Tests: `tests/accelerator_test.cpp` (22), `tests/hotplug_subsystem_test.cpp`
      (45), `tests/daemon_test.cpp` (10, incl. the bug-#2 regression
      `ShutdownRequestedBeforeStartIsNotLost`, both-signal storm, re-invocation fd-
      leak audit), plus the migrated `tests/reconfigure_test.cpp`. Covers the full
      state machine, PF1+PF2 ordering, RESCAN/REMOVE/HOTPLUG/TOGGLE_SBR routing +
      BDF targeting + ABI edge cases, the quiesce-first PF2 restore, and the two
      adversary bugs with regressions: the **stale-death generation guard**
      (`OnModelDiedStaleGenerationIsNoop`, `StaleDeathTaskDoesNotTearDownReadopted
      Model` ×12 iters, `ModelKilledDuringPf2RestoreIsClean`,
      `InflightTransferRacingReconfigureIsClean`,
      `QdmaReconstructsAgainstNewModelAfterReconfigure`) and the shutdown-flag fix.
      Clean 4-dir builds green (**565/565**): normal `-j16`, asan/ubsan/aubsan `-j8`
      (WSL2 full-suite -j16 is memory-pressure flaky), `-fsanitize` PROVEN live in
      each `flags.make`; `qdma_subsystem.*` confirmed empty-diff vs Step 10.
    * Step 11→12 handoff: the daemon now stands up a full emulated board from an
      empty `/run/slash_emu` and drives it through the hotplug socket; Step 12 points
      libslash at these sockets (control-file-vs-socket flag) and forwards the PF1
      BDF from the extended QDMA `INFO` field.

* **Steps 12–14 — NOT STARTED.** Next up: **Step 12 — libslash integration**
  (control-file-vs-socket flag, IOCTL↔datagram translation, SCM_RIGHTS fd-by-index
  both directions, QDMA BDF forwarding). Then 13 (kernel driver + VRTD changes,
  incl. moving `kReconfigApertureAddr` + memory ranges into the ABI header and
  driver-side QDMA bdf), 14 (end-to-end integration).

### Reusable building blocks now available

* `Result<T>`/`Result<void>` + `ErrorKind` (Transport vs Protocol) — the error
  vocabulary for all subsystems; Transport failures become `-ENODEV`.
* `UniqueFd` RAII fd owner; `send_message`/`recv_message`/`send_request` and the
  FD-index helpers — the datagram transport for PF2/QDMA/hotplug.
* `DaemonConfig` + socket-path helpers — feed the subsystem setup in Steps 9–11.
* `unpack_vbin` + `SystemMap`/`Kernel`/`Register`/`FunctionalArg`/`Connection`
  model (with `VbinError` taxonomy and RAII `TempDir`) — feed the model process
  launch/reconfiguration (Step 6), the model control workers and the
  register→address mapping (Step 8), and QDMA target routing (Step 10).
* `ModelClient` (vpp_sim ZeroMQ dialect, serialized + timed-out + Transport/
  Protocol error taxonomy) and the scriptable `MockModelServer` test double —
  drive the model process in the model control workers (Step 8) and QDMA
  transfers (Step 10); the mock is the shared fixture for both.
* `VbinStore`, `ModelProcess`, `WorkerController`, and `ModelInstance`
  (reconfigure) — the per-accelerator model lifecycle. Step 8 implements the
  real `WorkerController`; Step 10 appends reconfiguration chunks via
  `append_staging`; Step 11 drives `reconfigure()`/teardown and owns
  `cold_reboot_cleanup` — and MUST post model-death teardown to a thread other
  than the death-callback (monitor) thread per the `DeathCallback` contract.
  The `fake_model` executable + fixture VBINs are the shared end-to-end harness.
* `BarMemfd` + `BarSet`/`make_standard_bars()` — memfd-backed BAR windows with
  flock brackets, `update_u32` handshake helper, internal per-BAR mutex, and
  `reopen()` (distinct open file description). Feed Step 8 (workers poll/handshake
  the user-region BAR; internally thread-safe so per-kernel workers share it) and
  Step 9 (GET_BAR_INFO via `by_index`/sizes; GET_BAR_FD sends a `reopen()`ed fd).
* `ModelControlWorkers` (the concrete `WorkerController`) — per-kernel idle→busy→
  idle FSM + clock-wizard lock-bit pinning, driven over `ModelClient` against the
  injected user-region/clock-wizard `BarMemfd`s. Step 6 reconfiguration drives its
  `start`/`stop`; Step 9/11 construct it with the accelerator's `BarSet` and bind
  its lifetime to the model process.
* `CtlSubsystem` + `ctl_ioctls.h` (thin UAPI wrapper) — the PF2 `slash_ctl<N>`
  named-socket subsystem: listener + per-connection workers, GET_BAR_INFO/
  GET_BAR_FD/GET_DEVICE_INFO, SCM_RIGHTS BAR-fd passing (`reopen()`), and
  idempotent `setup()`/`remove()` over a borrowed `BarSet`. The established
  pattern for the QDMA (Step 10) and hotplug (Step 11) sockets: reuse the transport
  helpers, depend on the official UAPI header (`SLASH_UAPI_INCLUDE_DIR`), and keep
  BAR/model state borrowed so REMOVE leaves it running. Step 11 drives its
  lifecycle.
* `QdmaSubsystem` + `qdma_ioctls.h` (thin UAPI wrapper) — the PF1
  `slash_qdma_ctl<N>` named-socket subsystem: CTL + per-`GET_FD` XFER-session
  endpoints, the qpair state machine, `SCM_RIGHTS` fd passing (host-buffer memfds
  + session sockets), and H2C/C2H `TRANSFER` routing to `ModelClient` (HBM/DDR) or
  `VbinStore::append_staging` (reconfig aperture) over a borrowed model + vbin
  store. Serialisation leans on `ModelClient`'s per-socket lock with file I/O
  outside it. Idempotent `setup()`/`remove()`. Step 11 drives its lifecycle and
  owns staged-VBIN promotion + model restart.
* `Accelerator` (`accelerator.{h,cpp}`) — the per-BDF state machine that owns one
  board's `ModelInstance` + PF1 + PF2 and enforces instantiate/teardown ordering,
  the quiesce-first PF2-restore reconfiguration path, and the model-death
  generation guard (`on_model_died(gen)` no-ops on stale generations). Step 12+
  constructs one per discovered board.
* `HotplugSubsystem` + `hotplug_ioctls.h` (thin UAPI wrapper over the official
  `slash_hotplug.h`) — the daemon-level `slash_hotplug` socket serialising
  RESCAN/REMOVE/HOTPLUG/TOGGLE_SBR (+ BDF targeting) and the posted death-teardown
  under one `lifecycle_mu_`, with a lifecycle thread for off-monitor death
  callbacks. Drives every `Accelerator`'s lifecycle.
* `run_daemon()` + the self-pipe shutdown (`daemon.{h,cpp}`) — the real event-loop
  entry point: cold-reboot cleanup → hotplug setup → startup RESCAN → async-signal-
  safe self-pipe wait → REMOVE → cleanup; re-invocable, no fd leak. The Step-1
  `cv.notify_all()`-from-handler note is now resolved.

### Deferred / outstanding items

* **MVP scope exclusions** (see the "Scope" section) remain deferred by design:
  virtual network setups, non-polling BAR, FPGA-emulation-model support,
  hardened model-process isolation, and persisting HBM/DDR across
  reconfigurations.
* **Signal-handler async-signal-safety (from Step 1): RESOLVED in Step 11.**
  `run_daemon()` now uses an async-signal-safe self-pipe (handler = atomic CAS +
  `write` of 1 byte; main thread `read`/`poll`), replacing the earlier
  `cv.notify_all()`-from-handler.
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
