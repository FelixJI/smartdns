#!/bin/sh

set -eu

API_ROOT="https://api.github.com/repos"
DOWNLOAD_ROOT="https://github.com"
WORK_DIR="/tmp/smartdns-upgrade.$$"
BACKUP_FILE=""

say()
{
	printf '%s\n' "$*"
}

die()
{
	printf '错误：%s\n' "$*" >&2
	exit 1
}

cleanup()
{
	case "$WORK_DIR" in
	/tmp/smartdns-upgrade.*)
		rm -rf "$WORK_DIR"
		;;
	esac
}

trap cleanup 0
trap 'exit 1' HUP INT TERM

download()
{
	url="$1"
	destination="$2"

	if command -v uclient-fetch >/dev/null 2>&1; then
		uclient-fetch -q -O "$destination" "$url"
	elif command -v wget >/dev/null 2>&1; then
		wget -q -O "$destination" "$url"
	elif command -v curl >/dev/null 2>&1; then
		curl -fsSL -o "$destination" "$url"
	else
		die "未找到 uclient-fetch、wget 或 curl，无法下载文件。"
	fi
}

choose_source()
{
	say "SmartDNS 一键升级"
	say "1) 官方版本（pymumu/smartdns 最新正式版）"
	say "2) fork 修改版（FelixJI/smartdns 最新构建）"
	printf '请选择升级源 [1/2]：'
	IFS= read -r choice || die "未读取到升级源选择。"

	case "$choice" in
	1)
		SOURCE_LABEL="官方版本"
		REPOSITORY="pymumu/smartdns"
		RELEASE_API="$API_ROOT/$REPOSITORY/releases/latest"
		;;
	2)
		SOURCE_LABEL="fork 修改版"
		REPOSITORY="FelixJI/smartdns"
		RELEASE_API="$API_ROOT/$REPOSITORY/releases?per_page=1"
		;;
	*)
		die "无效选择，请重新运行并输入 1 或 2。"
		;;
	esac
}

detect_package_manager()
{
	if command -v apk >/dev/null 2>&1; then
		PACKAGE_MANAGER="apk"
		PACKAGE_EXTENSION="apk"
	elif command -v opkg >/dev/null 2>&1; then
		PACKAGE_MANAGER="opkg"
		PACKAGE_EXTENSION="ipk"
	else
		die "仅支持使用 opkg 或 apk 的 OpenWrt 系统。"
	fi
}

detect_architecture()
{
	machine="$(uname -m)"
	case "$machine" in
	x86_64|amd64)
		PACKAGE_ARCH="x86_64"
		;;
	i386|i486|i586|i686)
		PACKAGE_ARCH="x86"
		;;
	aarch64|arm64)
		PACKAGE_ARCH="aarch64"
		;;
	armv5*|armv6*|armv7*|armv8*|arm)
		PACKAGE_ARCH="arm"
		;;
	mips64el|mipsel*)
		PACKAGE_ARCH="mipsel"
		;;
	mips64|mips*)
		PACKAGE_ARCH="mips"
		;;
	*)
		die "不支持的 CPU 架构：$machine。"
		;;
	esac
}

is_installed()
{
	package_name="$1"
	case "$PACKAGE_MANAGER" in
	opkg)
		opkg status "$package_name" 2>/dev/null | grep -q '^Status:.* installed'
		;;
	apk)
		apk info 2>/dev/null | grep -qx "$package_name"
		;;
	esac
}

detect_luci_package()
{
	has_full_luci=0
	has_lite_luci=0
	is_installed luci-app-smartdns && has_full_luci=1
	is_installed luci-app-smartdns-lite && has_lite_luci=1

	if [ "$has_full_luci" -eq 1 ] && [ "$has_lite_luci" -eq 1 ]; then
		die "检测到完整和 lite 两种 LuCI 包，请先卸载其中一个再升级。"
	elif [ "$has_full_luci" -eq 1 ]; then
		LUCI_PACKAGE="luci-app-smartdns"
	elif [ "$has_lite_luci" -eq 1 ]; then
		LUCI_PACKAGE="luci-app-smartdns-lite"
	else
		LUCI_PACKAGE=""
	fi
}

read_release_metadata()
{
	release_file="$WORK_DIR/release.json"
	say "正在读取 $SOURCE_LABEL 的 latest Release..."
	download "$RELEASE_API" "$release_file" || die "无法读取 GitHub Release 信息。"

	RELEASE_TAG="$(tr ',' '\n' < "$release_file" | sed -n 's/.*"tag_name":"\([^"]*\)".*/\1/p' | head -n 1)"
	[ -n "$RELEASE_TAG" ] || die "Release 信息中没有可用的版本标签，可能触发了 GitHub API 限制。"

	ASSET_NAMES="$(tr ',' '\n' < "$release_file" | sed -n 's/.*"name":"\([^"]*\)".*/\1/p')"
	CORE_ASSET="$(printf '%s\n' "$ASSET_NAMES" | grep "^smartdns\\..*\\.$PACKAGE_ARCH-openwrt-all\\.$PACKAGE_EXTENSION$" | head -n 1 || true)"
	[ -n "$CORE_ASSET" ] || die "Release $RELEASE_TAG 中没有 $PACKAGE_ARCH 的 OpenWrt .$PACKAGE_EXTENSION 核心包。"

	VERSION="${CORE_ASSET#smartdns.}"
	VERSION="${VERSION%.$PACKAGE_ARCH-openwrt-all.$PACKAGE_EXTENSION}"

	if [ -n "$LUCI_PACKAGE" ]; then
		LUCI_ASSET="$(printf '%s\n' "$ASSET_NAMES" | grep "^$LUCI_PACKAGE\\..*\\.$PACKAGE_EXTENSION$" | head -n 1 || true)"
		[ -n "$LUCI_ASSET" ] || die "Release $RELEASE_TAG 中没有 $LUCI_PACKAGE 的 .$PACKAGE_EXTENSION 包。"
	else
		LUCI_ASSET=""
	fi
}

backup_configuration()
{
	backup_items=""
	[ -e /etc/config/smartdns ] && backup_items="$backup_items etc/config/smartdns"
	[ -d /etc/smartdns ] && backup_items="$backup_items etc/smartdns"
	[ -n "$backup_items" ] || return 0

	BACKUP_FILE="/tmp/smartdns-config-backup-$(date +%Y%m%d-%H%M%S).tar.gz"
	# Paths are fixed OpenWrt configuration locations and intentionally split here.
	(cd / && tar -czf "$BACKUP_FILE" $backup_items) || die "无法备份现有配置。"
	say "配置已备份到：$BACKUP_FILE"
}

download_packages()
{
	RELEASE_BASE="$DOWNLOAD_ROOT/$REPOSITORY/releases/download/$RELEASE_TAG"
	CORE_FILE="$WORK_DIR/$CORE_ASSET"
	say "正在下载 SmartDNS $VERSION（$PACKAGE_ARCH）..."
	download "$RELEASE_BASE/$CORE_ASSET" "$CORE_FILE" || die "核心包下载失败。"

	if [ -n "$LUCI_ASSET" ]; then
		LUCI_FILE="$WORK_DIR/$LUCI_ASSET"
		download "$RELEASE_BASE/$LUCI_ASSET" "$LUCI_FILE" || die "$LUCI_PACKAGE 下载失败。"
	else
		LUCI_FILE=""
	fi
}

install_packages()
{
	set -- "$CORE_FILE"
	[ -n "$LUCI_FILE" ] && set -- "$@" "$LUCI_FILE"

	case "$PACKAGE_MANAGER" in
	opkg)
		opkg install --force-reinstall --force-downgrade "$@"
		;;
	apk)
		apk add --allow-untrusted --repositories-file /dev/null "$@"
		;;
	esac
}

restart_service()
{
	[ -x /etc/init.d/smartdns ] || die "软件包已安装，但未找到 SmartDNS 服务脚本。"
	/etc/init.d/smartdns enable
	/etc/init.d/smartdns restart

	if command -v service >/dev/null 2>&1; then
		service smartdns running >/dev/null 2>&1 || die "软件包已安装，但 SmartDNS 服务未能正常运行。"
	else
		/etc/init.d/smartdns running >/dev/null 2>&1 || die "软件包已安装，但 SmartDNS 服务未能正常运行。"
	fi
}

main()
{
	choose_source
	[ "$(id -u)" -eq 0 ] || die "请使用 root 用户运行此脚本。"
	[ -r /etc/openwrt_release ] || die "未检测到 OpenWrt 系统。"

	mkdir -m 700 "$WORK_DIR"
	detect_package_manager
	detect_architecture
	detect_luci_package
	read_release_metadata
	backup_configuration
	download_packages
	install_packages || die "软件包安装失败；现有配置备份位于 ${BACKUP_FILE:-未生成}。"
	restart_service

	say ""
	say "SmartDNS 升级成功。"
	say "选择源：$SOURCE_LABEL（$REPOSITORY）"
	say "版本号：$VERSION"
	say "Release：$RELEASE_TAG"
	[ -n "$LUCI_PACKAGE" ] && say "LuCI：$LUCI_PACKAGE"
	[ -n "$BACKUP_FILE" ] && say "配置备份：$BACKUP_FILE"
}

main "$@"
