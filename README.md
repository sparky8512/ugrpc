# ugrpc

A light-weight [gRPC](https://grpc.io/) client for encoding and decoding [Protocol Buffers](https://protobuf.dev/) messages to and from JSON strings for use with [gRPC](https://grpc.io/) services, including an end-to-end client for use with [gRPC-Web](https://github.com/grpc/grpc-web) services. It is targeted to run on routers running [OpenWrt](https://openwrt.org/) firmware (or similar).

Vaguely similar to [gRPCurl](https://github.com/fullstorydev/grpcurl/) in functionality, but much smaller in size. The mipsel_24kc target build, for example, compresses to less than 20KB.

## Usage

The full build supports 4 different commands.

The `decode` and `encode` commands translate gRPC-prefixed binary Protocol Buffers messages to and from JSON strings, respectively. These can then be used with an HTTP client such as `wget` or `curl` to send requests to a gRPC or gRPC-Web service and interpret the results. This requires having the compiled protocol data in one of several forms.

The `service` command does the same, but without the need for a separate HTTP client and does not need the protocol data if the service support reflection. It is (currently) limited to gRPC-Web due to the limitations of the HTTP client included with the base OpenWrt firmware image.

The `reflect` command is similar to `service`, but instead of encoding a request message from JSON, it creates a reflection request for a specified symbol name and emits the returned protocol data in a form usable by the other commands.

Run `ugrc` without arguments for full usage details.

### Examples

The following examples communicate with a [Starlink](https://www.starlink.com/) user terminal, which can be reached from the LAN side at IP 192.168.100.1 and exposes services via a gRPC endpoint on port 9200 and a gRPC-Web endpoint on port 9201. They illustrate usage of the `Handle` method of service `SpaceX.API.Device.Device`, which takes a `SpaceX.API.Device.Request` message as input and returns a `SpaceX.API.Device.Response` message.

Get current status information and pretty-print the results:
```shell
ugrpc -p service 192.168.100.1:9201 SpaceX.API.Device.Device.Handle '{"get_status":{}}'
```

Do the same, but with a pre-downloaded set of protocol data, and fail if that protocol data is insufficient:
```shell
ugrpc -s /tmp/dish.fds -n -p service 192.168.100.1:9201 SpaceX.API.Device.Device.Handle '{"get_status":{}}'
```

Fetch the protocol data needed by the prior command:
```shell
ugrpc reflect 192.168.100.1:9201 SpaceX.API.Device.Device.Handle > /tmp/dish.fds
```

Encode a reflection request message using the built-in reflection protocol data (see [below](#the-reflection_v1-pseudo-package)):
```shell
ugrpc encode @reflection_v1.ServerReflectionRequest '{"file_containing_symbol":"SpaceX.API.Device.Device.Handle"}' > /tmp/reflect_req.gmsg
```

Use that encoded request with `wget` to fetch relevant protocol data:
```shell
wget --post-file=/tmp/reflect_req.gmsg -O /tmp/reflect_resp.gmsg --header='Content-Type: application/grpc-web+proto' --header='X-User-Agent: grpc-web-javascript/0.1' --header='X-Grpc-Web: 1' 'http://192.168.100.1:9201/grpc.reflection.v1.ServerReflection/ServerReflectionInfo'
```

Use that reflected protocol data to encode the request used by the `service` commands above, send that request to the service using `wget`, then decode the response:
```shell
ugrpc -r /tmp/reflect_resp.gmsg encode SpaceX.API.Device.Request '{"get_status":{}}' > /tmp/status_req.gmsg
wget --post-file=/tmp/status_req.gmsg -O /tmp/status_resp.gmsg --header='Content-Type: application/grpc-web+proto' --header='X-User-Agent: grpc-web-javascript/0.1' --header='X-Grpc-Web: 1' 'http://192.168.100.1:9201/SpaceX.API.Device.Device/Handle'
ugrpc -r /tmp/reflect_resp.gmsg decode SpaceX.API.Device.Response < /tmp/status_resp.gmsg
```

Do the same thing using curl and gRPC rather than gRPC-Web:
```shell
ugrpc encode @reflection_v1.ServerReflectionRequest '{"file_containing_symbol":"SpaceX.API.Device.Device.Handle"}' > /tmp/reflect_req.gmsg
curl --http2-prior-knowledge -X POST --data-binary @/tmp/reflect_req.gmsg -o /tmp/reflect_resp.gmsg -H 'Content-Type: application/grpc+proto' -H 'te: trailers' -H 'grpc-timeout: 10S' 'http://192.168.100.1:9200/grpc.reflection.v1.ServerReflection/ServerReflectionInfo'
ugrpc -r /tmp/reflect_resp.gmsg encode SpaceX.API.Device.Request '{"get_status":{}}' > /tmp/status_req.gmsg
curl --http2-prior-knowledge -X POST --data-binary @/tmp/status_req.gmsg -o /tmp/status_resp.gmsg -H 'Content-Type: application/grpc+proto' -H 'te: trailers' -H 'grpc-timeout: 10S' 'http://192.168.100.1:9200/SpaceX.API.Device.Device/Handle'
ugrpc -r /tmp/reflect_resp.gmsg decode SpaceX.API.Device.Response < /tmp/status_resp.gmsg
```

### Low-memory algorithm

The JSON library used by the `decode` and `service` commands to compute the JSON output is not very memory-efficient. `ugrpc` supports a more efficient algorithm for computing that JSON string, which can be enabled with the `-l` option. Without this option, memory usage will scale with the length of the decoded output.

This option does not correctly handle a few corner cases in the Protocol Buffers specification that decoders are supposed to handle, so this algorithm is not enabled by default. However, these corner cases are very unlikely to show up in properly serialized Protocol Buffers data unless there is a protocol data mismatch, and a warning will be printed if these cases are detected.

The other major source of memory usage is the parsed protocol data, which this option does nothing to reduce. If minimizing memory usage is critical for your application, consider creating a subset of of the protocol you are using with just the necessary type info and use that instead of pulling protocol data at run time via reflection. Even for a bloaty protocol, though, the difference here will typically be 10s of KB total usage vs 100s of KB, so this is probably only needed in extremely limited environments.

### The @reflection_v1 pseudo-package

The protocol data used by the `service` and `reflect` commands to encode and decode reflection messages can also be used for other purposes. To use it, substitute the package name `grpc.reflection.v1` or `grpc.reflection.v1alpha` with `@reflection_v1`. Note that this is a stripped down version of the reflection protocol with only support for the `file_containing_symbol` request, its associated response messages, and the associated method.

## Limitations

The camelCase [ProtoJSON](https://protobuf.dev/programming-guides/json/) form of JSON is not supported for either input or output. Instead, use the actual field names as they are specified in the protocol definition, which typically use underscores.

Support is targeted at the `proto3` syntax of Protocol Buffers messages. `proto2` syntax messages should decode without error, but some types (currently) may not encode correctly. `editions` syntax messages will probably only encode and decode correctly if no `editions`-specific feature that alters the encoding vs `proto3` is used.

Performance will probably not be as high as executables built with compiled protocol data (using `protoc`). This is a trade-off for the orders of magnitude reduction in code size.

See also the [list below](#to-be-done-or-not) for features that have not been implemented, and probably never will be unless someone asks for them. There's probably some other obscure features that are technically allowed in the `proto3` syntax that are not properly handled, support for which will only be added in the unlikely case someone actually needs them.

## Building for OpenWrt

The easiest way to build this repository is by using the [OpenWrt Build System](https://openwrt.org/docs/guide-developer/toolchain/use-buildsystem) or [OpenWrt SDK](https://openwrt.org/docs/guide-developer/toolchain/using_the_sdk). The following commands show minimal steps to build just the `ugrpc` package using the SDK. Building additional packages with the SDK or as part of a firmware image using the OpenWrt Build System would require additional configuration steps.

First, install the build prerequisites, download an SDK bundle, unpack it, and `cd` into the unpacked directory. See the above links for details on how to do that. Here's an example of getting and unpacking the most recent 25.12 branch SDK for MT7621-based devices:
```shell
wget https://downloads.openwrt.org/releases/25.12-SNAPSHOT/targets/ramips/mt7621/openwrt-sdk-25.12-SNAPSHOT-ramips-mt7621_gcc-14.3.0_musl.Linux-x86_64.tar.zst
tar --zstd -xvf openwrt-sdk-25.12-SNAPSHOT-ramips-mt7621_gcc-14.3.0_musl.Linux-x86_64.tar.zst
cd openwrt-sdk-25.12-SNAPSHOT-ramips-mt7621_gcc-14.3.0_musl.Linux-x86_64
```

Then add this repository as a feed and do some minimum configuration. This does not require having the repository cloned already:
```shell
cp feeds.conf.default feeds.conf
echo "src-git ugrpc https://github.com/sparky8512/ugrpc.git" >> feeds.conf
./scripts/feeds update base ugrpc
./scripts/feeds install -a
cat >.config <<END
# CONFIG_ALL is not set
# CONFIG_ALL_KMODS is not set
# CONFIG_ALL_NONSHARED is not set
CONFIG_PACKAGE_ugrpc=m
END
make defconfig
```

If you prefer to use `menuconfig` instead, the `ugrpc` package is listed in the "Network" menu.

### Building the target package

You can then build the target package with (adjust the `-j` option as appropriate):
```shell
make -j5 package/feeds/ugrpc/ugrpc/compile
```

The exact directory for the built package will depend on which target's SDK you are using. With the download example above, it will be at `bin/packages/mipsel_24kc/ugrpc/ugrpc-0.1-r1.apk`. If using version 24.10 or earlier SDK, the package will have extension `.ipk` instead.

To install the built package, first copy it to the target device. For example:
```shell
scp -O bin/packages/mipsel_24kc/ugrpc/ugrpc-0.1-r1.apk root@192.168.1.1:/tmp/
```

Then on the router, run the command to install it. For example, for 25.12 or later:
```shell
apk add --allow-untrusted /tmp/ugrpc-0.1-r1.apk
```

### Building the host command

`ugrpc` is also buildable as a host command. Follow the same steps above up through configuration, then run:
```shell
make -j5 package/feeds/ugrpc/ugrpc/host/compile
```

The command will be installed to `staging_dir/host/bin/ugrpc`. The host build of `ugrpc` only supports the `decode` and `encode` commands because the uclient library does not build for host without some hackery.

### Building from local source

To build a local copy of the source, perhaps with modifications, first clone this repository, then change the `src-git` line in the configuration steps above to an appropriate `src-link` line, for example:
```shell
echo "src-link ugrpc /home/user/devel/ugrpc" >> feeds.conf
```

## Building outside the OpenWrt build system

You can use `cmake` directly to build this repository, but the source has a dependency on the OpenWrt utility library [libubox](https://github.com/openwrt/libubox), so you'll need to let `cmake` know where to find it, such as by using `CMAKE_FIND_ROOT_PATH`. It also has a dependency on [JSON-C](https://github.com/json-c/json-c), but that tends to be more readily available. If you enable the `service` command it also needs to be able to find [uclient](https://github.com/openwrt/uclient). In practice, these requirements mean pointing `cmake` to a built OpenWrt firmware tree or SDK.

There are several `cmake` options defined in `CMakeLists.txt`. The `ENABLE_UCLIENT_SERVICE` option controls whether or not to build support for the `service` and `reflect` commands. It defaults to enabled, but can be disabled by passing `-DENABLE_UCLIENT_SERVICE=OFF` on the `cmake` command. The other options are not as impactful and have reasonable defaults.

## To be done (or not)

The following are correctness issues and should be addressed before this project can be considered "done":
- correct handling of repeated OneOf messages; unlikely to happen unless protocol data mismatch, but decoders are supposed to handle
- merge repeated messages in non-repeated fields; unlikely to happen unless protocol data mismatch, but decoders are supposed to handle
- detect noncontiguous repeated fields in low-memory algorithm decode; unlikely to happen in real life, but technically allowed by the specification
- tests, and anything that these turn up as broken

The following are features that are not currently supported and may or may not ever be:
- HTTP2 support for the `service` and `reflect` commands, probably using libcurl, in order to support gRPC (as opposed to gRPC-Web)
- SSL (HTTPS) support for gRPC-Web
- fill in default value on decode when field is not present
- binary input/output for the `service` command
- pretty-print for low-memory algorithm
- encode multiple messages at once, for stream type services; decoding multiple messages is already supported
- encode gRPC-Web trailers, for implementing a server using the `decode` and `encode` commands
- special handling of `map` fields; currently, these are supported only as the raw key value pairs
- finish and clean up minimization script used to strip down the reflection protocol
