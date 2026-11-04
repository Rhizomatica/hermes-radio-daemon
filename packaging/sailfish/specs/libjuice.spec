Name:           libjuice
Version:        1.7.4
Release:        1
Summary:        UDP Interactive Connectivity Establishment library
License:        MPL-2.0
URL:            https://github.com/paullouisageneau/libjuice
# The release tag's tree, as GitHub archives it (fetch-sources.sh); the
# version libdatachannel 0.24.6 pins
Source0:        libjuice-%{version}.tar.gz
BuildRequires:  cmake

%description
libjuice opens bidirectional UDP streams through NAT traversal with ICE
(RFC 8445), as WebRTC does. It is the ICE backend of libdatachannel.

%package devel
Summary:        Development files for libjuice
Requires:       %{name} = %{version}-%{release}

%description devel
Headers, CMake package configuration and linker link for libjuice.

%prep
%setup -q -n libjuice-%{version}

%build
# upstream's tests reach public STUN servers
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=%{_prefix} \
      -DCMAKE_INSTALL_LIBDIR=%{_lib} -DNO_TESTS=ON
cmake --build build %{?_smp_mflags}

%install
DESTDIR=%{buildroot} cmake --install build

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%license LICENSE
%{_libdir}/libjuice.so.*

%files devel
%{_includedir}/juice
%{_libdir}/libjuice.so
%{_libdir}/cmake/LibJuice
