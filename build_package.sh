#!/bin/sh

$wineversion
$packageversion

$path = $PWD

echo "Debian Build Script for Wine. Provided by PixelOS.\n"
echo "This script is intended to be used with our PixelOS Wine sources.\n"
echo "Sources are updated on every major Wine release.\n\n"

echo "Please build using >=glibc 2.31 (e.g. Ubuntu 20.04 LTS).\n\n"

echo "This is a WoW64 build...\n\n"

echo "Installing 64-bit dependencies. Please be wait...\n"
sudo apt -y install git libunwind-dev autoconf bison ccache debhelper desktop-file-utils docbook-to-man docbook-utils docbook-xsl flex fontforge gawk gettext libacl1-dev libasound2-dev libcapi20-dev libcups2-dev libdbus-1-dev libgif-dev libglu1-mesa-dev libgphoto2-dev libgsm1-dev libgtk-3-dev libkrb5-dev libxi-dev liblcms2-dev libldap2-dev libmpg123-dev libncurses5-dev libopenal-dev libosmesa6-dev libpcap-dev libpulse-dev libsane-dev libssl-dev libtiff5-dev libudev-dev libv4l-dev libva-dev libxslt1-dev libxt-dev ocl-icd-opencl-dev oss4-dev prelink sharutils unixodbc-dev valgrind schedtool libfreetype6-dev xserver-xorg-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gcc-multilib g++-multilib curl fonttools libsdl2-dev python3-tk libvulkan1 libc6-dev linux-libc-dev libkdb5-* libppl14 libcolord2 libvulkan-dev libgnutls28-dev libpng-dev libkadm5clnt-mit* libkadm5srv-mit* libavcodec-dev libavutil-dev libswresample-dev libavcodec58 libswresample3 libavutil56 libfaudio0 libfaudio-dev libvkd3d-dev libxinerama-dev libxcursor-dev libxrandr-dev libxcomposite-dev mingw-w64 glslang-dev glslang-tools meson wget python3-pefile rustc cargo python3-ldb samba-libs samba-dev libgcrypt20-dev libusb-1.0-0-dev yasm jq

cd $wineversion

# Configure Wine
echo "Configuring Wine. Please be patient!\n"
./configure --prefix=$path/winebuild-$wineversion/usr --enable-archs=i386,x86_64 --with-x --with-gstreamer --disable-tests

# Build Wine
echo "Building Wine. Please be patient!\n"
make -j10 && sudo make install

# Package Wine
echo "Packaging Wine for Debian. Please be patient!\n"
cd $path
tar --exclude='.[^/]*' -cvzf data.tar.gz -C winebuild-$wineversion ./ >> /dev/null
mkdir $path/control
cat >$path/control/control <<EOL
Package: wine-pixelos
Version: ${packageversion}
Architecture: all
Maintainer: Vince McMullin <vincem@pixelos.com>
Homepage: https://www.pixelos.com
Provides: wine, libwine, fonts-wine
Conflicts: wine, libwine, fonts-wine
Replaces: wine, libwine, fonts-wine
License: LGPL
Depends: libc6 (>= 2.17), libfontconfig1 (>= 2.11), libfreetype6 (>= 2.2.1), libncurses5 (>= 6), libasound2 (>= 1.0.16), libgcc1 (>= 1:3.0), libglu1-mesa | >
Installed-Size: $(du -sb winebuild-${wineversion} | awk '{printf "%1.0f\n",$1/1024}')
Description: PixelOS build of Wine
EOL
tar czf control.tar.gz -C control ./ >> /dev/null
#rm -r control

echo "2.0" > debian-binary

ar -r wine-tkg-pixelos.$wineversion.deb debian-binary control.tar.gz data.tar.gz &> /dev/null
