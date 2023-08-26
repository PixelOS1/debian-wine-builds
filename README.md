# Debian Wine Builds By PixelOS
We compile & package RELEASE only builds of Wine Mainline for Debian based systems. These builds are used in part to support PixelOS(https://www.pixelos.com).

Our Wine builds are built on a custom build environment using glibc 2.31 (Ubuntu 20.04 LTS era). This ensures maximum compatibility with most recent Linux distributions.

We do not plan to upgrade glibc 2.31 and provide our own LTS support for glibc 2.31.

# Releases
- 8.14 (Release notes: https://www.winehq.org/announce/8.14)

# Requirements:
- Build on Debian based systems or derivative (e.g. Ubuntu 20.04 LTS) running =glibc 2.31.
- Install on any Debian based systems or derivatives (e.g. Ubuntu) running >=glibc 2.31.
- Remove previous distro installs of Wine.
- Runners, Steam, Proton-builds, custom builds are fine.

# How to build?
```./build_package.sh version package_version```

example: ```./build_package.sh 8.14 8.14.r5.gcdcdde3d```

# How to install?
```dpkg -i wine-pixelos.8.14.deb```

# Will we be providing a deb repo?
Yes. We plan to provide a repo for PixelOS very soon. You will need to manually remove builds when we do to avoid conflicts!

# Will these be a Steam Proton specific build?
At the moment we have no plans to provide a proton specific build since there are already maintainers provided these builds for Steam games such as GE and TkG.

# What is different between this and Lutris or Bottles?
Lutris and Bottles were developed exclusively for gaming on Wine. Lutris is arguably the defacto standard. Lutris assumes you have lots of games and different runners for each game. Our Debian build is intended for systems that desire a system-wide install of Wine for a specific application. Usually Enterprise grade installs including Enterprise Servers.

# Is this just for video games?
Absolutely not. In-fact this is a full fledged system wide install of Wine. We support 3D and 2D desktop applications running on Linux. We often use this build inside of a Docker container or Kubernetes clusters.

# Who can use this build?
Anyone that needs Windows application or video game support on a Debian based Linux system.

# glibc Background
For those of you not familar with glibc.

The GNU C Library, commonly known as glibc, is the GNU Project's implementation of the C standard library. Despite its name, it now also directly supports C++ (and, indirectly, other programming languages). It was started in the 1980s by the Free Software Foundation (FSF) for the GNU operating system. (https://en.wikipedia.org/wiki/Glibc)

The state of glibc version support concludes that if you need your build environment to support older distros than you should run a bleeding edge build environment.

Generally. The Proton community recommends using ArchLinux to build Wine. And this is great for ArchLinux users. But for others such as Debian users - it is not possible to build Wine for Debian using ArchLinux as a build environment. In-fact, ironically, it isn't even possible to build Wine on Debian 12 without some hacks.

# This is a WoW64 build?
Yes. For many years Wine builders have had to build two seperate builds of Wine. One for x86 and one for amd64. This means there is no more ```wine64``` binary. In our build you simply run ```wine program.exe``` and watch the magic happen. WoW64 allows 32-bit Windows applications to work without a 32-bit Unix library!

# Community Builders
While I applaud the effort of TkG and GE to support robust build environments for Wine. Their support isn't far from ArchLinux (TkG) and Fedora (GE). We're pleased to stand on the shoulders of these giants and provide support for Debian based distros. Our build includes patches provided by both TkG and GE. Thank you both for your support on Discord and Github!

# Disclaimer
PixelOS provides support for Enterprise based GPU and vGPU suppport for virtual Desktop based applications. We do not WARRANTY the use of builds outside of intended systems. Use at your own RISK.
