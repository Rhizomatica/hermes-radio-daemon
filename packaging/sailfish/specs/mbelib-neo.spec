Name:           mbelib-neo
Version:        2.4.0
Release:        1
Summary:        AMBE and IMBE vocoder library
License:        ISC and GPL-2.0-or-later
URL:            https://github.com/arancormonk/mbelib-neo
Source0:        https://github.com/arancormonk/mbelib-neo/archive/refs/tags/v%{version}.tar.gz#/mbelib-neo-%{version}.tar.gz
BuildRequires:  cmake

%description
mbelib-neo decodes and encodes AMBE and IMBE voice frames (D-STAR, DMR, P25).

%package devel
Summary:        Development files for mbelib-neo
Requires:       %{name} = %{version}-%{release}

%description devel
Headers, pkg-config file and linker links for mbelib-neo.

%prep
%setup -q

%build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=%{_prefix} \
      -DCMAKE_INSTALL_LIBDIR=%{_lib}
cmake --build build %{?_smp_mflags}

%install
DESTDIR=%{buildroot} cmake --install build
rm -f %{buildroot}%{_libdir}/*.a
rm -rf %{buildroot}%{_libdir}/cmake %{buildroot}%{_datadir}/doc

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%{_libdir}/libmbe-neo.so.*

%files devel
%{_includedir}/mbelib-neo
%{_libdir}/libmbe-neo.so
%{_libdir}/pkgconfig/libmbe-neo.pc
