Name:           quill
Version:        0.1.2
Release:        1%{?dist}
Summary:        Quill

License:        MIT
URL:            https://github.com/Olivenda/quill-Linux-Editor.git
Source0:        editor.cpp
Source1:        LICENSE
Source2:        README.md
BuildRequires:  gcc, ncurses-devel
Requires:       gcc
BuildArch:      x86_64

%description
A simple CLI Text Editor

%prep
cp -p %{SOURCE0} .
cp -p %{SOURCE1} .
cp -p %{SOURCE2} .

%build
g++ editor.cpp -o quill -lncurses

%install
install -Dm755 quill %{buildroot}/usr/bin/quill

%files
/usr/bin/quill
%license LICENSE
%doc README.md
%changelog
* Wed Nov 05 2025 Christian R. <chris-r@cronodevelopment-com> 0.1
- First RPM Release
