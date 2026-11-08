%global commit 57fb7a15
%global gitdate 20261108
# git rev-list --count %{commit}: it grows along the branch, so a later
# commit always sorts newer, even on the same day (the hash alone may not)
%global gitcount 1432
# The Khronos EGL, GLES2 and KHR headers mercury-ui builds against
# (Sailfish carries the libraries, through libhybris, but not the headers)
%global egl_registry db3425b8246136faccb5e2782b5694960bd6edf1
%global gl_registry 6af574a14089ccfee87efe230ebcdd8742859813
# mercury-ui (the Fyne GUI) when a Go of the UI's go.mod version or later is
# on the PATH; --define 'have_go 0' or 1 decides it outright.
%{!?have_go:%global have_go %(command -v go >/dev/null 2>&1 && echo 1 || echo 0)}
Name:           mercury
# A snapshot of mercuryv2 past the 1.9.22 tag, for mercury-ui: the tag
# predates sailfish/build-mercury-ui.sh and the Wayland/GLES2 build tags.
# Back to the release tag's own spec (sailfish/mercury.spec in the tag
# archive) once a release carries them.
Version:        1.9.22+git%{gitdate}.%{gitcount}.%{commit}
Release:        1
Summary:        HF modem for store-and-forward communication
License:        GPL-3.0-or-later
URL:            https://github.com/Rhizomatica/mercury
# git clone of mercuryv2 at %{commit}, .git left out (fetch-sources.sh)
Source0:        mercury-%{commit}.tar.gz
Source1:        khronos-egl-%{egl_registry}.tar.gz
Source2:        khronos-gl-%{gl_registry}.tar.gz
# The app launcher entry, and mercury's icon (gui_interface/fyne-ui/
# mercury-ui.png) at the launcher's sizes
Source3:        mercury-ui.desktop
Source4:        mercury-ui-86.png
Source5:        mercury-ui-108.png
Source6:        mercury-ui-128.png
Source7:        mercury-ui-172.png
Source8:        mercury-ui-256.png
BuildRequires:  hamlib-devel
BuildRequires:  alsa-lib-devel
BuildRequires:  pulseaudio-devel
BuildRequires:  openssl-devel
%if %{have_go}
BuildRequires:  wayland-devel
BuildRequires:  libxkbcommon-devel
%endif

%description
Mercury is the modem of HERMES: ARQ and broadcast data links over HF
radio, with a VARA-compatible TCP TNC interface. This is the standalone
modem, with its own audio and its own radio control through Hamlib; radiod
has mercury built in and does not need it. No GUI.

%if %{have_go}
%package ui
Summary:        Mercury's graphical interface, with the modem built in

%description ui
mercury-ui is Mercury's Fyne GUI, with the modem engine linked in: it does
not need the mercury package. A technology preview on Sailfish OS
(Wayland, GLES2 through libhybris), in the app list as Mercury.
%endif

%prep
%setup -q -n mercury-%{commit}
%if %{have_go}
mkdir -p khronos
tar -xzf %{SOURCE1} -C khronos
tar -xzf %{SOURCE2} -C khronos
%endif

%build
%if %{have_go}
# The UI first: its engine objects are built -fPIE (the UI links as PIE),
# then cleaned away, so the standalone modem builds from its own.
GIT_HASH=%{commit} sh sailfish/build-mercury-ui.sh "$PWD/khronos"
mv mercury-ui ../mercury-ui-%{commit}
make clean >/dev/null
%endif
make %{?_smp_mflags} MERCURY_HOST=standalone GIT_HASH=%{commit}

%install
make install DESTDIR=%{buildroot} prefix=%{_prefix} MERCURY_HOST=standalone \
     GIT_HASH=%{commit}
%if %{have_go}
install -D -m 755 ../mercury-ui-%{commit} %{buildroot}%{_bindir}/mercury-ui
install -D -m 644 %{SOURCE3} %{buildroot}%{_datadir}/applications/mercury-ui.desktop
for size in 86 108 128 172 256; do
    install -D -m 644 %{_sourcedir}/mercury-ui-$size.png \
        %{buildroot}%{_datadir}/icons/hicolor/${size}x${size}/apps/mercury-ui.png
done
%endif

%check
make test MERCURY_HOST=standalone GIT_HASH=%{commit}

%files
%license LICENSE
%{_bindir}/mercury
%{_mandir}/man1/mercury.1*
%doc %{_docdir}/mercury/mercury.ini.example

%if %{have_go}
%files ui
%license LICENSE
%{_bindir}/mercury-ui
%{_datadir}/applications/mercury-ui.desktop
%{_datadir}/icons/hicolor/*/apps/mercury-ui.png
%endif
