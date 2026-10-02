# qt-cansub

A Qt 5/6 integration for the CANsub interfaces from [CSS Electronics](https://csselectronics.com/). The project source is released under the MIT license (see LICENSE).

# Table of Contents

* [Installation](#installation)
    * [Dependencies](#dependencies)
* [Limitations](#limitations)
* [Supported configuration options](#supported-configuration-options)
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
* StateMachine (Qt 6)

**These must all be present at runtime for the plugin to work**. Core and SerialBus are likely always present in any application the plugin would be used with.

The repository contains a pre-built plugin for Qt 5:

* Windows x64 using Qt 5.15.18.
* Linux x64 based on Ubuntu 22.04 and Qt 5.15.3.
* Linux aarch64 based on Ubuntu 22.04 and Qt 5.15.3.

## Limitations

* The plugin is currently unable to detect a network issue/power loss at the remote device.

## Connecting by IP

The plugin is designed around using auto-discovered devices over mDNS. If this fails, e.g. due to no mDNS forwarding between subnets, it is possible to connect via IP instead. The syntax will then be `IPv4 address@channel` instead of `mDNS hostname@channel`, e.g. `10.123.45.1@1`. In this mode, the communication between the CANsub device and the application is still secure, but since there is no hostname information, the hostname verification is disabled. To enforce hostname verification as well, the syntax `IPv4 address:expected device id@channel` must be used. This should be just the device ID, e.g. `11223344` instead of `11223344-eth.local`, so the resulting string becomes `10.123.45.1:11223344@1`.

## Supported configuration options

The plugin has support for the following combination of bit-rates built-in:

| Bitrate (kHz) | Data bitrate (FD BRS) (kHz) |
|---------------|-----------------------------|
| 50000         | 1000000                     |
| 100000        | 1000000                     |
| 100000        | 2000000                     |
| 125000        | 1000000                     |
| 125000        | 2000000                     |
| 125000        | 4000000                     |
| 200000        | 1000000                     |
| 200000        | 2000000                     |
| 200000        | 4000000                     |
| 250000        | 1000000                     |
| 250000        | 2000000                     |
| 250000        | 4000000                     |
| 500000        | 1000000                     |
| 500000        | 2000000                     |
| 500000        | 4000000                     |
| 800000        | 1000000                     |
| 800000        | 2000000                     |
| 800000        | 4000000                     |
| 1000000       | 1000000                     |
| 1000000       | 2000000                     |
| 1000000       | 4000000                     |

All these are supported with a sample point at 80 %, and can be configured via the regular `QCanBusDevice` [configuration interface](https://doc.qt.io/qt-6/qcanbusdevice.html#setConfigurationParameter). The CANsub interface requires configuration for both classical and CAN-FD data rates. If configuring the bitrate and leaving out the CAN-FD bitrate, a default value of 1000000 will be used for CAN-FD. If the `QCanBusDevice::CanFdKey` is set to false, the frames will still be received by the CANsub device, but the plugin does not forward them.

The device does not support loopback mode, so `QCanBusDevice::LoopbackKey` cannot be changed from the default value of false.

The CANsub device can forward transmit acknowledgement. To ignore these, the `QCanBusDevice::ReceiveOwnKey` can be set to false. The default value is false.

Aside from these standard Qt CAN configuration options, a series of additional user keys are supported.

* `CANsubConfigurationKeyListenOnly` (QCanBusDevice::UserKey + 1) - When set to true the device cannot write to the CAN network, neither via transmission of CAN frames nor via acknowledgement of received frames from other nodes. Defaults to false.
* `CANsubConfigurationKeyAutoReset` (QCanBusDevice::UserKey + 2) - When set to true the device will automatically reset the bus in the case of Bus-Off. Defaults to true.
* `CANsubConfigurationKeyErrorFramesEnabled` (QCanBusDevice::UserKey + 3) - When set to true the device will forward error frames. Defaults to false.
* `CANsubConfigurationKeySamplePoint` (QCanBusDevice::UserKey + 4) - Set the sample point to use when using the bitrate values. 80 % is the default and encoded as 8000. 81.5 % would be 8150. Currently, only values with a sample point of 80 % (8000) are supported.
* `CANsubConfigurationKeyBitTiming` (QCanBusDevice::UserKey + 5) - Manually configure the full bit-timing configuration for normal CAN operation. Overrides bitrate if set alongside `CANsubConfigurationKeyBitTimingData`. Is not set by default.
* `CANsubConfigurationKeyBitTimingData` (QCanBusDevice::UserKey + 6) - Manually configure the full bit-timing configuration for the data phase of CAN operation. Overrides data bitrate if set alongside `CANsubConfigurationKeyBitTiming`. Is not set by default.
* `CANsubConfigurationKeyUpdateTime` (QCanBusDevice::UserKey + 7) - Controls if the plugin synchronizes the remote clock on connection. Defaults to true.
* `CANsubConfigurationKeyBlockingWrite` (QCanBusDevice::UserKey + 8) - Controls if the plugin soft-blocks when writing to the CAN bus if the remote is not able to enqueue any more frames for transmition, up to 1 s. Defaults to false.
* `CANsubConfigurationKeyExpectedHostname` (QCanBusDevice::UserKey + 9) - When connecting via a raw IP address, the remote certificate is validated against this hostname instead of the IP. The CANsub presents a certificate issued for its device ID and mDNS hostnames, so this allows full certificate validation even when connecting by IP. Ignored when connecting by hostname. Is not set by default. If this is not set and connecting by IP, the remote certificate hostname is not validated, but the root certificate is still verified.
* `CANsubConfigurationKeyDisableCertificateValidation` (QCanBusDevice::UserKey + 10) - Deprecated.

## Building

The project has the following additional third party dependencies when building:

* [Boost](https://www.boost.org/)
    * [CRC](https://www.boost.org/library/1.90.0/crc/) (1.90.0 or later)
    * [Endian](https://www.boost.org/library/1.90.0/endian/) (1.90.0 or later)
    * [Beast](https://www.boost.org/library/1.90.0/beast/) (1.90.0 or later)
* [Qt 5 or Qt 6](https://www.qt.io/)

The project is configured to be built using CMake (version 3.30 or higher) with [vcpkg](https://github.com/microsoft/vcpkg) in manifest mode to handle these dependencies. The current targeted version of vcpkg is [2026.06.01](https://github.com/microsoft/vcpkg/releases/tag/2026.06.01). When using the manifest, Qt 5 will be installed, but only on Windows platforms. On Linux, usage of the system libraries instead is recommended. If building without vcpkg, ensure all the dependencies above are installed.

By default, the project will attempt to build a plugin for Qt 5. If the target version of the Qt framework is 6, set the CMake option `USE_QT6` to `ON` instead of the default value `OFF`, and ensure the required libraries are installed on the system. For Windows and vcpkg, this may require editing the `vcpkg.json` file to reflect the Qt version change. The name of the built artifact will reflect the used Qt version: `CANsub-Qt5` or `CANsub-Qt6`.

### Windows

The plugin must be built with a compiler compatible with the target Qt application. On Windows this is normally [Microsoft Visual C++](https://visualstudio.microsoft.com/). The build also needs [CMake](https://cmake.org/) (version 3.30 or higher) and the [Ninja](https://ninja-build.org/) generator used by the preset below.

These can be installed with [winget](https://learn.microsoft.com/windows/package-manager/winget/):

```
winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
winget install Kitware.CMake
winget install Ninja-build.Ninja
```

The first command installs the MSVC compiler and Windows SDK (a full Visual Studio install with the *Desktop development with C++* workload works equally well). Run the build commands from the `x64 Native Tools Command Prompt for VS` so MSVC, CMake and Ninja are all on `PATH`.

Install [vcpkg](https://github.com/microsoft/vcpkg) if not already installed. It is recommended to use a short path without spaces. The installation can either be via `git` or by downloading the source archive for the [2026.06.01](https://github.com/microsoft/vcpkg/archive/refs/tags/2026.06.01.zip) tag. Install by running `.\bootstrap-vcpkg.bat` in the vcpkg root directory.

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

Replace the `<path_to_vcpkg>` with the installation path of vcpkg. The project can now be built via CMake. Run the following from the `x64 Native Tools Command Prompt for VS` (or the equivalent for your platform):

```
cmake --preset configDefaultReleaseMSVCQt5
cmake --build .build/configDefaultReleaseMSVCQt5
```

Note: If you have changed the name in the `CMakeUserPresets.json` file from `configDefaultReleaseMSVCQt5` or the value of the `binaryDir` entry, you need to update it in the commands. Once done, the built artifact should be at `.build/configDefaultReleaseMSVCQt5/CANsub-Qt5.dll`. To build the Qt 6 variant, use the `configDefaultReleaseMSVCQt6` preset instead.

### Containerized building

The project has a recipe for generating a container to build the project in. This is stored in `containers/Containerfile`. The following commands assumes `podman` is the container runtime of choice, but can be replaced with `docker` or any other container runtime that supports the same syntax. The container file supports both x86 64-bit (x64) and Arm 64-bit (aarch64) architectures, which is configured at container image build time via a build argument (`VCPKG_TRIPLET`). The following is an example of building the container for x64 and aarch64, and using it to build the plugin:

```
# For x64:
podman build --build-arg VCPKG_TRIPLET=x64-linux-release -t cansub-builder-x64:vcpkg-2026.06.01 -f containers/Containerfile .
podman run --rm -v .:/project:Z cansub-builder-x64:vcpkg-2026.06.01 bash -c "cmake -B build -S . -GNinja -DCMAKE_BUILD_TYPE=Release -DVCPKG_TARGET_TRIPLET=x64-linux-release && cmake --build build"

# For aarch64:
podman build --build-arg VCPKG_TRIPLET=arm64-linux-release -t cansub-builder-aarch64:vcpkg-2026.06.01 -f containers/Containerfile .
podman run --rm -v .:/project:Z cansub-builder-aarch64:vcpkg-2026.06.01 bash -c "cmake -B build -S . -GNinja -DCMAKE_BUILD_TYPE=Release -DVCPKG_TARGET_TRIPLET=arm64-linux-release && cmake --build build"
```

This will map the current directory to the `/project` directory inside the container, and build the plugin in the sub-folder `build`. Once done, extract the plugin from the build directory.

To build for Qt 6, set the `USE_QT6` option to `ON` (`-DUSE_QT6=ON`).

The artifact will run on everything compatible with the base image (Ubuntu 22.04):

* Qt 5 (5.15.3)
* Qt 6 (6.2.4)

## Limitations

Due to the network nature of the CANsub devices, some function calls cannot return immediately. This is most pronounced when getting the available devices on the network, as there is no signal in the Qt CAN bus framework to notify the application when additional devices are discovered. To give a better user experience, the plugin blocks the calling thread for 2 seconds while attempting to discover new devices. This only happens if using the `availableDevices` function on the plugin. If creating a CAN bus directly from a hostname/address, or if using a previously acquired name, this 2 second block will not occur. 

## Third-party dependencies

The project ships a version of [QMdnsEngine](https://github.com/nitroshare/qmdnsengine) with the following two modifications:
* The CMakeLists.txt configuration files have been altered to use the same settings as the main project and to allow for easier switching between Qt 5 and Qt 6.
* All interfaces are enumerated when performing mDNS actions, based on [PR #18](https://github.com/nitroshare/qmdnsengine/pull/18).
The license for all files under the `thirdparty/qmdnsengine` path is that of `thirdparty/qmdnsengine/LICENSE.txt`.

The project ships with a pre-generated version of the OpenAPI integration for Qt under `thirdparty/OAPI`. The license of this follows that of the qt-cansub plugin project.
