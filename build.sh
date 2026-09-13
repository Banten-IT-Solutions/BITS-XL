#!/usr/bin/env bash
# Pack luci-app-bitsxl: .ipk (opkg) + .apk (apk) tanpa SDK.
# .ipk = tar.gz luar (debian-binary + control.tar.gz + data.tar.gz)
# .apk = apk-tools v3 `mkpkg` (butuh binary `apk` di PATH / env APK_BIN)
set -euo pipefail

PKG=luci-app-bitsxl
ctrl="$PKG/control"
pkg_ver=$(awk -F': ' '/^Version:/{print $2; exit}' "$ctrl")
pkg_desc=$(awk -F': ' '/^Description:/{print $2; exit}' "$ctrl")
pkg_depends=$(awk -F': ' '/^Depends:/{print $2; exit}' "$ctrl" | tr ',' ' ')

out_ipk="dist/${PKG}_${pkg_ver}_all.ipk"
out_apk="dist/${PKG}_${pkg_ver}_all.apk"

rm -rf .build dist
mkdir -p dist .build/root .build/control .build/outer

# htdocs -> /www ; root -> /
cp -a "$PKG/htdocs/." .build/root/www/
cp -a "$PKG/root/."   .build/root/

cp "$ctrl" .build/control/control
if [ -f "$PKG/postinst" ]; then
	cp "$PKG/postinst" .build/control/postinst
	chmod 755 .build/control/postinst
fi
if [ -f "$PKG/conffiles" ]; then
	cp "$PKG/conffiles" .build/control/conffiles
fi

tar czf .build/data.tar.gz --owner=0 --group=0 -C .build/root .
tar czf .build/control.tar.gz --owner=0 --group=0 -C .build/control .
printf '2.0\n' > .build/debian-binary

cp .build/debian-binary .build/control.tar.gz .build/data.tar.gz .build/outer/
tar czf "$out_ipk" -C .build/outer .

# ===== .apk (apk-tools v3) =====
APK_BIN="${APK_BIN:-apk}"
if command -v "$APK_BIN" >/dev/null 2>&1; then
	APK_ARGS=(
		mkpkg
		--info "name:${PKG}"
		--info "version:${pkg_ver}-r0"
		--info "arch:noarch"
		--info "description:${pkg_desc}"
		--info "license:MIT"
		--info "maintainer:Banten IT Solutions <support@bits.co.id>"
		--info "depends:${pkg_depends}"
	)
	if [ -f "$PKG/postinst" ]; then
		APK_ARGS+=(--script "post-install:$PKG/postinst")
	fi
	APK_ARGS+=(--files .build/root --output "$out_apk")
	"$APK_BIN" "${APK_ARGS[@]}"
	echo "Built: $out_apk"
else
	echo "skip .apk: binary 'apk' not found (set APK_BIN)"
fi

rm -rf .build
echo "Built: $out_ipk"