Name: pib
Version: 0.4.6
Release: 1%{?dist}
Summary: Pseudo InfiniBand (pib) HCA Kernel Driver
Group: System/Kernel
License: GPLv2 or BSD
Url: http://www.nminoru.jp/
Source0: %{name}-%{version}.tar.gz
Source1: %{name}.files
Source2: %{name}.conf
BuildRoot: %(mktemp -ud %{_tmppath}/%{name}-%{version}-%{release}-XXXXXX)
BuildRequires: %kernel_module_package_buildreqs
BuildArch: i686 x86_64

%kernel_module_package -f %{SOURCE1} default

%description
Pseudo InfiniBand (pib) HCA Kernel Driver

%prep

%setup -q
set -- *
mkdir source
mv "$@" source/
mkdir obj

%build
for flavor in %flavors_to_build; do
    rm -rf obj/$flavor
    cp -r source obj/$flavor
    make -C %{kernel_source $flavor} M=$PWD/obj/$flavor
done    

%install
export INSTALL_MOD_PATH=$RPM_BUILD_ROOT
export INSTALL_MOD_DIR=extra/%{name}
for flavor in %flavors_to_build; do
    make -C %{kernel_source $flavor} modules_install M=$PWD/obj/$flavor
done    

install -m 644 -D %{SOURCE2} $RPM_BUILD_ROOT/etc/depmod.d/%{name}.conf

%clean
rm -rf $RPM_BUILD_ROOT

%changelog
* Thu Feb 12 2015 Minoru NAKAMURA <nminoru1975@gmail.com> - 0.4.6
- Add SEND with Invaildate, Local Invalidate and Fast Register Physical MR operations

* Tue Nov 06 2014 Minoru NAKAMURA <nminoru@nminoru.jp> - 0.4.5
- Add i686 support

* Tue Oct 30 2014 Minoru NAKAMURA <nminoru@nminoru.jp> - 0.4.4
- Update codes for kernel 3.17

* Tue Jul 09 2014 Minoru NAKAMURA <nminoru@nminoru.jp> - 0.4.3
- Update codes for kernel 3.15

* Tue May 27 2014 Minoru NAKAMURA <nminoru@nminoru.jp> - 0.4.2
- Fix bug that the incorrect reinitialization of dev->thread.completion causes deadlock.

* Sat May 03 2014 Minoru NAKAMURA <nminoru@nminoru.jp> - 0.4.1
- Initial spec file
