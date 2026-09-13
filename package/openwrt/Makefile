include $(TOPDIR)/rules.mk

PKG_NAME:=bitsxl
PKG_VERSION:=1.2.0
PKG_RELEASE:=1
PKG_LICENSE:=MIT
PKG_MAINTAINER:=Banten IT Solutions <support@bits.co.id>

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/bitsxl
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Native MyXL CLI backend for OpenWrt
  URL:=https://github.com/Banten-IT-Solutions/BITS-XL
  DEPENDS:=+libmbedtls +libcurl +ca-bundle
endef

define Package/bitsxl/description
 Native OpenWrt backend for MyXL account, quota, store, checkout, and session handling.
endef

define Build/Prepare
	$(RM) -r $(PKG_BUILD_DIR)
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(CURDIR)/src $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) -std=c99 -Wall -Wextra -Werror=misleading-indentation -Wno-unused-parameter -ffunction-sections -fdata-sections \
		$(TARGET_LDFLAGS) -Wl,--gc-sections \
		-o $(PKG_BUILD_DIR)/bitsxl $(PKG_BUILD_DIR)/src/bitsxl.c -lmbedcrypto -lcurl
endef

define Package/bitsxl/install
	$(INSTALL_DIR) $(1)/usr/bin $(1)/usr/libexec/bitsxl
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/bitsxl $(1)/usr/libexec/bitsxl/bitsxl
	$(INSTALL_BIN) $(CURDIR)/files/bitsxl.wrapper $(1)/usr/bin/bitsxl
	$(INSTALL_DIR) $(1)/etc $(1)/etc/config $(1)/etc/bitsxl
	$(INSTALL_CONF) $(CURDIR)/files/bitsxl.env.example $(1)/etc/bitsxl/env.example
	$(INSTALL_CONF) $(CURDIR)/files/bitsxl.env.example $(1)/etc/bitsxl.env.example
	$(INSTALL_CONF) $(CURDIR)/files/bitsxl.env $(1)/etc/bitsxl/.env
	$(INSTALL_CONF) $(CURDIR)/files/bitsxl.env $(1)/etc/bitsxl.env
	$(INSTALL_CONF) $(CURDIR)/files/bitsxl.config $(1)/etc/config/bitsxl
endef

define Package/bitsxl/conffiles
/etc/config/bitsxl
/etc/bitsxl/.env
/etc/bitsxl.env
endef

define Package/bitsxl/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
mkdir -p /etc/bitsxl || exit 1
if [ ! -f /etc/bitsxl/.env ]; then
	if [ -f /etc/bitsxl.env ]; then
		cp /etc/bitsxl.env /etc/bitsxl/.env || exit 1
	fi
fi
if command -v uci >/dev/null 2>&1; then
	uci -q get bitsxl.config >/dev/null || uci set bitsxl.config=bitsxl || exit 1
	for key in BASE_API_URL BASE_CIAM_URL BASIC_AUTH AX_FP_KEY UA API_KEY ENCRYPTED_FIELD_KEY XDATA_KEY AX_API_SIG_KEY X_API_BASE_SECRET DECOY_PREPAID_FAMILY_CODE DECOY_PREPAID_PACKAGE_NUMBER DECOY_PRIORITAS_FAMILY_CODE DECOY_PRIORITAS_PACKAGE_NUMBER DECOY_PRIOHYBRID_FAMILY_CODE DECOY_PRIOHYBRID_PACKAGE_NUMBER; do
		[ -n "$$(uci -q get bitsxl.config.$${key})" ] && continue
		value="$$(sed -n "s/^$${key}=//p" /etc/bitsxl.env /etc/bitsxl/.env 2>/dev/null | tail -n1)"
		if [ -n "$${value}" ]; then
			uci set bitsxl.config.$${key}="$${value}" || exit 1
		fi
	done
	uci commit bitsxl || exit 1
	umask 077
	tmp=/etc/bitsxl/.env.$$$$
	stamp_tmp=/etc/bitsxl/.uci.sha256.$$$$
	cleanup() {
		rm -f "$${tmp}" "$${stamp_tmp}"
	}
	trap cleanup 0
	trap 'cleanup; exit 1' 1 2 3 15
	if ! {
		for key in BASE_API_URL BASE_CIAM_URL BASIC_AUTH AX_FP_KEY UA API_KEY ENCRYPTED_FIELD_KEY XDATA_KEY AX_API_SIG_KEY X_API_BASE_SECRET DECOY_PREPAID_FAMILY_CODE DECOY_PREPAID_PACKAGE_NUMBER DECOY_PRIORITAS_FAMILY_CODE DECOY_PRIORITAS_PACKAGE_NUMBER DECOY_PRIOHYBRID_FAMILY_CODE DECOY_PRIOHYBRID_PACKAGE_NUMBER; do
			value="$$(uci -q get bitsxl.config.$${key} 2>/dev/null)" || value=
			[ -n "$${value}" ] || continue
			printf '%s=%s\n' "$${key}" "$${value}" || exit 1
		done
	} > "$${tmp}"; then
		exit 1
	fi
	mv "$${tmp}" /etc/bitsxl/.env || exit 1
	config_sig="$$(sha256sum < /etc/config/bitsxl 2>/dev/null)" || config_sig=
	if [ -n "$${config_sig}" ]; then
		printf '%s\n' "$${config_sig}" > "$${stamp_tmp}" || exit 1
		mv "$${stamp_tmp}" /etc/bitsxl/.uci.sha256 || exit 1
	fi
	rm -rf /tmp/bitsxl-cache
	tmp=
	stamp_tmp=
	trap - 0 1 2 3 15
fi
exit 0
endef

$(eval $(call BuildPackage,bitsxl))
