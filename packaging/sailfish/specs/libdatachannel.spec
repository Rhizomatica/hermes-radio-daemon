%global commit 6b1e2e62
Name:           libdatachannel
Version:        0.24.6
Release:        1
Summary:        WebRTC data channels, media transport and WebSockets
License:        MPL-2.0
URL:            https://libdatachannel.org
# git clone --recursive at the v0.24.6 tag, .git left out (fetch-sources.sh):
# the Sailfish repositories have no usrsctp and only libsrtp 2.3, so the
# libsrtp, usrsctp and plog versions upstream pins are built into the
# library, as upstream does by default. ICE is the system libjuice.
Source0:        libdatachannel-%{commit}.tar.gz
BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  openssl-devel
BuildRequires:  libjuice-devel >= 1.7.4

%description
libdatachannel is a C/C++ implementation of WebRTC: data channels, media
transport over DTLS-SRTP with RTP and RTCP handling, and WebSockets. It
interoperates with browsers, with ICE through libjuice.

%package devel
Summary:        Development files for libdatachannel
Requires:       %{name} = %{version}-%{release}

%description devel
C and C++ headers, CMake package configuration and linker link for
libdatachannel.

%prep
%setup -q -n libdatachannel-%{commit}

%build
# upstream's tests reach public STUN and TURN servers
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=%{_prefix} \
      -DCMAKE_INSTALL_LIBDIR=%{_lib} -DUSE_SYSTEM_JUICE=ON -DUSE_NICE=OFF \
      -DNO_EXAMPLES=ON -DNO_TESTS=ON
cmake --build build %{?_smp_mflags}

%install
DESTDIR=%{buildroot} cmake --install build

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%license LICENSE
%{_libdir}/libdatachannel.so.*

%files devel
%{_includedir}/rtc
%{_libdir}/libdatachannel.so
%{_libdir}/cmake/LibDataChannel
