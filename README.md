<div align="center">
  <h1>BITS XL</h1>
  <p>
    <a href="https://bits.co.id">
      <img src="https://img.shields.io/badge/Banten%20IT%20Solutions-BITS%20XL-00C853?style=for-the-badge&logo=x&logoColor=white" alt="BITS XL" />
    </a>
  </p>
  <p>
    Drop-in LuCI app for XL (myXL) on OpenWrt &mdash; manage your XL account, quota, store, and payments from the web interface with a modern BITS theme.
  </p>
  <br>
  <p>
    <img src="https://img.shields.io/badge/OpenWrt-00A1E9?style=flat&logo=openwrt&logoColor=white" alt="OpenWrt" />
    <img src="https://img.shields.io/badge/LuCI-3D5780?style=flat" alt="LuCI" />
    <img src="https://img.shields.io/badge/XL-FF2D55?style=flat" alt="XL" />
    <img src="https://img.shields.io/badge/Python-3776AB?style=flat&logo=python&logoColor=white" alt="Python" />
    <img src="https://img.shields.io/badge/JavaScript-F7DF1E?style=flat&logo=javascript&logoColor=black" alt="JavaScript" />
    <img src="https://img.shields.io/badge/license-MIT-green?style=flat" alt="MIT License" />
  </p>
</div>

---

## ✨ Features

| Feature                   | Description                                                                                                     |
| ------------------------- | --------------------------------------------------------------------------------------------------------------- |
| **Dashboard**             | Account, quota, and package segments as cards, consistent with the native LuCI look.                            |
| **Store**                 | Shop families, point redemption, custom packages, bookmarks, and checkout with QRIS/ewallet/pulsa.               |
| **Riwayat**               | Transaction history, `Kuota History`, and logs viewer with recheck actions.                                    |
| **Notifikasi**            | XL notifications and detail viewer.                                                                             |
| **Settings**              | Environment (API keys) and custom Decoy pairs managed from LuCI.                                                |
| **Python Backend**        | `bitsxl` CLI (urllib + pycryptodome AES + hmac signing).                                                        |
| **Services Menu**         | Lives under `Services → BITS XL`.                                                                               |
| **Automated Release**     | semantic-release builds `.ipk` + `.apk` and publishes a GitHub Release on every conventional commit.            |

## 🛠️ Tech Stack

| Layer        | Technology                                                                        |
| ------------ | --------------------------------------------------------------------------------- |
| **Runtime**  | OpenWrt (LuCI + UCI)                                                              |
| **Backend**  | Python 3 (`bitsxl`) + `rpcd` ACL + `uci`                                          |
| **Frontend** | JavaScript (LuCI AMD views loaded via `require`)                                  |
| **Build**    | `bash` + `tar` (ipk) + `apk-tools v3` (apk) — no SDK                             |
| **Release**  | semantic-release + GitHub Actions                                                 |

---

## 📁 Project Structure

```text
BITS-XL/
├── .github/
│   ├── dependabot.yml             # dep update (npm + actions)
│   └── workflows/
│       └── release.yml            # semantic-release + build .ipk/.apk + attach asset
├── bitsxl/                        # ← paket BACKEND (Python)
│   ├── control                    # ipk/apk metadata (+ Depends)
│   ├── conffiles                  # preserve /etc/config/bitsxl
│   ├── postinst                   # clear cache + best-effort pip install pycryptodome
│   └── root/
│       ├── usr/bin/bitsxl         # backend CLI (Python 3)
│       └── etc/config/bitsxl      # UCI default config (credentials)
├── luci-app-bitsxl/               # ← paket UI, Depends: bitsxl
│   ├── control                    # ipk/apk metadata
│   ├── postinst                   # clear LuCI cache
│   ├── htdocs/luci-static/resources/view/bitsxl/*.js   # LuCI AMD views
│   └── root/usr/share/{luci,rpcd}/...                  # menu + ACL
├── scripts/
│   └── prepare.js                 # sync version + build .ipk/.apk
├── build.sh                       # SDK-less .ipk + .apk packer (2 packages)
├── package.json                   # semantic-release + plugins
├── package-lock.json              # npm lockfile (npm ci)
├── .releaserc.json                # release plugins (git + github)
└── LICENSE
```

---

## 🚀 Quick Start

### Prerequisites

- An OpenWrt device (22.03+), with `python3` + internet access for the XL API.
- `pycryptodome` (installed best-effort via `pip` in the backend `postinst`).

### 1. Download

Grab package dari [Releases](https://github.com/Banten-IT-Solutions/BITS-XL/releases), lalu copy ke device:
- `.ipk` untuk OpenWrt 22.03–24.10 (`opkg`)
- `.apk` untuk OpenWrt 25.12+ (`apk`)

### 2. Install

```sh
# backend (wajib)
opkg install bitsxl_<version>_all.ipk          # 22.03–24.10
# halaman LuCI (opsional, Depends: bitsxl)
opkg install luci-app-bitsxl_<version>_all.ipk

# OpenWrt 25.12+ (apk)
apk add bitsxl_<version>_all.apk
apk add luci-app-bitsxl_<version>_all.apk
```

### 3. Use

Open LuCI (`Services → BITS XL`) and sign in with your XL number.

The CLI accepts local `08...` as well as canonical `628...`:

```sh
bitsxl --help
bitsxl login 081234567890
bitsxl otp 081234567890 123456
```

### 4. Configuration

Default XL API credentials ship with the backend package at `/etc/config/bitsxl`. Optional Decoy overrides (`Services → BITS XL → Settings → Decoy`):

`DECOY_PREPAID_FAMILY_CODE`, `DECOY_PREPAID_PACKAGE_NUMBER`, `DECOY_PRIORITAS_FAMILY_CODE`, `DECOY_PRIORITAS_PACKAGE_NUMBER`, `DECOY_PRIOHYBRID_FAMILY_CODE`, `DECOY_PRIOHYBRID_PACKAGE_NUMBER` — leave blank to use built-in decoys.

---

## 🏗️ Build

SDK-less `.ipk` + `.apk`. Butuh `apk-tools v3` (`apk mkpkg`) di `PATH`. Di CI sudah di-cache; lokal install `apk-tools` 3.x atau set `APK_BIN=<path/to/apk>`.

```sh
./build.sh
# output: dist/bitsxl_<version>_all.ipk
#         dist/luci-app-bitsxl_<version>_all.ipk
#         dist/*.apk (bila apk-tools tersedia)
```

> `.ipk` = outer `tar.gz` (debian-binary + control.tar.gz + data.tar.gz). `.apk` = ADB container via `apk mkpkg`.

---

## 🚀 Release

Releases are automated with [semantic-release](https://semantic-release.gitbook.io) and [Conventional Commits](https://www.conventionalcommits.org). Write a conventional commit:

| Commit                           | Bump       |
| -------------------------------- | ---------- |
| `fix: ...`                       | patch      |
| `feat: ...`                      | minor      |
| `BREAKING CHANGE:` in body       | major      |

Push to `main` dan workflow build `.ipk` + `.apk` (build.sh + apk-tools) lalu publish ke GitHub Release.

---

## 📄 License

Distributed under the MIT License. See `LICENSE`.

---

<div align="center">
  <strong>BITS XL</strong> Developed with ❤️ by <a href="https://bits.co.id"><strong>Banten IT Solutions</strong></a>
</div>