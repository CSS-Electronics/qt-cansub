This folder contains the C++ bindings for connecting the CANsub library with the Qt 5 and Qt 6 frameworks. This allows remote CANsub hardware interfaces to be instantiated as `QCanBusDevice` objects. It is designed as a plugin, such that new devices can be added via the `QCanBusFactoryV2` (Qt 5) or `QCanBusFactory` (Qt 6) method, and features dynamic discoverability.

This is a stand-alone repository. The CANsub library (c-cpp-sdk-cansub, https://github.com/CSS-Electronics/c-cpp-sdk-cansub) is fetched by CMake with FetchContent at a fixed tag (`GIT_TAG` in `CMakeLists.txt`) and linked statically. To build against a local checkout, set `FETCHCONTENT_SOURCE_DIR_CANSUB` to its root. When the tag changes, update the README and check `THIRD_PARTY_NOTICES.md` against the code c-cpp-sdk-cansub bundles under `src/thirdparty`. qmdnsengine is vendored under `thirdparty/qmdnsengine`.

Building for Linux is done in a container to ensure the broadest binary compatibility. This uses the container image's own Qt libraries. See the `containers/Containerfile`. To build against the local c-cpp-sdk-cansub checkout, mount it into the container:

```sh
podman run --rm -v .:/project:Z -v <path to c-cpp-sdk-cansub>:/c-cpp-sdk-cansub:ro,z localhost/cansub-builder-x64:vcpkg-2026.10.16 cmake -B build -S . -GNinja -DCMAKE_BUILD_TYPE=Release -DVCPKG_TARGET_TRIPLET=x64-linux-release -DFETCHCONTENT_SOURCE_DIR_CANSUB=/c-cpp-sdk-cansub
```

Building for Windows uses Qt libraries supplied by vcpkg.

The folder layout is as follows:

```
├── src
│   ├── Core                # Everything that can be tested without `QCanBus` loading the plugin.
│   └── Plugin              # Only the factory code Qt needs to find and load the plugin.
```

Core is an `OBJECT` library linked into the plugin

The plugin is a `MODULE` with hidden symbols, which nothing can link against. Keep it to `Q_PLUGIN_METADATA`, `plugin.json` and the `QCanBusFactoryV2` (Qt5) or `QCanBusFactory` (Qt6) implementation, which forwards to Core.

Filetypes are .cxx/.hxx.

## Looking up Qt documentation

The builder image has no rendered Qt docs: `/usr/share/qt5/doc` and `/usr/share/qt6/doc` only contain qdoc config files. Use the Qt headers in the image for signatures, enums and virtual methods:

* Qt5: `/usr/include/x86_64-linux-gnu/qt5/` (e.g. `QtSerialBus/qcanbusdevice.h`, `QtSerialBus/qcanbusfactory.h`)
* Qt6: `/usr/include/x86_64-linux-gnu/qt6/`

```sh
podman run --rm localhost/cansub-builder-x64:vcpkg-2026.10.16 grep -rn virtual /usr/include/x86_64-linux-gnu/qt5/QtSerialBus/qcanbusfactory.h
podman run --rm localhost/cansub-builder-x64:vcpkg-2026.10.16 diff /usr/include/x86_64-linux-gnu/qt5/QtSerialBus/qcanbusdevice.h /usr/include/x86_64-linux-gnu/qt6/QtSerialBus/qcanbusdevice.h
```
