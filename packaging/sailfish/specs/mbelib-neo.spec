%global commit a8e56d7
%global gitdate 20261031
Name:           mbelib-neo
Version:        2.6.5~git%{gitdate}.%{commit}
Release:        1
Summary:        AMBE and IMBE vocoder library
License:        ISC and GPL-2.0-or-later
URL:            https://github.com/Rhizomatica/mbelib-neo
# Rhizomatica's fork: upstream v2.6.5 with the PFFFT it pins vendored in
# third_party/, so the build fetches nothing (branch upstream-2.6.5). git
# clone at the commit, .git left out (fetch-sources.sh).
Source0:        mbelib-neo-%{commit}.tar.gz
BuildRequires:  cmake

%description
mbelib-neo decodes and encodes AMBE and IMBE voice frames (D-STAR, DMR, P25).

%package devel
Summary:        Development files for mbelib-neo
Requires:       %{name} = %{version}-%{release}

%description devel
Headers, pkg-config file and linker links for mbelib-neo.

%prep
%setup -q -n mbelib-neo-%{commit}

%build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=%{_prefix} \
      -DCMAKE_INSTALL_LIBDIR=%{_lib} -DMBELIB_BUILD_TESTS=OFF \
      -DMBELIB_BUILD_EXAMPLES=OFF -DMBELIB_WARNINGS_AS_ERRORS=OFF
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
