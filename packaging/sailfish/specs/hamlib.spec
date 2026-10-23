Name:           hamlib
Version:        4.6.2
Release:        1.usbserial1
Summary:        Radio transceiver, receiver and rotator control library
License:        LGPL-2.1-or-later and GPL-2.0-or-later
URL:            https://hamlib.github.io
# Hamlib 4.6.2 with Hamlib PR #2272 backported (USB-serial bridge chips
# driven from user space: "usb:" and "usbfd:" rig pathnames). Made with
# `make dist` from tag 4.6.2 plus hamlib-4.6.2-usb-serial.patch, since the
# patch changes src/Makefile.am and the device has no automake.
Source0:        hamlib-%{version}-usb-serial.tar.gz
BuildRequires:  libusb1-devel

%description
Hamlib controls radios and rotators through one API, with the rigctl and
rigctld tools. This build also drives CP210x USB-serial chips through
usbfs (rig pathname "usb:VVVV:PPPP[:SERIAL]"), for kernels with no
cp210x driver.

%package devel
Summary:        Development files for hamlib
Requires:       %{name} = %{version}-%{release}

%description devel
Headers, pkg-config file and linker links for hamlib.

%prep
%setup -q

%build
./configure --prefix=%{_prefix} --libdir=%{_libdir} --disable-static \
    --without-cxx-binding --without-perl-binding --without-python-binding \
    --without-tcl-binding --without-lua-binding --without-readline \
    --without-xml-support --disable-html-matrix --without-indi
make %{?_smp_mflags}

%install
make install DESTDIR=%{buildroot}
rm -f %{buildroot}%{_libdir}/*.la
rm -rf %{buildroot}%{_mandir} %{buildroot}%{_infodir} %{buildroot}%{_datadir}/doc

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%license COPYING COPYING.LIB
%{_bindir}/*
%{_libdir}/libhamlib.so.*

%files devel
%{_includedir}/hamlib
%{_libdir}/libhamlib.so
%{_libdir}/pkgconfig/hamlib.pc
%{_datadir}/aclocal/hamlib.m4
