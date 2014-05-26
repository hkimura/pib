Name: pibnetd
Version: 0.4.0
Release: 1%{?dist}
Summary: Pseudo InfiniBand Fabric emulation daemon
Group: System Environment/Daemons
License: GPLv2 or BSD
Url: http://www.nminoru.jp/
Source: %{name}-%{version}.tar.gz
BuildRoot: %(mktemp -ud %{_tmppath}/%{name}-%{version}-%{release}-XXXXXX)
Provides: libpib-devel = %{version}-%{release}
Requires: libibverbs > 1.1.4
BuildRequires: libibverbs-devel > 1.1.4
# ExcludeArch: s390 s390x

%description
pibnetd is the Pseudo InfiniBand Fabric emulation daemon for pib.

%prep

%setup -q

%build
make

%install
rm -rf $RPM_BUILD_ROOT
install -D -m755 pibnetd %{buildroot}%{_sbindir}/pibnetd
install -D -m755 scripts/redhat-pibnetd.init %{buildroot}%{_initddir}/pibnetd

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root,-)
%{_sbindir}/pibnetd
%{_initddir}/pibnetd

%changelog