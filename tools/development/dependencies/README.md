# Optional AWS SDK source repair

AWS SDK C++ 1.11.906 pins aws-c-mqtt 0.16.1 at
`e35b9ca3f9fcbf1a972c831c5e79046ce56959d1`. It is affected by
[GHSA-g3x5-86mr-65p5](https://github.com/awslabs/aws-c-mqtt/security/advisories/GHSA-g3x5-86mr-65p5)
(CVE-2026-115417), fixed in 0.16.2 at
`95739b3f34830e9e32a392b783994df5279387d9`. Upgrade this exact transitive
dependency explicitly; a nominal SDK version alone does not prove the fix.

The [source lock](../../../cmake/dependencies/aws-sdk-1.11.906-mqtt-security.json)
retains SDK commit `ea46b10542d4dbb5c814801eaa02cd3ea90b2551` and its entire
recursive submodule inventory, with only MQTT overridden. The verifier checks
HEADs, the sole unstaged gitlink override, and clean tracked/index/untracked
contents, including ignored files. It refuses a later recursive submodule reset,
extra drift, incomplete checkouts and paths escaping the source root. Use a fresh
source tree and an outside-tree build. It performs no fetch, checkout or build.

This is a source check, **not installed-prefix admission or binary qualification**.
It does not change `NativeAwsSdkImport.cmake` or the existing explicit reviewed-prefix
trust. Existing installed archives remain unqualified for this repair. SDK archives
obtained elsewhere require their own source/build/link evidence. This procedure
does not change any NativeProvisionedAuth crypto binary pin.

## Fresh source and complete SDK build recipe

The following commands are preparation instructions, not an executed build.
Choose new owned absolute paths. Do not reuse or mutate an existing SDK source,
build or prefix. Acquire public source only where separately authorized.

```sh
ODBCPP_REPO=/absolute/path/to/odbcpp
SDK_SOURCE=/absolute/new/owned/aws-sdk-cpp-1.11.906-mqtt-0.16.2
SDK_BUILD=/absolute/new/owned/aws-sdk-build
SDK_PREFIX=/absolute/new/owned/aws-sdk-prefix
test ! -e "$SDK_SOURCE" && test ! -L "$SDK_SOURCE" || exit 1
test ! -e "$SDK_BUILD" && test ! -L "$SDK_BUILD" || exit 1
test ! -e "$SDK_PREFIX" && test ! -L "$SDK_PREFIX" || exit 1
git clone --no-checkout https://github.com/aws/aws-sdk-cpp.git "$SDK_SOURCE"
git -C "$SDK_SOURCE" checkout --detach ea46b10542d4dbb5c814801eaa02cd3ea90b2551
git -C "$SDK_SOURCE" submodule update --init --recursive
MQTT_SOURCE="$SDK_SOURCE/crt/aws-crt-cpp/crt/aws-c-mqtt"
git -C "$MQTT_SOURCE" fetch origin 95739b3f34830e9e32a392b783994df5279387d9
git -C "$MQTT_SOURCE" checkout --detach 95739b3f34830e9e32a392b783994df5279387d9
python3 -B "$ODBCPP_REPO/tools/development/dependencies/aws_sdk_provenance.py" --source-root "$SDK_SOURCE"
cmake -S "$SDK_SOURCE" -B "$SDK_BUILD" -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$SDK_PREFIX" \
  -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk \
  '-DBUILD_ONLY=redshift;redshift-serverless;sts;sso-oidc' \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_DEPS=ON -DENFORCE_SUBMODULE_VERSIONS=ON \
  -DUSE_OPENSSL=ON -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3 \
  -DUSE_CRT_HTTP_CLIENT=ON -DUSE_CRT_HTTP_CLIENT_ASYNC_IO=OFF \
  -DENABLE_TESTING=OFF -DAUTORUN_UNIT_TESTS=OFF
python3 -B "$ODBCPP_REPO/tools/development/dependencies/aws_sdk_provenance.py" --source-root "$SDK_SOURCE"
cmake --build "$SDK_BUILD" --parallel 2
cmake --install "$SDK_BUILD"
```

The profile above matches the inspected local static macOS SDK's selected service
set, including its additional `sso-oidc` component. It is not a cross-platform
qualification recipe. Upstream's submodule check permits descendants; leave its
enforcement enabled, while the stricter source lock permits only the exact MQTT
repair. Record the actual compiler, SDK, flags, dependency versions and source
lock result before accepting a produced prefix. Run the source verifier again
after the build. No artifact produced by these instructions has yet been accepted.

## Smaller local MQTT-only rebuild candidate

The inspected local prefix is about 25 MiB, static arm64 Release. Public headers
are unchanged between the two MQTT commits. A separately authorized fresh copy
of that prefix plus a standalone MQTT rebuild can avoid rebuilding the whole SDK.
This is a proposed qualification path, not an assertion of ABI compatibility:
upstream describes MQTT as not ABI-stable. Keep the original prefix and sources
untouched; do not install into them or select the new prefix for operations.

After verifying the original SDK closure against the retained upstream inventory
(with its old MQTT HEAD), acquire a **separate** MQTT checkout at the fixed commit. A standalone
checkout cannot pass the complete SDK closure verifier. Preserve both inventories
and compare the public headers, changed source and dependencies explicitly.

With a reviewed baseline prefix, new empty destinations, a clean fixed MQTT source
and the same AppleClang 17.0.0.17000013 toolchain, the proposed component commands are:

```sh
BASE_PREFIX=/absolute/path/to/reviewed/baseline-prefix
PATCH_PREFIX=/absolute/new/owned/aws-sdk-prefix-mqtt-0.16.2
MQTT_SOURCE=/absolute/new/owned/aws-c-mqtt-0.16.2
MQTT_BUILD=/absolute/new/owned/aws-c-mqtt-build
test ! -e "$PATCH_PREFIX" && test ! -L "$PATCH_PREFIX" || exit 1
test ! -e "$MQTT_BUILD" && test ! -L "$MQTT_BUILD" || exit 1
test "$(git -C "$MQTT_SOURCE" rev-parse HEAD)" = 95739b3f34830e9e32a392b783994df5279387d9 || exit 1
cp -a "$BASE_PREFIX" "$PATCH_PREFIX"
cmake -S "$MQTT_SOURCE" -B "$MQTT_BUILD" -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_C_STANDARD=99 -DCMAKE_C_EXTENSIONS=ON \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk \
  -DCMAKE_INSTALL_PREFIX="$PATCH_PREFIX" -DCMAKE_PREFIX_PATH="$PATCH_PREFIX" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DBUILD_TESTING=ON -DENABLE_NET_TESTS=OFF \
  '-DCMAKE_C_FLAGS=-DAWS_ENABLE_DISPATCH_QUEUE -DAWS_ENABLE_KQUEUE -DAWS_TEST_REGION=US_EAST_1 -DENABLED_REQUEST_COMPRESSION -DENABLED_ZLIB_REQUEST_COMPRESSION -DENFORCE_TLS_V1_2 -DHAVE_SYSCONF -DINTEL_NO_ITTNOTIFY_API -DPLATFORM_APPLE -DAWS_SDK_USE_CRT_HTTP -DHAVE_H2_CLIENT -fPIC -moutline-atomics'
# Review compile_commands.json and imported archive locations before building.
cmake --build "$MQTT_BUILD" --target aws-c-mqtt aws-c-mqtt-tests --parallel 2
ctest --test-dir "$MQTT_BUILD" -N -R '^mqtt5to3_adapter_'
ctest --test-dir "$MQTT_BUILD" --output-on-failure --timeout 60 \
  -R '^mqtt5to3_adapter_subscribe_multi_oversized_suback$'
ctest --test-dir "$MQTT_BUILD" --output-on-failure --timeout 60 \
  -R '^mqtt5to3_adapter_'
cmake --install "$MQTT_BUILD"
```

`ENABLE_NET_TESTS=OFF` excludes upstream internet tests; the adapter tests use local
mock servers. Do not run example/broker/cloud clients. Before accepting the copy,
retain raw test inventory/results and prove: original source/prefix unchanged;
new MQTT archive freshly compiled from the fixed source; arm64/static identity;
public-header and exported-symbol compatibility; and every other SDK/CRT archive
byte-identical to the original. Review changed CMake metadata and all imported
paths. Stop on unexpected changes or mismatched compile flags. The source verifier
alone cannot prove any of these binary properties.

Finally compile and link a fresh offline consumer using the existing
`cmake/NativeAwsSdkImport.cmake`, the new explicit `ODBCPP_AWS_SDK_INSTALL`, and
reviewed OpenSSL/ZLIB discovery. Include SDK Redshift/STS constructors and an MQTT
symbol reference so static linking actually selects the new archive; retain the
link command/map and verify no original SDK prefix is selected. Do not execute
the consumer, make AWS calls or alter operational configuration. Installed-prefix
acceptance stays pending until this independent source/build/link review passes.

## Offline controls

```sh
python3 -B tools/development/dependencies/test_aws_sdk_provenance.py
```

Tests exercise exact source success, old/reset MQTT, additional drift, staged or
missing modules, dirty/ignored inputs, path escape and Git failure. They use local
metadata fixtures and do not download, compile or qualify AWS dependencies.
