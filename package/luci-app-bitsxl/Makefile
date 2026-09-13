include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-bitsxl
PKG_VERSION:=1.2.0
PKG_RELEASE:=1
PKG_LICENSE:=MIT
PKG_MAINTAINER:=Banten IT Solutions <support@bits.co.id>
PKGARCH:=all

include $(INCLUDE_DIR)/package.mk

define Build/Prepare
endef

define Build/Configure
endef

define Package/luci-app-bitsxl
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=LuCI app for BITS XL MyXL quota client
  URL:=https://github.com/Banten-IT-Solutions/BITS-XL
  DEPENDS:=+bitsxl +luci-base +rpcd +rpcd-mod-file +cgi-io
endef

define Package/luci-app-bitsxl/description
 Full JavaScript LuCI frontend for the BITS XL backend CLI.
endef

define Build/Compile
endef

define Package/luci-app-bitsxl/install
	$(CP) $(CURDIR)/root/* $(1)/
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view/bitsxl
	$(INSTALL_DATA) $(CURDIR)/htdocs/luci-static/resources/view/bitsxl/*.js $(1)/www/luci-static/resources/view/bitsxl/
endef

define Package/luci-app-bitsxl/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || rm -rf /tmp/luci-indexcache /tmp/luci-modulecache
exit 0
endef

define Package/luci-app-bitsxl/postrm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || rm -rf /tmp/luci-indexcache /tmp/luci-modulecache
exit 0
endef

$(eval $(call BuildPackage,luci-app-bitsxl))
