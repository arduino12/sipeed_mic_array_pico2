#!/usr/bin/env bash
# Build ODAS (introlab/odas) inside WSL Ubuntu without root: the -dev
# packages are fetched with `apt-get download` and unpacked into a private
# sysroot instead of being installed. Result: ~/odas_build/build/bin/odaslive
#
#   wsl -e bash /mnt/c/.../sipeed_mic_array_pico2/odas/build_odas_wsl.sh
#
# With sudo available, the normal route is simpler:
#   sudo apt install libfftw3-dev libconfig-dev libasound2-dev libpulse-dev cmake
#   git clone https://github.com/introlab/odas && cmake -S odas -B build && cmake --build build
set -euo pipefail

ROOT="$HOME/odas_build"
S="$ROOT/sysroot"
L="$S/usr/lib/x86_64-linux-gnu"
mkdir -p "$ROOT/debs" "$S"
cd "$ROOT/debs"

PKGS="libfftw3-dev libfftw3-single3 libfftw3-double3 libfftw3-long3 libfftw3-quad3 libfftw3-bin
      libconfig-dev libconfig9 libasound2-dev libasound2t64 libasound2-data
      libpulse-dev libpulse0 libpulse-mainloop-glib0 libsndfile1 libasyncns0
      libflac12t64 libvorbis0a libvorbisenc2 libopus0 libogg0 libmpg123-0t64 libmp3lame0"
for p in $PKGS; do
    ls "${p}"_*.deb >/dev/null 2>&1 || apt-get download "$p"
done
for d in *.deb; do dpkg -x "$d" "$S"; done

cd "$ROOT"
[ -d odas ] || git clone --depth 1 https://github.com/introlab/odas.git
export PKG_CONFIG_PATH="$L/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$S"
LF="-L$L -Wl,-rpath-link,$L -Wl,-rpath-link,$L/pulseaudio -Wl,-rpath,$L -Wl,-rpath,$L/pulseaudio"
cmake -S odas -B build -DCMAKE_BUILD_TYPE=Release -DODAS_DISABLE_INSTALL=ON \
      -DCMAKE_C_FLAGS="-I$S/usr/include -fcommon" \
      -DCMAKE_EXE_LINKER_FLAGS="$LF" -DCMAKE_SHARED_LINKER_FLAGS="$LF"
cmake --build build -j"$(nproc)"
echo "built: $ROOT/build/bin/odaslive"
