..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2026 Advanced Micro Devices, Inc

#############################
Offload Builds to a Cluster
#############################

A full hardware build runs Vivado implementation, bootgen, the AVED firmware
build and, if you have kernels, Vitis HLS. Together those want more memory and
more hours than a laptop or a shared development machine tends to have. SLASH
can hand each of those tool invocations to a batch scheduler instead, while the
orchestration stays where you started it.

The hook is a single environment variable, and SLASH knows nothing about any
particular scheduler.

The contract
============

``SLASH_TOOL_LAUNCHER`` is a **command prefix**. SLASH inserts it in front of
every offloadable tool command line, so a wrapper must:

* run the command it is given, verbatim;
* block until that command exits;
* propagate its exit status, since SLASH treats a non-zero status as a failed
  build step;
* run it in the directory named by ``SLASH_TOOL_CWD``.

Empty, which is the default, runs everything locally exactly as before.

Two environment variables are exported to the wrapper:

``SLASH_BUILD_TASK``
   Which step is being run, so the wrapper can size the reservation.
   One of ``static_shell``, ``static_shell_compute``, ``rm_slash``,
   ``rm_service_layer``, ``bootgen``, ``aved``, or ``hls``.

``SLASH_TOOL_CWD``
   The directory the tool must run in. Several of the tools resolve their
   inputs against the process working directory — bootgen against the paths
   inside a ``.bif``, ``v++`` against ``--work_dir .`` — and schedulers
   reproduce the submission directory only as a courtesy, sometimes silently
   falling back to a temporary directory.

Nothing needs to be installed on the execution host
===================================================

Everything a tool reads has already been written to disk by the time the
command is built, so what the wrapper receives is always a plain vendor tool
invocation: ``vivado``, ``bootgen``, ``v++``, ``vitis-run``, or the ``bash
build_all.sh`` / ``bash build-rp1.sh`` of the AVED tree that SLASH cloned and
populated. An execution host therefore needs only the AMD toolchain and a
mount of the storage the build directory lives on.

The two firmware scripts are the demanding ones, because they drive a
toolchain rather than a single binary: between them they need ``cmake``,
``ninja``, ``make``, ``git``, ``python3``, ``sdtgen``, ``empyro`` and both R5
cross compilers (``armr5-none-eabi-gcc`` for AMC, ``arm-none-eabi-gcc`` for
RP1 — different binaries, in different directories, so having one is not
having the other). All of them ship inside Vitis or the base system;
``scripts/lsf/preflight.sh aved`` checks for them in about five seconds,
which is worth doing before spending an hour finding out.

The interpreter is worth singling out, because presence is not the test.
RP1's platform-config generator needs Python 3.7 or newer, and an execution
host can easily be older than the machine you develop on — RHEL 8 still ships
3.6.8 as ``python3``, where the script dies with a ``SyntaxError`` on its own
first import rather than anything that names the real problem.
``build-rp1.sh`` therefore prefers the interpreter Vitis bundles under
``tps/lnx64/python-*`` over whatever the host calls ``python3``, and the
preflight probe resolves it the same way, so a node it passes is one that
script can build on.

That does mean **everything must be on shared storage**: the checkout, the
build directory, the Python interpreter running SLASH, and the tool
installation. A path that resolves differently on the two hosts is the most
common way this fails, and it fails deep inside a tool rather than at the
point of the mistake.

Using it
========

.. code-block:: bash

   export SLASH_TOOL_LAUNCHER=/path/to/your/submit-wrapper
   python3 -m slashkit link ...

With a launcher set, ``--vivado`` accepts a bare name such as ``vivado``, which
is resolved on the execution host rather than locally. That is what makes a
build possible on a machine with no local Vivado install. Given a path with
directories in it, SLASH still checks it exists, since that is almost always a
typo rather than a remote path.

HLS kernels are built by ``make`` rather than by SLASH, so ``build_hls()``
reads the same variable at CMake configure time:

.. code-block:: bash

   cmake -B build -DSLASH_TOOL_LAUNCHER=/path/to/your/submit-wrapper
   cmake --build build

It is a cache entry and therefore sticky; re-configure with
``-USLASH_TOOL_LAUNCHER`` to go back to building locally.

The base IP cores under
``linker/slashkit/resources/base/common/iprepo``, which ``slashkit install``
expects to be built beforehand, are driven by plain makefiles that read the
same variable straight from the environment:

.. code-block:: bash

   export SLASH_TOOL_LAUNCHER=/path/to/your/submit-wrapper
   make -C linker/slashkit/resources/base/common/iprepo -j2

What still runs locally
=======================

A launcher offloads the vendor tools, not the whole flow. These steps stay on
the machine you invoke SLASH from:

* The ``git clone --recurse-submodules`` of the AVED tree, which needs network
  access, and the ``git`` queries that stamp the build ID into the design. The
  values they produce are carried to the execution host in the environment.
* Emulation builds, which link the testbench and kernels with the local
  ``g++`` against the Vitis headers. ``-p emu`` therefore still needs a Vitis
  installation readable from this machine: set ``XILINX_VITIS`` or put
  ``vitis`` on ``PATH``. Hardware links do not.

A minimal wrapper
=================

The contract is small enough that a wrapper for a scheduler with a blocking
submit mode is a couple of lines:

.. code-block:: bash

   #!/bin/bash
   # SLURM: --wait blocks and returns the job's exit status.
   exec srun --mem "${MEM:-120G}" -c "${CORES:-16}" -t 12:00:00 \
       bash -c 'source /path/to/settings64.sh && cd "$SLASH_TOOL_CWD" && exec "$@"' _ "$@"

Two things are worth getting right from the start:

* Source the **bash** flavour of the settings script, ``settings64.sh``. Batch
  jobs run as non-interactive scripts, and under bash the ``.csh`` flavour
  fails with ``setenv: command not found``. Because the usual idiom is
  ``source ... && vivado ...``, the ``&&`` then short-circuits and the tool
  never runs at all.
* Pass a real argv through rather than building a command string. Schedulers
  that flatten their arguments into a single string hand it to a remote shell
  that parses it *again*, which mangles anything containing spaces, quotes or
  brackets.

A complete, production-shaped example for IBM Spectrum LSF lives in
``scripts/lsf/`` in the SLASH repository. It covers per-task reservation
sizing, a preflight probe that checks a node before you commit hours of queue
time to it, and a two-minute smoke test. All of its cluster-specific values
live in a configuration file outside the repository, which is the pattern to
copy for your own site.
