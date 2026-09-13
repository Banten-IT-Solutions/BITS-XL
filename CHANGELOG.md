## [1.2.3](https://github.com/Banten-IT-Solutions/BITS-XL/compare/v1.2.2...v1.2.3) (2026-09-13)


### Bug Fixes

* show page title above tab strip + spacing for refresh buttons ([f393d29](https://github.com/Banten-IT-Solutions/BITS-XL/commit/f393d297b358cc9842d6f45ad5c885fe698295b5))

## [1.2.2](https://github.com/Banten-IT-Solutions/BITS-XL/compare/v1.2.1...v1.2.2) (2026-09-13)


### Bug Fixes

* drop non-existent python3-json/hashlib/ssl deps (bundled in python3-light) ([a6093e3](https://github.com/Banten-IT-Solutions/BITS-XL/commit/a6093e3007ab232f0cf1a356c2b75e8f3d30da2b))

## [1.2.1](https://github.com/Banten-IT-Solutions/BITS-XL/compare/v1.2.0...v1.2.1) (2026-09-13)


### Bug Fixes

* add python3-pip dep (postinst installs pycryptodome via pip) ([e5f7ff4](https://github.com/Banten-IT-Solutions/BITS-XL/commit/e5f7ff4e8d658187a81f30022a4c954822c622db))

# [1.2.0](https://github.com/Banten-IT-Solutions/BITS-XL/compare/v1.1.1...v1.2.0) (2026-09-13)


### Features

* rewrite backend in Python (Networks-Bot structure) ([6946ab5](https://github.com/Banten-IT-Solutions/BITS-XL/commit/6946ab51a65f5b6bf86b1b83f18ce852d75236f2))

## [1.1.1](https://github.com/Banten-IT-Solutions/BITS-XL/compare/v1.1.0...v1.1.1) (2026-09-13)


### Bug Fixes

* trigger bitsxl backend build via workflow_run (avoid GITHUB_TOKEN chained-workflow limit) ([6d1d9a6](https://github.com/Banten-IT-Solutions/BITS-XL/commit/6d1d9a66b5aa3573e2aacfa278feb81c44e94feb))

# [1.1.0](https://github.com/Banten-IT-Solutions/BITS-XL/compare/v1.0.0...v1.1.0) (2026-09-13)


### Features

* ship native bitsxl backend package ([8e7fed1](https://github.com/Banten-IT-Solutions/BITS-XL/commit/8e7fed1ed163074d195640b2185a634d4d37db0b))

# 1.0.0 (2026-09-13)


### Features

* BITS XL LuCI app (rebrand from luci-app-engsel) ([b3c127c](https://github.com/Banten-IT-Solutions/BITS-XL/commit/b3c127c258365e36dbab800f72a0b60fa0a7b20c))
* ship default XL API credentials in /etc/config/bitsxl ([52a0086](https://github.com/Banten-IT-Solutions/BITS-XL/commit/52a0086a50bf231d0cb4c629a77e4cbeb1536e76))

## 1.0.0 (2026-09-13)

### Features

* BITS XL LuCI app for OpenWrt — MyXL account, quota, store, checkout, notifications, and transaction status.
* LuCI app rebranded from `luci-app-engsel`, menu at `Services → BITS XL`.
* SDK-less `.ipk` + `.apk` build with semantic-release.
