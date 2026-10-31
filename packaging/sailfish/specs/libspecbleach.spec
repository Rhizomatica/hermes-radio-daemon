%global commit ec808ce
%global gitdate 20261023
Name:           libspecbleach
Version:        0.4.1~git%{gitdate}.%{commit}
Release:        1
Summary:        Voice noise reduction library
License:        LGPL-2.1-or-later
URL:            https://github.com/Rhizomatica/libspecbleach
# Rhizomatica's fork at the commit the HERMES Debian package has (upstream
# f63b64b, the 0.4 API radiod uses). git clone at the commit, .git left out
# (fetch-sources.sh).
Source0:        libspecbleach-%{commit}.tar.gz
BuildRequires:  cmake
BuildRequires:  fftw-devel

%description
libspecbleach reduces noise in speech by spectral subtraction.

%package devel
Summary:        Development files for libspecbleach
Requires:       %{name} = %{version}-%{release}

%description devel
Headers, pkg-config file and linker link for libspecbleach.

%prep
%setup -q -n libspecbleach-%{commit}

%build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=%{_prefix} \
      -DCMAKE_INSTALL_LIBDIR=%{_lib} -DBUILD_SHARED_LIBS=ON -DENABLE_AVX=OFF \
      -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF -DSPECBLEACH_BUILD_EXTRAS=OFF
cmake --build build %{?_smp_mflags}

%install
DESTDIR=%{buildroot} cmake --install build
rm -rf %{buildroot}%{_libdir}/cmake

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%license LICENSE
%{_libdir}/libspecbleach.so.*

%files devel
%{_includedir}/specbleach*
%{_libdir}/libspecbleach.so
%{_libdir}/pkgconfig/specbleach.pc
