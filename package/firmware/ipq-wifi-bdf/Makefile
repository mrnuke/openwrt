include $(TOPDIR)/rules.mk

PKG_NAME:=ipq-wifi-bdf
PKG_SOURCE_DATE:=2023-02-16
PKG_SOURCE_VERSION:=e478c7ceb568559bebc7e3ac997458e8cad0f811
PKG_MIRROR_HASH:=bbd89ad722ea2fb7dfdcf4b75776e104e6ac58559f84ad3a13e7cd09501cfef6
PKG_RELEASE:=$(AUTORELEASE)

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/mrnuke/ipq-wifi-bdf.git

PKG_MAINTAINER:=Alexandru Gagniuc <mr.nuke.me@gmail.com>

include $(INCLUDE_DIR)/package.mk

define Package/ipq-bdf-default
  SECTION:=firmware
  CATEGORY:=Firmware
  URL:=$(PKG_SOURCE_URL)
endef

comma:=,
BOARD_IDS_AND_NAMES:= \
	dynalink_dl-wrx36,Dynalink::DL-WRX36 \
	edgecore_eap102,Edgecore::EAP102 \
	edimax_cax1800,Edimax::CAX1800 \
	qnap_301w,QNAP::301w \
	redmi_ax6,Redmi::AX6 \
	xiaomi_ax3600,Xiaomi::AX3600 \
	xiaomi_ax9000,Xiaomi::AX9000 \
	zyxel_nbg7815,Zyxel::NBG7815 \

define install-bdf
	$(INSTALL_DIR) $(1)/lib/firmware/ath11k/$(3)
	$(INSTALL_DATA) \
		$(PKG_BUILD_DIR)/$(3)/$(subst _,$(comma),$(2))-board-2.bin \
		$(1)/lib/firmware/ath11k/$(3)/
endef

define add-bdf-package
  define Package/ath11k-wifi-$(1)
    $(call Package/ipq-bdf-default)
    SUBMENU:=ath11k Board-Data-Files
    TITLE:=ath11k board file for $(2)
  endef

  define Package/ath11k-wifi-$(1)/install-overlay
	  $(call install-bdf,$$(1),$(1),IPQ8074/hw2.0)
  endef
endef

define Build/Compile
endef

# Somebody who knows make, please make this more readable!!!
$(foreach ID_NAME,$(BOARD_IDS_AND_NAMES), \
	$(eval BOARD_ID := $(word 1,$(subst $(comma), ,$(ID_NAME)))) \
	$(eval NAME_MANGLED := $(word 2,$(subst $(comma), ,$(ID_NAME)))) \
	$(eval NAME := $(subst ::, ,$(NAME_MANGLED))) \
	$(eval $(call add-bdf-package,$(BOARD_ID),$(NAME))) \
	$(eval $(call BuildPackage,ath11k-wifi-$(BOARD_ID))) \
)
