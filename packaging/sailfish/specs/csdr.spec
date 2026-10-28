%global commit 3b6a9a8
Name:           csdr
Version:        0.15
Release:        1
Summary:        DSP library and command-line tool for software defined radio
License:        BSD-2-Clause and GPL-3.0-or-later
URL:            https://github.com/Rhizomatica/csdr
Source0:        csdr-%{commit}.tar.gz
BuildRequires:  fftw-devel

%description
libcsdr, a set of simple DSP routines for software defined radio, with
the csdr and nmux tools. Rhizomatica's tree.

%package devel
Summary:        Development files for libcsdr
Requires:       %{name} = %{version}-%{release}

%description devel
Headers (in include/csdr, as the HERMES packages have them) and linker link.

%prep
%setup -q -n csdr-%{commit}

%build
make libcsdr.so csdr nmux PARSEVECT=no

%install
install -D -m 755 libcsdr.so.%{version} %{buildroot}%{_libdir}/libcsdr.so.%{version}
ln -s libcsdr.so.%{version} %{buildroot}%{_libdir}/libcsdr.so
install -D -m 755 csdr %{buildroot}%{_bindir}/csdr
install -m 755 csdr-fm nmux %{buildroot}%{_bindir}/
install -d %{buildroot}%{_includedir}/csdr
install -m 644 *.h %{buildroot}%{_includedir}/csdr/

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%{_libdir}/libcsdr.so.%{version}
%{_bindir}/*

%files devel
%{_includedir}/csdr
%{_libdir}/libcsdr.so
