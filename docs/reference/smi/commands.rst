..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

#####################
Command Reference
#####################

``v80-smi`` is the command-line system management interface for AMD Alveo V80
boards. Running ``v80-smi`` with no subcommand prints usage help.

Device Addressing
=================

Several commands accept a ``-d/--device`` option that takes a **BDF**
(Bus:Device.Function) address. The following formats are supported:

.. list-table::
   :header-rows: 1
   :widths: 30 30

   * - Format
     - Example
   * - ``BB:DD`` (short)
     - ``03:00``
   * - ``BB:DD.F`` (short with function)
     - ``03:00.0``
   * - ``DDDD:BB:DD`` (domain:bus:device)
     - ``0000:03:00``
   * - ``DDDD:BB:DD.F`` (full)
     - ``0000:03:00.0``

Commands
========

version
-------

Print the v80-smi version and exit.

.. code-block:: text

   v80-smi version [-p|--plain]

.. option:: -p, --plain

   Print only the version in ``x.y.z`` format with no prefix. Useful for
   scripting.

list
----

Enumerate V80 boards visible on the system and report their readiness status.

.. code-block:: text

   v80-smi list [-j|--json] [-J|--pretty-json] [-l|--long] [-s|--sensors]

.. option:: -j, --json

   Output as compact JSON.

.. option:: -J, --pretty-json

   Output as indented JSON.

.. option:: -l, --long

   Include additional information (PCI IDs, driver status).

.. option:: -s, --sensors

   Include sensor readings (temperature, power). Requires the vrtd daemon to be
   running.

inspect
-------

Display metadata from a vrtbin file on disk without programming it onto a
device.

.. code-block:: text

   v80-smi inspect <vbin> [-j|--json] [-J|--pretty-json]

.. option:: vbin

   Path to the vrtbin file. **Required.**

.. option:: -j, --json

   Output as compact JSON.

.. option:: -J, --pretty-json

   Output as indented JSON.

query
-----

Display the metadata of the vrtbin currently loaded on a device.

.. code-block:: text

   v80-smi query -d <BDF> [-j|--json] [-J|--pretty-json]

.. option:: -d, --device <BDF>

   Board address. **Required.**

.. option:: -j, --json

   Output as compact JSON.

.. option:: -J, --pretty-json

   Output as indented JSON.

.. _shell-build-id:

Shell build ID
~~~~~~~~~~~~~~

``list`` and ``query`` read a read-only register baked into the static region
of the shell, reporting which shell build is physically loaded on the board.
Both shells expose it at BD address ``0x0204_0002_0000`` — offset
``0x20000`` within BAR4 — as a dual-channel AXI GPIO.

.. list-table::
   :header-rows: 1
   :widths: 20 20 60

   * - Register
     - Bits
     - Meaning
   * - ``+0x0`` (channel 1)
     - ``[31:0]``
     - Low 32 bits of the commit prefix
   * - ``+0x8`` (channel 2)
     - ``[27:0]``
     - High 28 bits of the commit prefix
   * - ``+0x8`` (channel 2)
     - ``[28]``
     - Shell variant: ``0`` = service, ``1`` = compute
   * - ``+0x8`` (channel 2)
     - ``[30:29]``
     - Reserved
   * - ``+0x8`` (channel 2)
     - ``[31]``
     - Set when the shell was built from a dirty working tree

The commit value is the top 60 bits of the SLASH source SHA-1, so it reads as
the commit's short hash. It is ``0`` with the dirty bit set when the shell was
built outside a git checkout.

Which shell is loaded
~~~~~~~~~~~~~~~~~~~~~

Three different things can be called "the shell type", and SMI keeps them
apart:

* **The shell that is loaded.** Bit ``[28]`` of the register, baked into the
  bitstream, so it is the authoritative answer. ``list`` prints it as
  ``Shell:`` and ``query`` as ``Shell loaded:``.
* **The shell state ``vrtd`` tracks.** Software bookkeeping in the daemon,
  reset to unknown when the daemon restarts. ``list --long`` shows it under
  ``VRTD:`` as ``Shell state:``. It is used for the ``Shell:`` line, marked
  ``(per vrtd)``, only as a fallback when the register cannot be read.
* **The shell a vbin requires.** The ``<ShellType>`` node in the vbin's system
  map, reported by ``inspect`` and ``query`` as ``Shell required:``. This is a
  request telling the stack which shell to program onto the board, not a
  statement about the board's current state.

In JSON, ``list`` and ``query`` report the loaded shell as ``shell`` alongside
a ``shell_source`` of ``hardware`` or ``vrtd``; ``inspect`` and ``query``
report the vbin's request as ``shell_required``; and ``vrtd.shell_type`` in
``list`` stays the daemon's own tracked state.

Service shells built before bit ``[28]`` was assigned read back as
``service``, since the reserved bits were tied to zero.

Reading the register is best-effort: it needs a usable BAR4. ``list`` falls
back to the ``vrtd`` state when it is unavailable, and ``query`` prints a
warning and omits the loaded-shell and commit fields.

program
-------

Load a vrtbin file onto a device, programming the FPGA.

.. code-block:: text

   v80-smi program <vbin> -d <BDF>

.. option:: vbin

   Path to the vrtbin file. **Required.**

.. option:: -d, --device <BDF>

   Board address. **Required.**

reset
-----

Perform a hardware reset of a V80 board. This executes a full PCIe secondary
bus reset and rescan (hotplug) sequence.

.. code-block:: text

   v80-smi reset -d <BDF> [--shell-type <service|compute>]

.. option:: -d, --device <BDF>

   Board address. **Required.**

.. option:: --shell-type <service|compute>

   Shell to boot after reset. ``service`` selects boot partition 0 and
   ``compute`` selects boot partition 1. Defaults to ``service``.

write-static-shell
------------------

Write the installed static SLASH shell PDI to a V80 board.

.. code-block:: text

   v80-smi write-static-shell --flash -d <BDF> [--shell-type <all|service|compute>] [--pdi <file>]
   v80-smi write-static-shell --jtag -d <BDF> [--shell-type <service|compute>] [--pdi <file>] [--xsdb-target-id <id>] [--bash-source <file> ...]
   v80-smi write-static-shell --jtag --no-remove-device [--shell-type <service|compute>] [--pdi <file>] [--xsdb-target-id <id>] [--bash-source <file> ...]

Modes:

* ``--flash`` resolves the selected shell PDI via
  ``python3 -m slashkit static-shell-path`` and programs its boot partition
  through the VRTD cfgmem command. Without ``--pdi`` or ``--shell-type``, it
  programs both the service and compute partitions.
* ``--jtag`` resolves ``amd_v80_gen5x8_25.1_nofpt.pdi`` the same way,
  optionally sources Vivado/Vitis setup scripts, and runs ``xsdb`` with the
  installed ``versal_flash_pdi.tcl`` script.
* ``--pdi`` bypasses static-shell path resolution for active development. The
  file must match the selected mode: use a flash-image PDI with ``--flash`` and
  a no-FPT/JTAG-bootable PDI with ``--jtag``. It cannot be combined with
  ``--shell-type all``.

.. option:: --flash

   Program the flash-image PDI via VRTD cfgmem programming.

.. option:: --jtag

   Program the no-FPT PDI over JTAG via ``xsdb``.

.. option:: -d, --device <BDF>

   Board address. Required except with ``--jtag --no-remove-device``.

.. option:: --shell-type <all|service|compute>

   Shell partition to program. ``all`` is valid only with ``--flash`` and no
   ``--pdi``. Flash mode without a PDI override defaults to ``all``; JTAG mode
   and commands with ``--pdi`` default to ``service``.

.. option:: --pdi <file>

   Use this PDI file instead of resolving the installed static shell PDI.

.. option:: --no-remove-device

   Skip the pre-JTAG PCIe device removal. Valid only with ``--jtag``. PCIe
   rescan still runs after ``xsdb``.

.. option:: --xsdb-target-id <id>

   Select the ``Versal xcv80`` XSDB ``target_id``. Valid only with ``--jtag``.

.. option:: --bash-source <file>

   Source a shell setup script before running ``xsdb``. May be repeated and is
   valid only with ``--jtag``.

In JTAG mode, ``v80-smi`` removes all V80 PCIe functions through VRTD unless
``--no-remove-device`` is used, runs ``/bin/bash -c 'source ...; xsdb ...'``
with ``PDI_PATH`` set to the selected PDI, optionally sets ``V80_TARGET_ID``
from ``--xsdb-target-id``, and always asks VRTD to rescan PCIe after
the ``xsdb`` step completes. Since default PDI resolution uses
``python3 -m slashkit``, setting ``PYTHONPATH`` can select an in-repo
``slashkit``.

Progress is printed to stderr. Flash mode polls VRTD for cfgmem job status and
prints phase changes plus interval-based PDI download progress. JTAG mode prints
local stages such as PCIe removal, ``xsdb`` execution, and PCIe rescan.

validate
--------

Run memory integrity and bandwidth tests against a board's HBM and DDR
subsystems. For each memory path, bandwidth is reported as single-direction
C2H read, single-direction H2C write, and simultaneous bidirectional
throughput (read, write, and total). After the per-memory phases, a final
parallel phase drives HBM and DDR simultaneously with ``2 * N`` buffers for
single-direction tests and ``4 * N`` threads for bidirectional tests; this
phase is skipped when ``--ddr-only`` or ``--hbm-only`` is given.

.. code-block:: text

   v80-smi validate -d <BDF> [-j|--threads <N>] [-R|--no-reset] [--mm-channel <spec>] [--buffer-size <size>] [--offset <size>] [--starting-offset <size>] [--raw-transfer-test | --use-qdma-driver] [--ddr-only | --hbm-only] [--channel-allocation <auto|paired>] [--channel-region-stride <size>] [--ring-size-index <0-15>] [--bandwidth-iterations <N>] [--bandwidth-duration <seconds>]

Requirements by mode:

* Default mode uses VRTD buffers, requires a running VRTD daemon, and resets
  the board unless ``--no-reset`` is given.
* ``--raw-transfer-test`` bypasses VRTD for transfers and requires the SLASH
  QDMA driver device node for the board. It skips reset.
* ``--use-qdma-driver`` bypasses both VRTD and SLASH for transfers and requires
  the stock ``qdma-pf`` driver to be bound to the board's QDMA PF. This backend
  is built only when ``SMI_ENABLE_QDMA_DRIVER_BACKEND`` is enabled at CMake
  configure time.

.. option:: -d, --device <BDF>

   Board address. **Required.**

.. option:: -j, --threads <N>

   Number of parallel buffers/threads for the validation test (1–64, default 8).
   Bidirectional phases use ``2 * N`` logical positions in each enabled memory
   space.

.. option:: --buffer-size <size>

   Size of each test buffer. Values may be bare bytes or use ``k``/``K`` or
   ``m``/``M`` suffixes. The default and maximum are ``512M``. Values must be
   4 KiB-aligned.

.. option:: --offset <size>

   Distance between logical buffer positions. The default is ``512M``. Values
   may be bare bytes or use ``k``/``K`` or ``m``/``M`` suffixes, must be
   4 KiB-aligned, and must be at least ``--buffer-size`` so buffers do not
   overlap.

.. option:: --starting-offset <size>

   Offset from each memory-space base for logical position 0. The default is
   ``0``. Values may be bare bytes or use ``k``/``K`` or ``m``/``M`` suffixes
   and must be 4 KiB-aligned.

Buffers are placed at ``memory_base + starting_offset + position * offset``.
Single-direction phases use positions ``0..N-1``. Bidirectional phases use
positions ``0..2N-1`` with reads on even positions and writes on odd positions.
The full range must remain inside the 64 x 512 MB DDR/HBM address space. If any
placement option is specified in default VRTD mode, ``validate`` uses raw VRTD
buffers so the exact addresses are honored; this requires raw memory access
permission.

The largest phase maps up to ``4 * N * buffer-size`` of host buffers when both
HBM and DDR are enabled, or ``2 * N * buffer-size`` with ``--ddr-only`` or
``--hbm-only``; the command fails early if that exceeds currently available
host memory.

.. option:: -R, --no-reset

   Skip the device reset step before running memory tests.

.. option:: --mm-channel <spec>

   AXI-MM / NoC channel selection for each buffer's QDMA queue pair, in every
   mode. ``spec`` is either a single value applied to all buffers, or a
   comma-separated list giving one channel per logical buffer position
   (exactly ``2 x --threads`` entries; there is no repeating/wrap, and any
   other length is an error):

   * ``auto`` (the default) lets the driver stripe queues across both channels
     by ``qid & 1``.
   * ``0`` / ``1`` pin the queue to that AXI-MM channel (and hence NoC channel).
   * e.g. with ``-j 1`` the list ``0,1`` puts buffer position 0 on channel 0 and
     position 1 on channel 1. Bidirectional phases use positions ``0..2N-1``;
     single-direction phases use the first ``N`` entries.

   This is independent of ``--channel-allocation`` (which controls the device
   address): ``--mm-channel`` controls the host-side NoC ingress (NMU) per
   queue. With ``--use-qdma-driver`` the selection maps to the stock driver's
   per-queue MM-channel attribute.

.. option:: --raw-transfer-test

   Use libslash raw QDMA transfers instead of VRTD buffers. This mode implies
   ``--no-reset`` and requires the SLASH QDMA driver device to be present.

.. option:: --use-qdma-driver

   Run the raw transfer test over the off-the-shelf Xilinx QDMA driver
   (``/dev/qdma<idx>-MM-<qid>``) instead of SLASH. smi provisions the queues
   itself: it raises the function's ``qmax`` via sysfs if needed, creates and
   starts bidirectional AXI-MM queue pairs over generic netlink (the same
   ``xnl_pf`` interface ``dma-ctl`` uses), then transfers over the per-queue
   char devices. Queue pairs are spread round-robin across the function's MM
   engine channels (``channel = qid % mm_channel_max``); the CPM5 QDMA on the
   V80 exposes two, so the test exercises both. This mode implies
   ``--no-reset`` and is mutually exclusive with ``--raw-transfer-test``. It
   requires the stock ``qdma-pf`` driver to be bound to the board's PF (it
   cannot be bound at the same time as the SLASH driver), and typically
   requires root to raise ``qmax`` and open the queue devices.

.. option:: --ddr-only

   Run only the DDR memory tests and skip the HBM phase. Mutually exclusive
   with ``--hbm-only``.

.. option:: --hbm-only

   Run only the HBM memory tests and skip the DDR phase. Mutually exclusive
   with ``--ddr-only``.

.. option:: --channel-allocation <auto|paired>

   Raw-transfer-only (``--raw-transfer-test`` or ``--use-qdma-driver``) control
   over how QDMA MM/NoC channels map onto device memory. On CPM5 the host-side
   NoC ingress port (NMU) is chosen per queue by the SW-context
   mm-channel/host_id (SLASH uses ``qid & 1``), while the memory-side NoC egress
   endpoint (NSU / pseudo-channel) is chosen by the device address. Default
   ``auto`` keeps the historical behaviour: channel ``qid & 1`` with linear
   addressing, so both NMUs can converge on a single NSU and bandwidth caps at
   one path. ``paired`` couples the two: even positions land in memory region 0
   on channel 0, odd positions in region 1 on channel 1 (one
   ``--channel-region-stride`` apart), giving two independent NMU->NSU paths.
   This mirrors the off-the-shelf ``dma-perf`` ``offset_ch0``/``offset_ch1``
   knobs and is the placement that lets both NoC ports contribute bandwidth.

.. option:: --channel-region-stride <size>

   In ``--channel-allocation paired`` mode, the byte distance between the two
   per-channel memory regions (the NSU / pseudo-channel stride). Default ``16G``
   (== half the per-memory address space, matching the dma-perf HBM
   ``offset_ch1 - offset_ch0`` spacing). Must be a non-zero multiple of 4 KiB.
   Accepts bare bytes or ``k``/``K``, ``m``/``M``, ``g``/``G`` suffixes.

.. option:: --ring-size-index <0-15>

   Raw-transfer-only (``--raw-transfer-test`` or ``--use-qdma-driver``).
   Override the QDMA descriptor-ring size index used when creating SLASH raw
   queue pairs or starting stock-driver queues. When omitted, each backend keeps
   its existing default. Useful A/B values for 4 KiB descriptor throughput are
   ``0``, ``11``, ``13``, and ``15``.

.. option:: --bandwidth-iterations <N>

   Raw-transfer-only (``--raw-transfer-test`` or ``--use-qdma-driver``). Repeat
   each whole-buffer transfer in every bandwidth phase ``N`` times and report
   bandwidth over the sustained loop. The default is ``1``, which preserves the
   historical one-shot measurement.

.. option:: --bandwidth-duration <seconds>

   Raw-transfer-only duration mode. When non-zero, each bandwidth phase repeats
   whole-buffer transfers until the requested wall-clock duration has elapsed
   and counts only completed transfers. This is useful for comparing SLASH's raw
   path against long-running tools such as ``dma-perf``. A value of ``0`` uses
   ``--bandwidth-iterations`` instead.

debug
-----

Low-level troubleshooting commands.

debug bar-poke
^^^^^^^^^^^^^^

Read or write BAR words.

.. code-block:: text

   v80-smi debug bar-poke -d <BDF> -b <BAR> (-r|--read | -w|--write) [-x|--hex] [-W|--word-size <N>] [-c|--count <N>] <address> [value]

.. option:: -d, --device <BDF>

   Board address. **Required.**

.. option:: -b, --bar <BAR>

   BAR number (0-5). **Required.**

.. option:: -r, --read

   Read mode.

.. option:: -w, --write

   Write mode.

.. option:: -x, --hex

   Print read output in hexadecimal.

.. option:: -W, --word-size <N>

   Access width in bytes: 1, 2, 4, or 8 (default 4).

.. option:: -c, --count <N>

   Number of words to read (default 1; must be 1 for write).

Rules:

- Exactly one of ``--read`` or ``--write`` must be provided.
- ``value`` is required for write and forbidden for read.
- ``address`` is a BAR-relative byte offset.

debug mem-poke
^^^^^^^^^^^^^^

Read or write device memory at a raw physical address. This bypasses the
allocator and requires raw-mem-access permission in vrtd.

.. code-block:: text

   v80-smi debug mem-poke -d <BDF> (-r|--read | -w|--write) [-x|--hex] [-W|--word-size <N>] [-c|--count <N>] <address> [value] [-f|--file <path>]

.. option:: -d, --device <BDF>

   Board address. **Required.**

.. option:: -r, --read

   Read mode.

.. option:: -w, --write

   Write mode.

.. option:: -x, --hex

   Hex mode.

   - Read-to-stdout: prints values in hexadecimal.
   - With ``--file``: treats file payload as hex text/hexdump format.

.. option:: -W, --word-size <N>

   Access width in bytes: 1, 2, 4, or 8 (default 4).

.. option:: -c, --count <N>

   Number of words to transfer (default 1).

.. option:: -f, --file <path>

   File mode transfer path.

   - In read mode: destination file.
   - In write mode: source file.

Rules:

- Exactly one of ``--read`` or ``--write`` must be provided.
- ``address`` is a device physical address.
- ``word-size`` must be 1, 2, 4, or 8.
- ``count`` must be greater than zero.
- Scalar mode (no ``--file``):
  - write requires ``value`` and forces ``count == 1``
  - read forbids ``value``
  - address must be aligned to word-size
- File mode (``--file`` present):
  - ``value`` is forbidden
  - transfer size is exactly ``word-size * count`` bytes
  - with ``--hex`` file is text hex/hexdump; without ``--hex`` file is raw binary

debug clockwiz
^^^^^^^^^^^^^^

Read or set clock rate for a device clock region using vrtd clock-op.

.. code-block:: text

   v80-smi debug clockwiz -d <BDF> (--get | --set <rate_hz>) [--region <user|service>] [-x|--hex]

.. option:: -d, --device <BDF>

   Board address. **Required.**

.. option:: --get

   Read current clock rate for selected region.

.. option:: --set <rate_hz>

   Set requested clock rate in Hz for selected region.

.. option:: --region <user|service>

   Clock region selector (default: ``user``).

.. option:: -x, --hex

   Print ``--get`` output in hexadecimal.

Rules:

- Exactly one of ``--get`` or ``--set`` must be provided.
- ``--set`` value is in Hz and must be greater than zero.
- ``--hex`` is valid only with ``--get``.
- ``--set`` prints requested and achieved frequencies.
