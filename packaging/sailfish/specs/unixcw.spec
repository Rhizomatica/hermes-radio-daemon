%global commit 5af118e3
Name:           unixcw
Version:        3.6.1
Release:        1
Summary:        libcw, the Morse code library of unixcw
License:        GPL-2.0-or-later
URL:            https://github.com/Rhizomatica/unixcw
Source0:        unixcw-%{commit}.tar.gz
BuildRequires:  alsa-lib-devel

%description
libcw generates and receives Morse code. Only the library is built, with
ALSA output. Rhizomatica's tree.

%package devel
Summary:        Development files for libcw
Requires:       %{name} = %{version}-%{release}

%description devel
Headers, pkg-config file and linker links for libcw.

%prep
%setup -q -n unixcw-%{commit}

%build
./configure --prefix=%{_prefix} --libdir=%{_libdir} --disable-static \
    --disable-console --disable-oss --disable-pulseaudio \
    --disable-cwgen --disable-cw --disable-cwcp --disable-xcwcp
make %{?_smp_mflags}

%install
make install DESTDIR=%{buildroot}
rm -f %{buildroot}%{_libdir}/*.la
rm -rf %{buildroot}%{_mandir} %{buildroot}%{_datadir}

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%license COPYING
%{_libdir}/libcw.so.*

%files devel
%{_includedir}/libcw*.h
%{_libdir}/libcw.so
%{_libdir}/pkgconfig/libcw.pc
