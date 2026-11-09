# qt-cansub

A Qt 5/6 integration for the CANsub interfaces from [CSS Electronics](https://csselectronics.com/). The project source is released under the MIT license (see LICENSE).

# Table of Contents

* [Installation](#installation)
    * [Dependencies](#dependencies)
* [Connecting to a device](#connecting-to-a-device)
    * [Connecting by IP](#connecting-by-ip)
* [Supported configuration options](#supported-configuration-options)
* [Limitations](#limitations)
* [Building](#building)
    * [Windows](#windows)
    * [Containerized building](#containerized-building)
* [Third-party dependencies](#third-party-dependencies)

## Installation

The plugin needs to be accessible to the Qt framework at one of the locations it searches for common plugins. Usually, the application root is one of these places, and can be used by creating a `plugins/canbus` folder under the root of the target Qt application and place the plugin inside. Below is an example structure for a Qt 5 application on Windows:

```
Qt5Application/
├── main.exe                    <-- Your application executable
├── plugins/                    <-- Create this folder in your application root
│   └── canbus/                 <-- Create this sub-folder
│       └── CANsub-Qt5.dll      <-- Place the built plugin artifact here
└── ...
```

Note that this can vary depending on the target executable. If a folder named `canbus` is already present at another path and contains other CAN system plugins, use this path instead for installation.

Plugins for versions 5 and 6 of the Qt framework are *not* interchangeable. Remember to use the version of the plugin matching the framework version of the application.

* Qt 5: CANsub-Qt5
* Qt 6: CANsub-Qt6

### Dependencies

The plugin depends on the following Qt modules:

* Core
* Network
* SerialBus

**These must all be present at runtime for the plugin to work**. Core and SerialBus are likely always present in any application the plugin would be used with. All other dependencies are linked statically into the plugin.

The repository contains a pre-built plugin for Qt 5:

* Windows x64 using Qt 5.15.18.
* Linux x64 based on Ubuntu 22.04 and Qt 5.15.3.
* Linux aarch64 based on Ubuntu 22.04 and Qt 5.15.3.

## Connecting to a device

The plugin is designed around using auto-discovered devices over mDNS. The devices found by `QCanBus::availableDevices()` have interface names of the form `<mDNS hostname>@<channel>`, e.g. `aabbccdd-eth.local@1`, and can be used as they are.

An interface name can also be written by hand, as `<device ID>-usb.local@<channel>` for a connection over USB or `<device ID>-eth.local@<channel>` for a connection over Ethernet, e.g. `aabbccdd-usb.local@1`. The device ID is the 8 hex digits identifying the CANsub device, and the channel is the CAN channel on the device, from 1 to 4. The `.local` suffix is required.

The connection to the device is always encrypted, and the device certificate is verified against the hostname.

### Connecting by IP

If mDNS discovery fails, e.g. due to no mDNS forwarding between subnets, it is possible to connect via IP instead. The syntax will then be `IPv4 address@channel` instead of `mDNS hostname@channel`, e.g. `10.123.45.1@1`. In this mode, the communication between the CANsub device and the application is still secure, but since there is no hostname information, only the root certificate is verified, not which device it belongs to.

To verify the device as well, the syntax `IPv4 address:device ID@channel` must be used. This should be just the device ID, e.g. `aabbccdd` instead of `aabbccdd-eth.local`, so the resulting string becomes `10.123.45.1:aabbccdd@1`. The device ID can also be set with `CANsubConfigurationKeyExpectedHostname`, see below.

## Supported configuration options

The bit rates are configured via the regular `QCanBusDevice` [configuration interface](https://doc.qt.io/qt-6/qcanbusdevice.html#setConfigurationParameter). The CANsub interface always runs CAN FD, so it requires a configuration for both the nominal and the data bit rate. The plugin calculates the bit timing for `QCanBusDevice::BitRateKey` and `QCanBusDevice::DataBitRateKey` with the sample point of `CANsubConfigurationKeySamplePoint` (80 % by default). A combination is rejected when connecting if the device cannot reach both bit rates within 0.1 %. If the data bit rate is left out, it defaults to the nominal bit rate or 1000000, whichever is higher.

If the `QCanBusDevice::CanFdKey` is set to false, CAN FD frames are still received by the CANsub device, but the plugin does not forward them, and writing a CAN FD frame fails.

The device does not support loopback mode, so `QCanBusDevice::LoopbackKey` cannot be changed from the default value of false.

The CANsub device can forward transmit acknowledgement. To ignore these, the `QCanBusDevice::ReceiveOwnKey` can be set to false. The default value is false.

Aside from these standard Qt CAN configuration options, a series of additional user keys are supported.

* `CANsubConfigurationKeyListenOnly` (QCanBusDevice::UserKey + 1) - When set to true the device cannot write to the CAN network, neither via transmission of CAN frames nor via acknowledgement of received frames from other nodes. Defaults to false.
* `CANsubConfigurationKeyAutoReset` (QCanBusDevice::UserKey + 2) - When set to true the device will automatically reset the bus in the case of Bus-Off. Defaults to true.
* `CANsubConfigurationKeyErrorFramesEnabled` (QCanBusDevice::UserKey + 3) - When set to true the device will forward error frames. Defaults to false.
* `CANsubConfigurationKeySamplePoint` (QCanBusDevice::UserKey + 4) - The sample point used to calculate the bit timing from the bit rates, for both the nominal and the data phase. 80 % is the default and encoded as 8000. 81.5 % would be 8150. Values from 0 to 10000 are accepted.
* `CANsubConfigurationKeyBitTiming` (QCanBusDevice::UserKey + 5) - Manually configure the full bit timing (`CANsubQt::BitTiming`) for the nominal phase. Overrides the bit rates if set alongside `CANsubConfigurationKeyBitTimingData`. Is not set by default.
* `CANsubConfigurationKeyBitTimingData` (QCanBusDevice::UserKey + 6) - Manually configure the full bit timing (`CANsubQt::BitTiming`) for the data phase. Overrides the bit rates if set alongside `CANsubConfigurationKeyBitTiming`. Is not set by default.
* `CANsubConfigurationKeyUpdateTime` (QCanBusDevice::UserKey + 7) - Controls if the plugin sets the device clock on connection, if it is off by 1 s or more and the device is not synchronized via PTP. Defaults to true.
* `CANsubConfigurationKeyBlockingWrite` (QCanBusDevice::UserKey + 8) - Controls if the plugin soft-blocks when writing to the CAN bus if the remote is not able to enqueue any more frames for transmission, up to 1 s. Defaults to false.
* `CANsubConfigurationKeyExpectedHostname` (QCanBusDevice::UserKey + 9) - When connecting via an IPv4 address, the device certificate is validated against this name instead of the IP. The value is the device ID (e.g. `aabbccdd`) or one of the device hostnames (e.g. `aabbccdd-eth.local`), and other names are rejected. An empty string only validates the root certificate. Defaults to the device ID of the interface name, or an empty string if it has none. Ignored when connecting by hostname.
* `CANsubConfigurationKeyDisableCertificateValidation` (QCanBusDevice::UserKey + 10) - Deprecated, and has no effect.

All configuration options are applied when connecting. Changes while connected take effect on the next connection.

## Limitations

Due to the network nature of the CANsub devices, some function calls cannot return immediately. This is most pronounced when getting the available devices on the network, as there is no signal in the Qt CAN bus framework to notify the application when additional devices are discovered. To give a better user experience, the plugin blocks the calling thread for 2 seconds while attempting to discover new devices. This only happens if using the `availableDevices` function on the plugin. If creating a CAN bus directly from a hostname/address, or if using a previously acquired name, this 2 second block will not occur.

## Building

The project has the following third-party dependencies when building, besides the Qt 5 or Qt 6 framework:

* [c-cpp-sdk-cansub](https://github.com/CSS-Electronics/c-cpp-sdk-cansub) - The CANsub library, which handles the communication with the device. CMake downloads it at the tag `2026-11-09` when configuring the project, and links it statically into the plugin. To build against a local checkout instead, e.g. without network access, add `-DFETCHCONTENT_SOURCE_DIR_CANSUB=<path to c-cpp-sdk-cansub>` to the CMake configuration.
* [QMdnsEngine](https://github.com/nitroshare/qmdnsengine) - Used for the mDNS device discovery and hostname resolution. It is shipped with the project under `thirdparty/qmdnsengine`, see [Third-party dependencies](#third-party-dependencies).

The project is configured to be built using CMake (version 3.30 or higher) with [vcpkg](https://github.com/microsoft/vcpkg) in manifest mode. The current targeted version of vcpkg is [2026.10.16](https://github.com/microsoft/vcpkg/releases/tag/2026.10.16). When using the manifest, Qt 5 will be installed, but only on Windows platforms. On Linux, usage of the system libraries instead is recommended. If building without vcpkg, ensure Qt and its SerialBus module are installed. Downloading c-cpp-sdk-cansub requires `git`.

By default, the project will attempt to build a plugin for Qt 5. If the target version of the Qt framework is 6, set the CMake option `USE_QT6` to `ON` instead of the default value `OFF`, and ensure the required libraries are installed on the system. For Windows and vcpkg, this may require editing the `vcpkg.json` file to reflect the Qt version change. The name of the built artifact will reflect the used Qt version: `CANsub-Qt5` or `CANsub-Qt6`.

### Windows

The plugin must be built with a compiler compatible with the target Qt application. On Windows this is normally [Microsoft Visual C++](https://visualstudio.microsoft.com/). The build also needs [CMake](https://cmake.org/) (version 3.30 or higher), [Git](https://git-scm.com/) and the [Ninja](https://ninja-build.org/) generator used by the preset below.

These can be installed with [winget](https://learn.microsoft.com/windows/package-manager/winget/):

```
winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
winget install Kitware.CMake
winget install Git.Git
winget install Ninja-build.Ninja
```

The first command installs the MSVC compiler and Windows SDK (a full Visual Studio install with the *Desktop development with C++* workload works equally well). Run the build commands from the `x64 Native Tools Command Prompt for VS` so MSVC, CMake, Git and Ninja are all on `PATH`.

Install [vcpkg](https://github.com/microsoft/vcpkg) if not already installed. It is recommended to use a short path without spaces. The installation can either be via `git` or by downloading the source archive for the [2026.10.16](https://github.com/microsoft/vcpkg/archive/refs/tags/2026.10.16.zip) tag. Install by running `.\bootstrap-vcpkg.bat` in the vcpkg root directory.

Add a `CMakeUserPresets.json` file to the root of the project based on the following template:

```json
{
  "version": 3,
  "cmakeMinimumRequired": {
    "major": 3,
    "minor": 30,
    "patch": 0
  },
  "configurePresets": [
    {
      "name": "MSVC",
      "hidden": true,
      "displayName": "MSVC",
      "generator": "Ninja",
      "description": "Base for using the MSVC Toolchain",
      "toolchainFile": "<path_to_vcpkg>\\scripts\\buildsystems\\vcpkg.cmake",
      "cacheVariables": {
        "VCPKG_TARGET_TRIPLET": "x64-windows-release"
      }
    },
    {
      "name": "Settings",
      "hidden": true,
      "displayName": "Settings",
      "description": "Additional shared settings",
      "binaryDir": "${sourceDir}/.build/${presetName}"
    },
    {
      "name": "Release",
      "hidden": true,
      "description": "Configuration of CMake when building release targets",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release"
      },
      "inherits": [
        "Settings"
      ]
    },
    {
      "name": "configDefaultReleaseMSVCQt5",
      "displayName": "Default - Release",
      "description": "Default - Release",
      "cacheVariables": {
        "USE_QT6": "OFF"
      },
      "inherits": [
        "MSVC",
        "Release"
      ]
    },
    {
      "name": "configDefaultReleaseMSVCQt6",
      "displayName": "Default - Release",
      "description": "Default - Release",
      "cacheVariables": {
        "USE_QT6": "ON"
      },
      "inherits": [
        "MSVC",
        "Release"
      ]
    }
  ]
}
```

Replace the `<path_to_vcpkg>` with the installation path of vcpkg. To use a local checkout of c-cpp-sdk-cansub, also add `"FETCHCONTENT_SOURCE_DIR_CANSUB": "<path_to_c_cpp_sdk_cansub>"` to the `cacheVariables` of the `MSVC` preset. The project can now be built via CMake. Run the following from the `x64 Native Tools Command Prompt for VS` (or the equivalent for your platform):

```
cmake --preset configDefaultReleaseMSVCQt5
cmake --build .build/configDefaultReleaseMSVCQt5
```

Note: If you have changed the name in the `CMakeUserPresets.json` file from `configDefaultReleaseMSVCQt5` or the value of the `binaryDir` entry, you need to update it in the commands. Once done, the built artifact should be at `.build/configDefaultReleaseMSVCQt5/plugins/canbus/CANsub-Qt5.dll`. To build the Qt 6 variant, use the `configDefaultReleaseMSVCQt6` preset instead.

### Containerized building

The project has a recipe for generating a container to build the project in. This is stored in `containers/Containerfile`. The following commands assumes `podman` is the container runtime of choice, but can be replaced with `docker` or any other container runtime that supports the same syntax. The container file supports both x86 64-bit (x64) and Arm 64-bit (aarch64) architectures, which is configured at container image build time via a build argument (`VCPKG_TRIPLET`). The following is an example of building the container for x64 and aarch64, and using it to build the plugin:

```
# For x64:
podman build --build-arg VCPKG_TRIPLET=x64-linux-release -t cansub-builder-x64:vcpkg-2026.10.16 -f containers/Containerfile .
podman run --rm -v .:/project:Z cansub-builder-x64:vcpkg-2026.10.16 bash -c "cmake -B build -S . -GNinja -DCMAKE_BUILD_TYPE=Release -DVCPKG_TARGET_TRIPLET=x64-linux-release && cmake --build build"

# For aarch64:
podman build --build-arg VCPKG_TRIPLET=arm64-linux-release -t cansub-builder-aarch64:vcpkg-2026.10.16 -f containers/Containerfile .
podman run --rm -v .:/project:Z cansub-builder-aarch64:vcpkg-2026.10.16 bash -c "cmake -B build -S . -GNinja -DCMAKE_BUILD_TYPE=Release -DVCPKG_TARGET_TRIPLET=arm64-linux-release && cmake --build build"
```

This will map the current directory to the `/project` directory inside the container, and build the plugin in the sub-folder `build`. Once done, the plugin is in `build/plugins/canbus`.

To build against a local checkout of c-cpp-sdk-cansub, mount it into the container as well, e.g. by adding `-v <path to c-cpp-sdk-cansub>:/c-cpp-sdk-cansub:ro,z` to `podman run` and `-DFETCHCONTENT_SOURCE_DIR_CANSUB=/c-cpp-sdk-cansub` to the CMake configuration.

To build for Qt 6, set the `USE_QT6` option to `ON` (`-DUSE_QT6=ON`).

The artifact will run on everything compatible with the base image (Ubuntu 22.04):

* Qt 5 (5.15.3)
* Qt 6 (6.2.4)

## Third-party dependencies

The project ships a version of [QMdnsEngine](https://github.com/nitroshare/qmdnsengine) with the following two modifications:

* The CMakeLists.txt configuration files have been altered to use the same settings as the main project and to allow for easier switching between Qt 5 and Qt 6.
* All interfaces are enumerated when performing mDNS actions, based on [PR #18](https://github.com/nitroshare/qmdnsengine/pull/18).

The license for all files under the `thirdparty/qmdnsengine` path is that of `thirdparty/qmdnsengine/LICENSE.txt`.

The plugin links [c-cpp-sdk-cansub](https://github.com/CSS-Electronics/c-cpp-sdk-cansub) statically, which is released under the MIT license. c-cpp-sdk-cansub bundles [Mbed TLS](https://github.com/Mbed-TLS/mbedtls), parts of [Zephyr](https://github.com/zephyrproject-rtos/zephyr) and [picohttpparser](https://github.com/h2o/picohttpparser), so these are part of the built plugin as well.

The licenses of all third-party code in the built plugin are collected in `THIRD_PARTY_NOTICES.md`.
