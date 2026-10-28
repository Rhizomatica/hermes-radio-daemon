Name:           fftw
Version:        3.3.10
Release:        1
Summary:        Fast Fourier transform library (double and single precision)
License:        GPL-2.0-or-later
URL:            https://www.fftw.org
Source0:        https://www.fftw.org/fftw-%{version}.tar.gz

%description
FFTW computes discrete Fourier transforms. This package has the double
(libfftw3) and single precision (libfftw3f) libraries, with threads.

%package devel
Summary:        Development files for fftw
Requires:       %{name} = %{version}-%{release}

%description devel
Headers, pkg-config files and linker links for fftw.

%prep
%setup -q

%build
common="--prefix=%{_prefix} --libdir=%{_libdir} --enable-shared --disable-static --enable-threads --disable-fortran --disable-doc"
mkdir double single
(cd double && ../configure $common && make %{?_smp_mflags})
(cd single && ../configure $common --enable-float --enable-neon && make %{?_smp_mflags})

%install
(cd double && make install DESTDIR=%{buildroot})
(cd single && make install DESTDIR=%{buildroot})
rm -f %{buildroot}%{_libdir}/*.la
rm -rf %{buildroot}%{_infodir} %{buildroot}%{_mandir} %{buildroot}%{_libdir}/cmake

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%license COPYING
%{_libdir}/libfftw3*.so.*
%{_bindir}/*

%files devel
%{_includedir}/fftw3*
%{_libdir}/libfftw3*.so
%{_libdir}/pkgconfig/fftw3*.pc
