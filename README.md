# omcproxy - Embedded IGMPv3 and MLDv2 proxy

omcproxy is an IGMPv3 and MLDv2 multicast proxy for use in embedded Linux
devices like routers. It is small in size and can be compiled to <40 KB.

It is partly based on code of https://github.com/Oryon/pimbd

## Specifications and Features

1. Source-Specific Multicast Querier
	- MLDv2 querier (based on RFC 3810)
	- IGMPv3 querier (based on RFC 3376)

2. Multicast Proxying (based on RFC 4605)
	- Kernel-space multicast routing
	- Multiple instances support
	- Address-scope specific proxying
	

## Compiling

omcproxy uses libubox as submodule, be sure to clone this git repository
with --recursive or run: "git submodule init; git submodule update"
after cloning. If you are already using libubox as a shared library
just pass -DWITH_LIBUBOX=1 to cmake.

omcproxy uses cmake:
- To prepare a Makefile use:  "cmake ." 
- To build / install use: "make" / "make install" afterwards.
- To build DEB or RPM packages use: "make package" afterwards.
