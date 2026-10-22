Name:           i2c-tools
Version:        4.4
Release:        1
Summary:        I2C tools and the libi2c SMBus library
License:        GPL-2.0-or-later and LGPL-2.1-or-later
URL:            https://i2c.wiki.kernel.org/index.php/I2C_Tools
Source0:        https://mirrors.edge.kernel.org/pub/software/utils/i2c-tools/i2c-tools-%{version}.tar.xz

%description
Tools for I2C buses, and libi2c, the SMBus helper library.

%package devel
Summary:        Development files for libi2c
Requires:       %{name} = %{version}-%{release}

%description devel
The i2c/smbus.h header and the libi2c linker link.

%prep
%setup -q

%build
make %{?_smp_mflags} CFLAGS="%{optflags}" BUILD_STATIC_LIB=0 EXTRA=""

%install
make install DESTDIR=%{buildroot} PREFIX=%{_prefix} libdir=%{_libdir} BUILD_STATIC_LIB=0 EXTRA=""
rm -rf %{buildroot}%{_mandir}

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%license COPYING COPYING.LGPL
%{_sbindir}/*
%{_bindir}/*
%{_libdir}/libi2c.so.*

%files devel
%{_includedir}/i2c
%{_libdir}/libi2c.so
