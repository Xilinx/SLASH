# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
# 
#  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
#  and associated documentation files (the "Software"), to deal in the Software without restriction,
#  including without limitation the rights to use, copy, modify, merge, publish, distribute,
#  sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
#  furnished to do so, subject to the following conditions:
# 
#  The above copyright notice and this permission notice shall be included in all copies or
#  substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
# NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
# ##################################################################################################

%global debug_package %{nil}
%global dkms_name slash
%global dkms_version %{version}

Name:           slash
Version:        %{_version}
Release:        1%{?dist}
Summary:        SLASH/VRT System
License:        MIT
URL:            https://github.com/Xilinx/SLASH

Source0:        %{name}-%{version}.tar.gz

BuildRequires:  bash
BuildRequires:  cmake
BuildRequires:  make
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  ninja-build
BuildRequires:  pkg-config
BuildRequires:  cli11-devel
BuildRequires:  cppzmq-devel
BuildRequires:  inih-devel
BuildRequires:  jsoncpp-devel
BuildRequires:  libxml2-devel
BuildRequires:  systemd-devel
BuildRequires:  zeromq-devel
BuildRequires:  zlib-devel
BuildRequires:  rsync
BuildRequires:  python3
BuildRequires:  python3-devel
BuildRequires:  python3-jinja2
BuildRequires:  python3-pip
BuildRequires:  python3-setuptools
BuildRequires:  python3-wheel
BuildRequires:  systemd-rpm-macros

# ---- Metapackages ----

%description
SLASH/VRT System Full

%package -n     slash-devel
Summary:        SLASH/VRT System Full (development files)
Requires:       slash-sim-emu-devel = %{version}-%{release}
Requires:       libslash-devel = %{version}-%{release}
Requires:       libvrtd-devel = %{version}-%{release}
BuildArch:      noarch

%description -n slash-devel
SLASH/VRT System Full (development files)

%package -n     slash-sim-emu
Summary:        SLASH/VRT System for simulation and emulation
Requires:       libvrt = %{version}-%{release}
BuildArch:      noarch

%description -n slash-sim-emu
SLASH/VRT System for simulation and emulation

%package -n     slash-sim-emu-devel
Summary:        SLASH/VRT System for simulation and emulation (development files)
Requires:       slash-sim-emu = %{version}-%{release}
Requires:       slashkit = %{version}-%{release}
Requires:       libvrt-devel = %{version}-%{release}
BuildArch:      noarch

%description -n slash-sim-emu-devel
SLASH/VRT System for simulation and emulation (development files)

%package -n     slash-dkms
Summary:        SLASH kernel module (DKMS)
Requires:       dkms, gcc, make
BuildArch:      noarch

%description -n slash-dkms
SLASH kernel module (DKMS)

# ---- Libraries ----

%package -n     libslash
Summary:        Library for interacting with the SLASH kernel module

%description -n libslash
Library for interacting with the SLASH kernel module

%package -n     libslash-devel
Summary:        Library for interacting with the SLASH kernel module (development files)
Requires:       libslash = %{version}-%{release}

%description -n libslash-devel
Library for interacting with the SLASH kernel module (development files)

%package -n     vrtd
Summary:        VRTd daemon for managing VRT devices
Requires:       libslash = %{version}-%{release}
%{?systemd_requires}

%description -n vrtd
VRTd daemon for managing VRT devices

%package -n     libvrtd
Summary:        Library for interacting with the VRTd daemon
Requires:       libslash = %{version}-%{release}

%description -n libvrtd
Library for interacting with the VRTd daemon for managing VRT devices

%package -n     libvrtd-devel
Summary:        Library for interacting with the VRTd daemon (development files)
Requires:       libvrtd = %{version}-%{release}
Requires:       libslash-devel = %{version}-%{release}

%description -n libvrtd-devel
Library for interacting with the VRTd daemon for managing VRT devices (development files)

%package -n     libvrt
Summary:        VRT Runtime
Requires:       libvrtd = %{version}-%{release}
Requires:       systemd

%description -n libvrt
VRT Runtime

%package -n     libvrt-devel
Summary:        VRT Runtime (development files)
Requires:       libvrt = %{version}-%{release}
Requires:       libvrtd-devel = %{version}-%{release}
Requires:       jsoncpp-devel
Requires:       libxml2-devel
Requires:       zeromq-devel
Requires:       zlib-devel

%description -n libvrt-devel
VRT Runtime (development files)

%package -n     v80-smi
Summary:        V80 System Management Interface
Requires:       libvrt = %{version}-%{release}

%description -n v80-smi
V80 System Management Interface

%package -n     slashkit
Summary:        SLASH Linker
Requires:       python3
Requires:       python3-jinja2
Requires:       cppzmq-devel

%description -n slashkit
SLASH Linker

# ---- Build ----

%prep
%autosetup -n %{name}-%{version}

%build

bash scripts/pconfigure.sh %{_lib}
bash scripts/pbuild.sh

%install
bash scripts/pinstall.sh %{buildroot}

# systemd units (mirrors debian/rules rsync lines)
install -D -m 0644 vrt/vrtd/systemd/vrtd.service \
    %{buildroot}%{_unitdir}/vrtd.service
install -D -m 0644 vrt/vrtd/systemd/vrtd.socket \
    %{buildroot}%{_unitdir}/vrtd.socket

# sysusers (mirrors debian/vrtd.install + vrtd.postinst)
install -D -m 0644 vrt/vrtd/sysusers/vrtd.conf \
    %{buildroot}%{_sysusersdir}/vrtd.conf

# vrtd config and drop-in directory
install -D -m 0644 vrt/vrtd/conf/vrtd.conf \
    %{buildroot}%{_sysconfdir}/vrt/vrtd.conf
install -d %{buildroot}%{_sysconfdir}/vrt/vrtd.conf.d

# udev rules (mirrors debian/vrtd.udev)
install -D -m 0644 vrt/vrtd/udev/99-vrtd.rules \
    %{buildroot}%{_udevrulesdir}/99-vrtd.rules

# DKMS source tree (mirrors debian/slash-dkms.install exactly)
install -d %{buildroot}%{_usrsrc}/%{dkms_name}-%{dkms_version}/driver/libslash/include/slash

install -m 0644 driver/*.c      %{buildroot}%{_usrsrc}/%{dkms_name}-%{dkms_version}/driver/
install -m 0644 driver/*.h      %{buildroot}%{_usrsrc}/%{dkms_name}-%{dkms_version}/driver/
install -m 0644 driver/Makefile %{buildroot}%{_usrsrc}/%{dkms_name}-%{dkms_version}/driver/

cp -a driver/kcompat %{buildroot}%{_usrsrc}/%{dkms_name}-%{dkms_version}/driver/

cp -a driver/libslash/include/slash/uapi \
    %{buildroot}%{_usrsrc}/%{dkms_name}-%{dkms_version}/driver/libslash/include/slash/

cp -a submodules/qdma_drv/QDMA/linux-kernel/driver/libqdma \
    %{buildroot}%{_usrsrc}/%{dkms_name}-%{dkms_version}/driver/

# DKMS config (equivalent to debian/slash-dkms.dkms)
cat > %{buildroot}%{_usrsrc}/%{dkms_name}-%{dkms_version}/dkms.conf << 'EOF'
PACKAGE_NAME="slash"
PACKAGE_VERSION="%{dkms_version}"

BUILT_MODULE_NAME[0]="slash"
BUILT_MODULE_LOCATION[0]="driver"
DEST_MODULE_LOCATION[0]="/updates/dkms"
AUTOINSTALL="yes"

MAKE[0]="make -C driver KDIR=/lib/modules/${kernelver}/build SLASH_VERSION=${PACKAGE_VERSION}"
CLEAN="make -C driver KDIR=/lib/modules/${kernelver}/build clean"
EOF

# ---- File lists ----
# You must list every file each subpackage owns.
# Adjust these globs to match your actual installed files.

%files
# metapackage — empty

%files -n slash-devel
# metapackage — empty

%files -n slash-sim-emu
# metapackage — empty

%files -n slash-sim-emu-devel
# metapackage — empty

%files -n slash-dkms
%{_prefix}/src/%{dkms_name}-%{dkms_version}/

%files -n libslash
%{_libdir}/libslash.so
%{_libdir}/libslash.so.*

%files -n libslash-devel
%{_includedir}/slash/
%{_libdir}/cmake/slash/

%pre -n vrtd
%sysusers_create_package vrtd vrt/vrtd/sysusers/vrtd.conf

%post -n vrtd
%systemd_post vrtd.service vrtd.socket
udevadm control --reload-rules && udevadm trigger 2>/dev/null || :

%preun -n vrtd
%systemd_preun vrtd.service vrtd.socket

%postun -n vrtd
%systemd_postun_with_restart vrtd.service vrtd.socket
udevadm control --reload-rules && udevadm trigger 2>/dev/null || :

%files -n vrtd
%{_bindir}/vrtd
%{_bindir}/vrtd-*
%{_unitdir}/vrtd.service
%{_unitdir}/vrtd.socket
%{_udevrulesdir}/99-vrtd.rules
%{_sysusersdir}/vrtd.conf
%config(noreplace) %{_sysconfdir}/vrt/vrtd.conf
%dir %{_sysconfdir}/vrt/vrtd.conf.d

%files -n libvrtd
%{_libdir}/libvrtd.so
%{_libdir}/libvrtd.so.*
%{_libdir}/libvrtdpp.so
%{_libdir}/libvrtdpp.so.*

%files -n libvrtd-devel
%{_includedir}/vrtd/
%{_libdir}/cmake/vrtd/

%files -n libvrt
%{_libdir}/libvrt.so
%{_libdir}/libvrt.so.*

%files -n libvrt-devel
%{_includedir}/vrt/
%{_libdir}/cmake/vrt/

%files -n v80-smi
%{_bindir}/v80-smi

%files -n slashkit
%{_bindir}/slashkit
%{python3_sitelib}/slashkit/
%{python3_sitelib}/slashkit-*.dist-info/
%{_libdir}/cmake/SlashTools/

# ---- Scriptlets ----

%post -n slash-dkms
dkms add -m %{dkms_name} -v %{dkms_version} --rpm_safe_upgrade
dkms build -m %{dkms_name} -v %{dkms_version}
dkms install -m %{dkms_name} -v %{dkms_version}

%preun -n slash-dkms
dkms remove -m %{dkms_name} -v %{dkms_version} --all --rpm_safe_upgrade

%post -n libslash -p /sbin/ldconfig
%postun -n libslash -p /sbin/ldconfig

%post -n libvrtd -p /sbin/ldconfig
%postun -n libvrtd -p /sbin/ldconfig

%post -n libvrt -p /sbin/ldconfig
%postun -n libvrt -p /sbin/ldconfig

%changelog
* Thu Jun 12 2025 Vlad-Gabriel Serbu <Vlad-Gabriel.Serbu@amd.com> - %{_version}-1
- Initial RPM packaging
