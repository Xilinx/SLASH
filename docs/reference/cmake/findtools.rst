..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

##############################
FindVivado and FindVitis
##############################

SLASH provides two CMake find modules for locating AMD Vivado and Vitis HLS
installations. ``FindVivado`` is used internally by ``SlashTools.cmake``.
Both modules can also be included directly.

FindVivado
==========

Locates an AMD Vivado installation for FPGA synthesis and implementation.

Search Order
------------

1. ``VIVADO_ROOT_DIR`` CMake variable.
2. ``XILINX_VIVADO`` environment variable.
3. System ``PATH`` (searches for ``vivado`` in ``bin/`` subdirectories).

Variables Set
-------------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Variable
     - Description
   * - ``VIVADO_FOUND``
     - ``TRUE`` if Vivado was located.
   * - ``VIVADO_ROOT_DIR``
     - Root directory of the Vivado installation.
   * - ``VIVADO_BINARY``
     - Full path to the ``vivado`` executable.
   * - ``VIVADO_PATH``
     - Path to the ``bin/`` directory containing the Vivado binary.

Usage
-----

``FindVivado`` is included automatically by ``SlashTools.cmake`` — no
manual ``include()`` is needed in most projects.

Source the Vivado environment before running CMake so that ``vivado`` is on
``PATH``:

.. code-block:: bash

   source <path-to-vivado>/settings64.sh

For ``csh``/``tcsh`` shells, use ``settings64.csh`` instead. SLASH has been
built and tested against **Vivado 2025.1**; using other versions may cause
breakage.

Error Behaviour
---------------

``FindVivado`` issues a ``FATAL_ERROR`` if the ``vivado`` binary cannot be
found.

FindVitis
=========

Locates an AMD Vitis HLS installation for kernel compilation.

Search Order
------------

1. ``VITIS_ROOT_DIR`` CMake variable.
2. ``XILINX_VITIS`` environment variable.
3. ``VITIS_HOME`` environment variable.
4. ``VITIS`` environment variable.
5. System ``PATH`` (searches for ``vitis`` in ``bin/`` subdirectories).

Variables Set
-------------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Variable
     - Description
   * - ``VITIS_FOUND``
     - ``TRUE`` if Vitis was located.
   * - ``VITIS_BINARY``
     - Full path to the ``vitis`` executable.
   * - ``VITIS_ROOT_DIR``
     - Root directory of the Vitis installation.
   * - ``VITIS_INCLUDE_DIR``
     - Path to the Vitis include directory (e.g. for ``ap_fixed.h``,
       ``hls_stream.h``). Validated to exist.

Usage
-----

``FindVitis`` can be included manually if your project needs the Vitis root
or include directories. ``BuildHLS.cmake`` locates the ``v++`` and
``vitis-run`` executables directly and does not require this module.

Source the Vitis HLS environment before running CMake so that ``vitis`` is
on ``PATH``:

.. code-block:: bash

   source <path-to-vitis-hls>/settings64.sh

For ``csh``/``tcsh`` shells, use ``settings64.csh`` instead. SLASH has been
built and tested against **Vitis HLS 2025.1**; using other versions may
cause breakage.

Error Behaviour
---------------

``FindVitis`` issues a ``FATAL_ERROR`` if:

- The ``vitis`` binary cannot be found.
- The include directory (``${VITIS_ROOT_DIR}/include``) does not exist.
