# Changelog

All notable changes to this project will be documented in this file.

## 2026.09.28 (API 05.00)

### Added

* Support for direct IP connections without hostname

### Changed

* Base API support increased to 05.00
* Qt WebSocket replaced with a custom implementation to avoid hard dependency on Qt WebSocket library being available

### Removed

* Support for disabling certificate validation via `CANsubConfigurationKeyDisableCertificateValidation` has been deprecated

## 2026.09.04 (internal) (API 04.00)

### Changed

* Base API support increased to 04.00

## 2026.07.14 (API 03.00)

Initial release

### Known issues

Connectivity issues are not detected
