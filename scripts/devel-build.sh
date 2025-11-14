#!/bin/bash

set -euxo pipefail
cd "${0%/*}"
cd ..

# Sanity checks
if [ ! -e "CMakeLists.txt" ] || [ ! -e "src/omcproxy.c" ]; then
	echo "omcproxy checkout not found" >&2
	exit 1
fi

BUILD_ARGS="$@"

# Create build dirs
OMCPROXYDIR="$(pwd)"
BUILDDIR="${OMCPROXYDIR}/build"
DEPSDIR="${BUILDDIR}/depends"
[ -e "${BUILDDIR}" ] || mkdir "${BUILDDIR}"
[ -e "${DEPSDIR}" ] || mkdir "${DEPSDIR}"

# Download deps
cd "${DEPSDIR}"
[ -e "json-c" ] || git clone https://github.com/json-c/json-c.git
[ -e "libubox" ] || git clone https://github.com/openwrt/libubox.git

# Build json-c
cd "${DEPSDIR}/json-c"
cmake							\
	-S .						\
	-B .						\
	-DCMAKE_PREFIX_PATH="${BUILDDIR}"		\
	-DBUILD_SHARED_LIBS=OFF				\
	-DDISABLE_EXTRA_LIBS=ON				\
	-DBUILD_TESTING=OFF				\
	--install-prefix "${BUILDDIR}"
make
make install

# Build libubox
cd "${DEPSDIR}/libubox"
cmake							\
	-S .						\
	-B .						\
	-DCMAKE_PREFIX_PATH="${BUILDDIR}"		\
	-DBUILD_LUA=OFF					\
	-DBUILD_EXAMPLES=OFF				\
	--install-prefix "${BUILDDIR}"
make
make install

# Build omcproxy
cd "${OMCPROXYDIR}"
cmake							\
	-S .						\
	-B "${BUILDDIR}"				\
	-DCMAKE_PREFIX_PATH="${BUILDDIR}"		\
	${BUILD_ARGS}
make -C "${BUILDDIR}"

set +x
echo "✅ Success - the omcproxy binary is available at ${BUILDDIR}/omcproxy"
echo "👷 You can rebuild omcproxy by running 'make -C build'"

exit 0
