..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

######################
vrtd Configuration
######################

This tutorial walks through configuring the ``vrtd`` daemon — the V80 Runtime
Daemon that multiplexes access to FPGA devices and enforces role-based
permissions. By the end you will know how to manage roles, assign users, and
integrate with systemd.

Prerequisites
=============

- The SLASH platform is set up (kernel module, libraries, ``vrtd`` installed).
  See :doc:`platform-setup`.
- Root or ``sudo`` access for editing configuration and restarting the daemon.

How vrtd Manages Access
========================

All VRT operations — programming a device, allocating buffers, launching
kernels — go through ``vrtd`` via a Unix domain socket at
``/run/vrtd.sock``. The daemon authenticates the connecting user and checks
their role before allowing each operation.

This makes multi-tenant deployments possible: several users or applications can
share the same FPGA boards, each with different privilege levels.

Configuration File
===================

``vrtd`` reads its configuration at startup from ``vrtd.conf``, located
alongside the ``vrtd`` binary (typically ``/etc/vrt/vrtd.conf``). The file
uses an INI-style format.

The first line enables drop-in fragments:

.. code-block:: ini

   include-glob = vrtd.conf.d/*.conf

Any ``.conf`` file placed in the ``vrtd.conf.d/`` directory is loaded
automatically. This lets you add custom roles and user mappings without
editing the main configuration.

Understanding Roles
====================

A **role** defines a set of permissions. The default configuration ships with
two roles.

fullaccess
----------

Grants all permissions on all devices:

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

info
----

Can enumerate and query devices but not access them:

.. code-block:: ini

   [role:info]
   query-devices = yes

Permission Keys
---------------

.. list-table::
   :header-rows: 1
   :widths: 25 50

   * - Key
     - Description
   * - ``query-devices``
     - Enumerate devices and read device info. Set in the ``[role:<name>]``
       section (global, not per-device).
   * - ``bar-access``
     - BAR MMIO access level. Values: ``full`` or omit to deny.
   * - ``qdma``
     - Allow QDMA (DMA transfer) operations.
   * - ``buffer``
     - Allow device buffer allocation.
   * - ``design-write``
     - Allow programming (loading a vrtbin onto a device).
   * - ``clock``
     - Allow clock frequency changes.
   * - ``pcie-hotplug``
     - Allow PCIe hotplug operations (reset, remove, rescan).

Per-device permissions go in ``[role:<name>:<device>]`` sub-sections, where
``<device>`` is a BDF address or the ``any`` wildcard.

User and Group Mappings
========================

Users and groups are assigned roles with ``[user:<name>]`` and
``[group:<name>]`` sections. The default mappings are:

.. code-block:: ini

   [user:root]
   role = fullaccess

   [group:vrtadmin]
   role = fullaccess

   [user:*]
   role = info

This gives ``root`` and members of the ``vrtadmin`` group full access, while
all other users receive read-only enumeration.

To grant a user full access, add them to the ``vrtadmin`` group:

.. code-block:: bash

   sudo usermod -aG vrtadmin <username>

The user must log out and back in for the new group membership to take effect.

Creating a Custom Role
========================

Suppose you want a **runner** role that can execute kernels but cannot
reprogram the FPGA or change the clock. Create a drop-in file
``vrtd.conf.d/runner.conf``:

.. code-block:: ini

   [role:runner]
   query-devices = yes

   [role:runner:any]
   bar-access = full
   qdma = yes
   buffer = yes

   [group:fpga-users]
   role = runner

Members of the ``fpga-users`` group can now allocate buffers and run kernels,
but ``design-write``, ``clock``, and ``pcie-hotplug`` are denied.

Per-Device Permissions
========================

You can restrict a user to a specific board by using a BDF instead of ``any``:

.. code-block:: ini

   [role:lab-board1]
   query-devices = yes

   [role:lab-board1:03:00]
   bar-access = full
   qdma = yes
   buffer = yes
   design-write = yes
   clock = yes

   [user:labuser]
   role = lab-board1

The user ``labuser`` can only access device ``03:00``. Operations targeting any
other board will be denied.

Systemd Integration
====================

``vrtd`` is managed by two systemd units: a socket unit that creates the
listening socket and a service unit that runs the daemon.

Socket Unit
-----------

The socket unit (``vrtd.socket``) creates the Unix socket before ``vrtd``
starts:

.. code-block:: ini

   [Socket]
   ListenSequentialPacket=/run/vrtd.sock
   FileDescriptorName=api
   SocketMode=0666
   SocketGroup=vrt
   RemoveOnStop=yes

``SocketMode=0666`` allows any local user to connect. Access control is then
enforced by ``vrtd``'s role system after authentication.

Service Unit
------------

The service unit (``vrtd.service``) runs the daemon under a dedicated
``vrtd`` user with security hardening:

.. code-block:: ini

   [Service]
   Type=notify
   ExecStart=/usr/lib/vrt/vrtd
   User=vrtd
   Group=vrtd
   WatchdogSec=60s
   Restart=on-failure
   RestartSec=2s

   # Hardening
   NoNewPrivileges=true
   ProtectSystem=full
   ProtectHome=true
   PrivateTmp=true

The daemon uses ``sd_notify`` to signal readiness and integrates with the
systemd watchdog for automatic restart on failure.

Enabling the Service
---------------------

.. code-block:: bash

   sudo systemctl enable --now vrtd.socket
   sudo systemctl enable --now vrtd

Verify that the daemon is running and boards are visible:

.. code-block:: bash

   v80-smi list

Reloading Configuration
-------------------------

Configuration is read at startup. After editing ``vrtd.conf`` or adding
drop-in files, restart the daemon:

.. code-block:: bash

   sudo systemctl restart vrtd

Multi-Tenancy
==============

With roles and per-device permissions in place, ``vrtd`` enables multiple users
and applications to share FPGA devices safely:

- Roles control which operations each user can perform.
- Per-device permissions allow partitioning boards across teams.
- The ``vrtadmin`` group provides a convenient way to grant full access to
  administrators without editing configuration files.

Troubleshooting
================

Check daemon status:

.. code-block:: bash

   systemctl status vrtd

View logs:

.. code-block:: bash

   journalctl -u vrtd

Common issues:

- **VRTD_RET_AUTH_ERROR** in application output — the user's role lacks a
  required permission. Check their role assignment and group membership.
- **vrtd not running** — ensure ``vrtd.socket`` is enabled and start ``vrtd``
  manually with ``sudo systemctl start vrtd``.
- **Group membership not taking effect** — the user must log out and back in
  after being added to a group. Verify with ``groups <username>``.

Next Steps
==========

- :doc:`/reference/vrtd/configuration` — full configuration reference.
- :doc:`/reference/vrtd/client-flow` — how applications communicate with
  ``vrtd``.
- :doc:`platform-setup` — initial platform installation.
- :doc:`device-management` — day-to-day device management.
