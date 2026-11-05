#!/bin/sh
# Cross-compiles the Legacia client plugin (32-bit) on Linux with MinGW-w64.
#   sudo apt install g++-mingw-w64-i686 cmake curl
#   ./build.sh            -> build/package/legacia.dll + build/package/plugins/legacia_plugin.dll
set -e
cd "$(dirname "$0")"
OPUS_VERSION=1.5.2
OPUS_SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
DEPS="$PWD/build/deps"
mkdir -p "$DEPS"

cat > build/mingw32.cmake <<'TC'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)
set(CMAKE_C_COMPILER i686-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER i686-w64-mingw32-g++)
set(CMAKE_RC_COMPILER i686-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
TC

if [ ! -f "$DEPS/lib/libopus.a" ]; then
	curl -sSL -o build/opus.tar.gz "https://github.com/xiph/opus/releases/download/v$OPUS_VERSION/opus-$OPUS_VERSION.tar.gz"
	echo "$OPUS_SHA256  build/opus.tar.gz" | sha256sum -c -
	tar xzf build/opus.tar.gz -C build
	cmake -S "build/opus-$OPUS_VERSION" -B build/opus -DCMAKE_TOOLCHAIN_FILE="$PWD/build/mingw32.cmake" \
		-DCMAKE_BUILD_TYPE=Release -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF \
		-DOPUS_BUILD_PROGRAMS=OFF -DOPUS_STACK_PROTECTOR=OFF -DOPUS_FORTIFY_SOURCE=OFF \
		-DCMAKE_INSTALL_PREFIX="$DEPS"
	cmake --build build/opus -j4
	cmake --install build/opus
fi

cmake -S . -B build/dll -DCMAKE_TOOLCHAIN_FILE="$PWD/build/mingw32.cmake" -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_PREFIX_PATH="$DEPS"
cmake --build build/dll -j4
# loader layout: legacia.dll (loader) + plugins/legacia_plugin.dll (the real plugin)
rm -rf build/package
mkdir -p build/package/plugins
cp build/dll/loader/legacia.dll build/package/legacia.dll
cp build/dll/legacia_plugin.dll build/package/plugins/legacia_plugin.dll
cp legacia.ini build/package/legacia.ini
echo "built build/package/ (legacia.dll loader + plugins/legacia_plugin.dll + legacia.ini)"
