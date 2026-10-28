Name:           iniparser
Version:        4.2.6
Release:        1
Summary:        INI file reader and writer
License:        MIT
URL:            https://gitlab.com/iniparser/iniparser
Source0:        https://gitlab.com/iniparser/iniparser/-/archive/v%{version}/iniparser-v%{version}.tar.gz
BuildRequires:  cmake

%description
iniParser is a C library that parses INI files and writes them back.

%package devel
Summary:        Development files for iniparser
Requires:       %{name} = %{version}-%{release}

%description devel
Headers (in include/iniparser, as Debian has them) and linker links.

%prep
%setup -q -n iniparser-v%{version}

%build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=%{_prefix} \
      -DCMAKE_INSTALL_LIBDIR=%{_lib} -DBUILD_STATIC_LIBS=OFF -DBUILD_SHARED_LIBS=ON \
      -DBUILD_DOCS=OFF -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF
cmake --build build %{?_smp_mflags}

%install
DESTDIR=%{buildroot} cmake --install build
rm -rf %{buildroot}%{_datadir}/doc %{buildroot}%{_libdir}/cmake

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%license LICENSE
%{_libdir}/libiniparser.so.*

%files devel
%{_includedir}/iniparser
%{_libdir}/libiniparser.so
%{_libdir}/pkgconfig/iniparser.pc
