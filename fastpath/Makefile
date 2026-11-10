# OpenWrt package for the Archer C1200 v2 NAT fast path (see fastpath.c).
# Lives in the build tree as package/kernel/fastpath; needs patch 323
# (bgmac_rx_hook, bgmac_tx_dirty_hint, bgmac_rx_lazy_sync).
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=fastpath
PKG_RELEASE:=2
PKG_LICENSE:=GPL-2.0

include $(INCLUDE_DIR)/package.mk

define KernelPackage/fastpath
  SUBMENU:=Network Support
  TITLE:=CTF-like NAT fast path for bgmac + Broadcom-tagged DSA
  DEPENDS:=@TARGET_bcm53xx +kmod-nf-flow +kmod-nft-offload
  FILES:=$(PKG_BUILD_DIR)/fastpath.ko
  AUTOLOAD:=$(call AutoProbe,fastpath)
endef

define KernelPackage/fastpath/description
  Forwards established IPv4/IPv6 flows from fw4's flowtable straight from the
  bgmac receive ring to its transmit ring, rewriting NAT, MACs and the
  Broadcom switch tag in place. Installs an nftables include that adds the
  switch ports to the flowtable so flows get direct transmit info.
endef

define KernelPackage/fastpath/install
	$(INSTALL_DIR) $(1)/etc/nftables.d
	$(INSTALL_DATA) ./files/20-fastpath.nft $(1)/etc/nftables.d/20-fastpath.nft
	$(INSTALL_DIR) $(1)/usr/libexec $(1)/etc/hotplug.d/net $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/fastpath-wifi.sh $(1)/usr/libexec/fastpath-wifi.sh
	$(INSTALL_DATA) ./files/30-fastpath-wifi $(1)/etc/hotplug.d/net/30-fastpath-wifi
	$(INSTALL_BIN) ./files/fastpath.uci-defaults $(1)/etc/uci-defaults/90-fastpath-wifi
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,fastpath))
