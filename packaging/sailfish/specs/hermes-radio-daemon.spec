%global commit c99915f
%global gitdate 20261105
# git rev-list --count %{commit}: it grows along the branch, so a later
# commit always sorts newer, even on the same day (the hash alone may not)
%global gitcount 481
# The Go API layer (/api/v2, hermes.v1: what the browser pages and WebRTC
# audio use) when a Go of api/go.mod's version or later is on the PATH;
# --define 'have_go 0' or 1 decides it outright.
%{!?have_go:%global have_go %(command -v go >/dev/null 2>&1 && echo 1 || echo 0)}
Name:           hermes-radio-daemon
Version:        0~git%{gitdate}.%{gitcount}.%{commit}
Release:        1
Summary:        HERMES radio controller daemon (radiod), with the mercury modem
License:        GPL-3.0-or-later
URL:            https://github.com/Rhizomatica/hermes-radio-daemon
# git clone --recursive at %{commit}, .git left out (fetch-sources.sh)
Source0:        hermes-radio-daemon-%{commit}.tar.gz
Source1:        radiod-sailfish.service
BuildRequires:  hamlib-devel
BuildRequires:  iniparser-devel
BuildRequires:  alsa-lib-devel
BuildRequires:  openssl-devel
BuildRequires:  fftw-devel
BuildRequires:  i2c-tools-devel
BuildRequires:  csdr-devel
BuildRequires:  libspecbleach-devel
BuildRequires:  mbelib-neo-devel
# WebRTC browser audio (rtc.offer, over hermes.v1: the Go layer)
BuildRequires:  libdatachannel-devel
BuildRequires:  opus-devel

%description
radiod controls a HERMES station's radio (Hamlib rigs and the sBitx),
its audio, digital modes and digital voice, and runs the mercury HF data
modem inside the daemon.
%if %{have_go}
Built with the Go /api/v2 and hermes.v1 layer.
%else
Built without the Go /api/v2 layer: /api/v2 answers 503.
%endif

%prep
%setup -q -n hermes-radio-daemon-%{commit}

%build
make %{?_smp_mflags} radio_daemon radio_client ws_client \
     HAVE_GO_API=%{have_go} RADIOD_GIT_HASH=%{commit}

%install
make install DESTDIR=%{buildroot} prefix=%{_prefix} sysconfdir=%{_sysconfdir} \
     HAVE_GO_API=%{have_go} RADIOD_GIT_HASH=%{commit}
install -m 755 ws_client %{buildroot}%{_bindir}/ws_client
# Sailfish OS: the unit goes with the package's units, and there is no
# ssl-cert group
rm -f %{buildroot}%{_sysconfdir}/systemd/system/radiod.service
install -D -m 644 %{SOURCE1} %{buildroot}%{_unitdir}/radiod.service

%post
systemctl daemon-reload >/dev/null 2>&1 || :

%preun
if [ $1 -eq 0 ]; then
    systemctl disable --now radiod.service >/dev/null 2>&1 || :
fi

%postun
systemctl daemon-reload >/dev/null 2>&1 || :

%files
%license LICENSE
%{_bindir}/radio_daemon
%{_bindir}/radio_client
%{_bindir}/sbitx_client
%{_bindir}/ws_client
%{_unitdir}/radiod.service
%dir %{_sysconfdir}/hermes
%config(noreplace) %{_sysconfdir}/hermes/core.ini
%config(noreplace) %{_sysconfdir}/hermes/user.ini
%{_sysconfdir}/hermes/web
%{_sysconfdir}/hermes/avahi
