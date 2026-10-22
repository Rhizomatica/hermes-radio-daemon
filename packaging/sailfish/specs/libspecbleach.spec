Name:           libspecbleach
Version:        0.3.0
Release:        1
Summary:        Voice noise reduction library
License:        LGPL-2.1-or-later
URL:            https://github.com/lucianodato/libspecbleach
Source0:        https://github.com/lucianodato/libspecbleach/archive/refs/tags/v%{version}.tar.gz#/libspecbleach-%{version}.tar.gz
BuildRequires:  cmake
BuildRequires:  fftw-devel

%description
libspecbleach reduces noise in speech by spectral subtraction.

%package devel
Summary:        Development files for libspecbleach
Requires:       %{name} = %{version}-%{release}

%description devel
Headers and the linker link for libspecbleach.

%prep
%setup -q

%build
# upstream has no install rules: built shared, files copied in below
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DENABLE_AVX=OFF \
      -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF
cmake --build build %{?_smp_mflags}

%install
install -D -m 755 build/libspecbleach.so %{buildroot}%{_libdir}/libspecbleach.so
install -d %{buildroot}%{_includedir}
install -m 644 include/specbleach_*.h %{buildroot}%{_includedir}/

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%license LICENSE
%{_libdir}/libspecbleach.so

%files devel
%{_includedir}/specbleach_*.h
