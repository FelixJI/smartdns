#!/bin/sh

set -eu

TEST_DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
REPO_DIR="$(CDPATH= cd -- "$TEST_DIR/.." && pwd)"
TEST_TMP="$(mktemp -d "${TMPDIR:-/tmp}/smartdns-upgrade-test.XXXXXX")"
MOCK_BIN="$TEST_TMP/bin"
DOWNLOAD_LOG="$TEST_TMP/download.log"
DESTINATION="$TEST_TMP/package.ipk"

cleanup()
{
	case "$TEST_TMP" in
	*/smartdns-upgrade-test.*)
		rm -rf "$TEST_TMP"
		;;
	esac
}

trap cleanup 0
mkdir -p "$MOCK_BIN"

cat > "$MOCK_BIN/uclient-fetch" <<'EOF'
#!/bin/sh
printf 'uclient-fetch\n' >> "$DOWNLOAD_LOG"
exit_code="${MOCK_UCLIENT_EXIT:-0}"
while [ "$#" -gt 0 ]; do
	case "$1" in
	-O)
		output="$2"
		shift 2
		;;
	*)
		shift
		;;
	esac
done
[ "$exit_code" -ne 0 ] || printf 'downloaded\n' > "$output"
exit "$exit_code"
EOF

cat > "$MOCK_BIN/wget" <<'EOF'
#!/bin/sh
printf 'wget\n' >> "$DOWNLOAD_LOG"
exit_code="${MOCK_WGET_EXIT:-0}"
while [ "$#" -gt 0 ]; do
	case "$1" in
	-O)
		output="$2"
		shift 2
		;;
	*)
		shift
		;;
	esac
done
[ "$exit_code" -ne 0 ] || printf 'downloaded\n' > "$output"
exit "$exit_code"
EOF

cat > "$MOCK_BIN/curl" <<'EOF'
#!/bin/sh
printf 'curl\n' >> "$DOWNLOAD_LOG"
exit_code="${MOCK_CURL_EXIT:-0}"
while [ "$#" -gt 0 ]; do
	case "$1" in
	-o)
		output="$2"
		shift 2
		;;
	*)
		shift
		;;
	esac
done
[ "$exit_code" -ne 0 ] || printf 'downloaded\n' > "$output"
exit "$exit_code"
EOF

chmod +x "$MOCK_BIN/uclient-fetch" "$MOCK_BIN/wget" "$MOCK_BIN/curl"
export DOWNLOAD_LOG
PATH="$MOCK_BIN:$PATH"
export PATH
SMARTDNS_UPGRADE_SOURCE_ONLY=1
export SMARTDNS_UPGRADE_SOURCE_ONLY

. "$REPO_DIR/package/openwrt/upgrade.sh"

assert_download()
{
	expected="$1"
	label="$2"
	: > "$DOWNLOAD_LOG"
	rm -f "$DESTINATION"

	if ! download "https://example.invalid/package.ipk" "$DESTINATION"; then
		printf 'FAIL: %s: download returned failure\n' "$label" >&2
		exit 1
	fi

	actual="$(cat "$DOWNLOAD_LOG")"
	[ "$actual" = "$expected" ] || {
		printf 'FAIL: %s: expected downloader order:\n%s\nactual:\n%s\n' "$label" "$expected" "$actual" >&2
		exit 1
	}

	grep -qx 'downloaded' "$DESTINATION" || {
		printf 'FAIL: %s: downloader did not create the destination\n' "$label" >&2
		exit 1
	}
}

MOCK_WGET_EXIT=0
MOCK_CURL_EXIT=0
MOCK_UCLIENT_EXIT=0
export MOCK_WGET_EXIT MOCK_CURL_EXIT MOCK_UCLIENT_EXIT
assert_download "wget" "prefer wget"

MOCK_WGET_EXIT=4
assert_download "$(printf 'wget\ncurl')" "fall back from wget to curl"

MOCK_CURL_EXIT=5
assert_download "$(printf 'wget\ncurl\nuclient-fetch')" "fall back from curl to uclient-fetch"

printf 'PASS: downloader preference and fallbacks are correct\n'
