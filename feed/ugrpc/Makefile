# this file is to integrate with OpenWrt package build

include $(TOPDIR)/rules.mk

PKG_NAME:=ugrpc
PKG_VERSION:=0.1
PKG_RELEASE=1

PKG_LICENSE:=Apache-2.0

# the following won't work as long as this file is included in the same
# repository, since the hash value cannot be set, so instead the
# CopyLocalSource logic below copies the source directly from the feed
#PKG_SOURCE_PROTO:=git
#PKG_SOURCE_URL=https://github.com/sparky8512/ugrpc.git
#PKG_MIRROR_HASH:=TBD
#PKG_SOURCE_VERSION:=v$(PKG_VERSION)

CMAKE_INSTALL:=1

PKG_BUILD_DEPENDS:=libjson-c libubox uclient
HOST_BUILD_DEPENDS:=libjson-c libubox

HOST_BUILD_PREFIX:=$(STAGING_DIR_HOST)

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/host-build.mk
include $(INCLUDE_DIR)/cmake.mk

CMAKE_OPTIONS += \
	-DENABLE_UCLIENT_SERVICE=ON

CMAKE_HOST_OPTIONS += \
	-DENABLE_UCLIENT_SERVICE=OFF \
	-DENABLE_PRETTY_COLOR=ON \
	-DENABLE_LEAK_CHECK=ON \
	-DCMAKE_SKIP_RPATH=FALSE \
	-DCMAKE_INSTALL_RPATH="/lib:${STAGING_DIR_HOST}/lib"

define Package/ugrpc
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Lightweight gRPC(-Web) client
  DEPENDS:=+libjson-c +libubox +libuclient
endef

define Package/ugrpc/description
  A lightweight tool for encoding and decoding Protocol Buffers data
  to and from JSON strings for use with gRPC services, including an
  end-to-end client for use with gRPC-Web services.
endef

define Package/ugrpc/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/ugrpc $(1)/usr/bin/
endef

SOURCE_DIR:=../..
define CopyLocalSource
	mkdir -p $(1)
	$(CP) $(SOURCE_DIR)/CMakeLists.txt $(SOURCE_DIR)/src $(1)
	$(Build/Patch)
endef
define Build/Prepare
  $(call CopyLocalSource,$(PKG_BUILD_DIR))
endef
define Host/Prepare
  $(call CopyLocalSource,$(HOST_BUILD_DIR))
endef

$(eval $(call BuildPackage,ugrpc))
$(eval $(call HostBuild))
