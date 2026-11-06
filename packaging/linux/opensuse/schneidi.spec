#
# spec file for package schneidi (reference for packagers, see README.md next to it)
#
# Copyright (c) 2026 the schneidi project
#
# All modifications and additions to the file contributed by third parties
# remain the property of their copyright owners, unless otherwise agreed
# upon. The license for this file, and modifications and additions to the
# file, is the same license as for the pristine package itself (unless the
# license for the pristine package is not an Open Source License, in which
# case the license is the MIT License).
#

%define rnnoise_model 0b50c45
# The tests make their test videos with libx264, which openSUSE's FFmpeg does not have (Packman's has): about half of
# them fail without it. Off by default; "--with tests" (osc build / rpmbuild) builds and runs them.
%bcond_with tests
Name:           schneidi
Version:        0.18.0
Release:        0
Summary:        Fast, light video editor that an AI assistant can edit with
# schneidi: AGPL-3.0-only; built in: RNNoise (BSD-3-Clause), earshot sound recipes (MIT),
# recorded sounds (CC0-1.0), miniaudio (MIT-0 or Unlicense)
License:        AGPL-3.0-only AND BSD-3-Clause AND MIT AND CC0-1.0 AND (MIT-0 OR Unlicense)
Group:          Productivity/Multimedia/Video/Editors and Convertors
URL:            https://github.com/OnkelTheorie/schneidi
Source0:        https://github.com/OnkelTheorie/schneidi/archive/refs/tags/v%{version}.tar.gz#/%{name}-%{version}.tar.gz
# Noise suppression model of RNNoise (the build downloads it otherwise; OBS has no network).
# SHA-256 4ac81c5c0884ec4bd5907026aaae16209b7b76cd9d7f71af582094a2f98f4b43, checked by the build.
Source1:        https://media.xiph.org/rnnoise/models/rnnoise_data-%{rnnoise_model}.tar.gz
BuildRequires:  AppStream
BuildRequires:  cmake >= 3.21
BuildRequires:  desktop-file-utils
BuildRequires:  gcc-c++
# owns share/icons/hicolor
BuildRequires:  hicolor-icon-theme
BuildRequires:  ninja
BuildRequires:  pkgconfig
BuildRequires:  qt6-gui-private-devel
BuildRequires:  cmake(Qt6DBus)
BuildRequires:  cmake(Qt6Network)
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  pkgconfig(libavcodec)
BuildRequires:  pkgconfig(libavformat)
BuildRequires:  pkgconfig(libavutil)
BuildRequires:  pkgconfig(libswresample)
BuildRequires:  pkgconfig(libswscale)
%if %{with tests}
BuildRequires:  cmake(Qt6Test)
# Tests make media with the ffmpeg/ffprobe programs and compare against them
BuildRequires:  ffmpeg
%endif
# miniaudio opens ALSA/PulseAudio at run time (dlopen), invisible to the automatic dependencies
Requires:       libasound2
Recommends:     ffmpeg
Recommends:     libpulse0
Requires:       hicolor-icon-theme

%description
schneidi is a fast, light video editor with four pages for the steps of an
edit (Media, Edit, Color, Deliver). An AI assistant can edit together with
you, live in the open project: through the AI chat in the app (Claude, an
OpenAI-compatible service or a local model) or through the command line
program schneidi-cli and its MCP server. Multicam, 9:16 shorts next to the
16:9 edit, templates, color wheels and scopes, offline speech recognition.

%prep
%autosetup -p1
# The model tarball where the build looks for it (no download)
cp %{SOURCE1} third_party/rnnoise/
# %%license puts every file into one folder: the three LICENSE files need names of their own
cp third_party/miniaudio/LICENSE LICENSE.miniaudio
cp third_party/earshot/LICENSE LICENSE.earshot
cp third_party/rnnoise/COPYING COPYING.rnnoise

%build
# No system miniaudio on openSUSE: the build uses its bundled copy (third_party/miniaudio)
# From the tarball (no .git folder) schneidi's own code is linked statically into both programs.
%cmake_qt6 \
  -DQT_NO_PRIVATE_MODULE_WARNING=ON \
  -DSCHNEIDI_TESTS=%{?with_tests:ON}%{!?with_tests:OFF}
%{qt6_build}

%install
%{qt6_install}
# Debian-policy copyright/changelog.gz (CMakeLists.txt): the licenses are packaged with %%license below
rm -rf %{buildroot}%{_datadir}/doc/%{name}

%check
export QT_QPA_PLATFORM=offscreen
%if %{with tests}
%ctest
%endif
# The installed command line program starts (statically linked: no library of schneidi's own to find)
%{buildroot}%{_bindir}/%{name}-cli --version
desktop-file-validate %{buildroot}%{_datadir}/applications/%{name}.desktop
appstreamcli validate --no-net %{buildroot}%{_datadir}/metainfo/io.github.OnkelTheorie.schneidi.metainfo.xml

%files
%license LICENSE THIRD_PARTY.md COPYING.rnnoise LICENSE.miniaudio LICENSE.earshot
%doc README.md
%{_bindir}/%{name}
%{_bindir}/%{name}-cli
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/scalable/apps/%{name}.svg
%{_datadir}/metainfo/io.github.OnkelTheorie.schneidi.metainfo.xml

%changelog
