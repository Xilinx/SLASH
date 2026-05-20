..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

################
Configuration
################

The ``vrtd`` daemon reads its configuration from ``vrtd.conf`` at startup.
The file uses an INI-style format with sections for roles, users, and groups.

File Location
=============

The default configuration file is installed alongside the ``vrtd`` binary.
Additional configuration fragments can be included via the ``include-glob``
directive at the top of the file:

.. code-block:: ini

   include-glob = vrtd.conf.d/*.conf

This loads all ``.conf`` files in the ``vrtd.conf.d/`` directory, allowing
drop-in configuration without editing the main file.

Roles
=====

A role defines a set of permissions. Roles are declared with
``[role:<name>]`` sections for global permissions and
``[role:<name>:<device>]`` sub-sections for per-device permissions.

The ``<device>`` specifier can be a BDF address or the ``any`` wildcard to
match all devices.

Built-in Roles
--------------

The default configuration defines two roles:

**fullaccess** — grants all permissions on all devices:

.. code-block:: ini

   [role:fullaccess]
   query-devices = yes

   [role:fullaccess:any]
   bar-access = full
   qdma = yes
   buffer = yes
   design-write = yes
   clock = yes
   pcie-hotplug = yes

**info** — can enumerate and query devices but not access them:

.. code-block:: ini

   [role:info]
   query-devices = yes

Permission Keys
---------------

The following permission keys are available in per-device sub-sections:

.. list-table::
   :header-rows: 1
   :widths: 25 50

   * - Key
     - Description
   * - ``query-devices``
     - Enumerate devices and read device info (global, not per-device).
   * - ``bar-access``
     - BAR MMIO access level. Values: ``full``, or omit to deny.
   * - ``qdma``
     - Allow QDMA (DMA transfer) operations.
   * - ``buffer``
     - Allow device buffer allocation.
   * - ``design-write``
     - Allow programming (loading vrtbin onto device).
   * - ``clock``
     - Allow clock frequency configuration.
   * - ``pcie-hotplug``
     - Allow PCIe hotplug operations (reset, remove, rescan).

User and Group Mappings
=======================

Users and groups are assigned roles with ``[user:<name>]`` and
``[group:<name>]`` sections. The wildcard ``*`` matches any user or group.

Default mappings:

.. code-block:: ini

   [user:root]
   role = fullaccess

   [group:vrtadmin]
   role = fullaccess

   [user:*]
   role = info

This gives ``root`` and members of the ``vrtadmin`` group full access, while
all other users get read-only device enumeration.

Custom Roles
============

You can define custom roles to grant fine-grained access. For example, to allow
a role that can run kernels but not reprogram the FPGA:

.. code-block:: ini

   [role:runner]
   query-devices = yes

   [role:runner:any]
   bar-access = full
   qdma = yes
   buffer = yes

   [group:fpga-users]
   role = runner
