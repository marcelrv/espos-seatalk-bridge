# Changelog

## [0.5.0](https://github.com/marcelrv/espos-seatalk-bridge/compare/v0.4.3...v0.5.0) (2026-11-27)


### Added

* add runtime hardware config, /api/v1/seatalk endpoint, and release defaults ([012d8aa](https://github.com/marcelrv/espos-seatalk-bridge/commit/012d8aa05ad7549e1e2b04925eb2c2ef712b695b))
* **bench:** show 8-bit internal heap, largest block and OTA progress ([006a26e](https://github.com/marcelrv/espos-seatalk-bridge/commit/006a26e5f4cae9887b46a9d45c307c477699ad52))


### Fixed

* configure the TX pin once, silencing the gpio conflict warnings ([50bb887](https://github.com/marcelrv/espos-seatalk-bridge/commit/50bb887f6799b017dd5143ce13ba9e9467b86603))
* **espos_seatalk:** address CodeRabbit review on example manifest, Kconfig, and README pins ([682a1ea](https://github.com/marcelrv/espos-seatalk-bridge/commit/682a1eacb7f70d6b1c66268c5d5a1a7e55d78097))
* **main:** always release the old default TX pin with a pull-down ([6611258](https://github.com/marcelrv/espos-seatalk-bridge/commit/66112586445283a66d1a3d5dac96c8849945e882))
* **main:** guard default TX pin reset and preserve RX/TX conflict check ([062d908](https://github.com/marcelrv/espos-seatalk-bridge/commit/062d908ced8747258ab6edfce59b956d2cdcbef5))
* **main:** keep the lights pin off a released default TX pin ([562eb2e](https://github.com/marcelrv/espos-seatalk-bridge/commit/562eb2e7b02e2b13decde90ea7ac2ef58f965a01))
* **main:** refuse strapping and USB pins from the hardware config ([4e404e5](https://github.com/marcelrv/espos-seatalk-bridge/commit/4e404e5e1c140913c4cdbf9c6534a5695270daf4))
* **main:** safe TX pin handling, build-default pins in the config page ([5fd194f](https://github.com/marcelrv/espos-seatalk-bridge/commit/5fd194f1b269fb862b0b3151452af6484834795b))
* **phy:** idle TX HIGH for a non-inverting interface ([6b61fe2](https://github.com/marcelrv/espos-seatalk-bridge/commit/6b61fe2f6433c499cf76451327e7e8a2cd24a4b5))
* **seatalk:** release the bus at boot with the configured inversion ([71aa734](https://github.com/marcelrv/espos-seatalk-bridge/commit/71aa734c2a87dec12c7a77d464247c45d7fb6bed))
* **sk:** keep the bus-load baseline current while Signal K is down ([867828f](https://github.com/marcelrv/espos-seatalk-bridge/commit/867828f3eed660d81961bbe0198e2206ac107a22))
* **sk:** publish diagnostics only while the Signal K stream is connected ([d1576e4](https://github.com/marcelrv/espos-seatalk-bridge/commit/d1576e4cee31820abbef88f9fa2f17be05e0a1dc))


### Changed

* consolidate st_* layers into components/espos_seatalk ([a1566e1](https://github.com/marcelrv/espos-seatalk-bridge/commit/a1566e146f28a99ee7eedabbea8938fd86a86e64))

## Changelog

All notable changes to `espos-seatalk-bridge` are managed by
[release-please](https://github.com/googleapis/release-please) from
conventional commit messages on `main`.
