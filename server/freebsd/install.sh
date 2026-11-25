#!/bin/sh
#
# Backrooms Iris - dedicated server setup for FreeBSD (TUI).
#
#   sudo sh server/freebsd/install.sh            # run from inside the repo checkout
#
# Uses bsddialog (in the FreeBSD 14+ base system), falls back to dialog(1), and to plain
# text prompts when neither exists or with --plain.
#
#   --plain     text prompts instead of the TUI
#   --dry-run   build for real, but install into ./dryrun-root and only print the
#               system commands (pkg, pw, sysrc, service) instead of running them
#
# What "Install / update" does:
#   1. pkg install cmake + ninja if they're missing (clang is in base)
#   2. asks for the server settings (name, port, players, seed, starting level)
#   3. builds iris_server from this checkout (server only, no SDL/GL needed)
#   4. installs /usr/local/bin/iris_server, /usr/local/etc/iris/server.conf,
#      the rc.d service, a newsyslog rule, and an unprivileged "iris" user
#   5. enables + starts the service and checks it answers on its UDP port

set -u

IRIS_TITLE="Backrooms Iris server setup"
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)
BUILD_DIR="$SRC_DIR/build-server"
BUILD_LOG="${TMPDIR:-/tmp}/iris-server-build.log"

DRY_RUN=0
PLAIN=0
for arg in "$@"; do
	case "$arg" in
		--dry-run) DRY_RUN=1 ;;
		--plain) PLAIN=1 ;;
		-h|--help) sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "unknown option: $arg (try --help)"; exit 2 ;;
	esac
done

if [ "$DRY_RUN" = 1 ]; then
	ROOT="$SRC_DIR/dryrun-root"
else
	ROOT=""
fi
PREFIX="$ROOT/usr/local"
BIN="$PREFIX/bin/iris_server"
CONF_DIR="$PREFIX/etc/iris"
CONF="$CONF_DIR/server.conf"
RC_SCRIPT="$PREFIX/etc/rc.d/iris_server"
NEWSYSLOG="$PREFIX/etc/newsyslog.conf.d/iris_server.conf"
LOGFILE="$ROOT/var/log/iris_server.log"
SVC_USER="iris"

# current settings (overwritten from an existing config)
S_NAME="Backrooms Iris server"
S_PORT="27315"
S_MAX="4"
S_SEED="0"
S_LEVEL="0"
S_DIFF="medium"
S_AUTOSTART="YES"

# ------------------------------------------------------------------ helpers

# System commands go through here so --dry-run can show them instead.
sys() {
	if [ "$DRY_RUN" = 1 ]; then
		echo "[dry-run] $*" >> "$SRC_DIR/dryrun-root/commands.log"
		return 0
	fi
	"$@"
}

have() { command -v "$1" >/dev/null 2>&1; }

DIALOG=""
if [ "$PLAIN" = 0 ]; then
	if have bsddialog; then DIALOG=bsddialog
	elif have dialog; then DIALOG=dialog
	fi
fi

# ui_msg TITLE TEXT
ui_msg() {
	if [ -n "$DIALOG" ]; then
		$DIALOG --backtitle "$IRIS_TITLE" --title " $1 " --msgbox "$2" 0 0
	else
		printf '\n== %s ==\n%b\n\n[press Enter] ' "$1" "$2"
		read -r _ </dev/tty
	fi
}

# ui_info TEXT  (non-blocking "please wait")
ui_info() {
	if [ -n "$DIALOG" ]; then
		$DIALOG --backtitle "$IRIS_TITLE" --title " working " --infobox "$1" 5 60
	else
		printf '%b\n' "$1"
	fi
}

# ui_yesno TITLE TEXT -> 0 yes, 1 no
ui_yesno() {
	if [ -n "$DIALOG" ]; then
		$DIALOG --backtitle "$IRIS_TITLE" --title " $1 " --yesno "$2" 0 0
		return $?
	fi
	printf '\n== %s ==\n%b\n[y/N] ' "$1" "$2"
	read -r _a </dev/tty
	case "$_a" in y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
}

# ui_textfile TITLE FILE
ui_textfile() {
	if [ -n "$DIALOG" ]; then
		$DIALOG --backtitle "$IRIS_TITLE" --title " $1 " --textbox "$2" 22 90
	else
		printf '\n== %s ==\n' "$1"
		tail -n 40 "$2"
		printf '\n[press Enter] '
		read -r _ </dev/tty
	fi
}

# ui_menu -> prints the chosen tag
ui_menu() {
	_status=$(service_status_line)
	if [ -n "$DIALOG" ]; then
		$DIALOG --backtitle "$IRIS_TITLE" --title " Backrooms Iris " --cancel-label "Exit" \
			--menu "Dedicated server for up to 4 players (co-op PvE).\n\nStatus: $_status\n\nUse the arrow keys and Enter." 20 66 9 \
			install  "Install / update the server" \
			settings "Change server settings" \
			status   "Service status + live check" \
			start    "Start the server" \
			stop     "Stop the server" \
			restart  "Restart the server" \
			log      "View the server log" \
			uninstall "Uninstall" \
			exit     "Exit" \
			3>&1 1>&2 2>&3
		return $?
	fi
	printf '\n== Backrooms Iris ==\nStatus: %s\n\n' "$_status" >&2
	printf '  1) Install / update\n  2) Change settings\n  3) Status\n  4) Start\n  5) Stop\n  6) Restart\n  7) View log\n  8) Uninstall\n  9) Exit\n> ' >&2
	read -r _c </dev/tty
	case "$_c" in
		1) echo install ;; 2) echo settings ;; 3) echo status ;; 4) echo start ;; 5) echo stop ;;
		6) echo restart ;; 7) echo log ;; 8) echo uninstall ;; *) echo exit ;;
	esac
}

# ----------------------------------------------------------------- settings

load_settings() {
	[ -f "$CONF" ] || return 0
	_get() { sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\"\{0,1\}\([^\"#]*\)\"\{0,1\}.*/\1/p" "$CONF" | head -n 1 | sed 's/[[:space:]]*$//'; }
	_v=$(_get name); [ -n "$_v" ] && S_NAME="$_v"
	_v=$(_get port); [ -n "$_v" ] && S_PORT="$_v"
	_v=$(_get max_players); [ -n "$_v" ] && S_MAX="$_v"
	_v=$(_get seed); [ -n "$_v" ] && S_SEED="$_v"
	_v=$(_get start_level); [ -n "$_v" ] && S_LEVEL="$_v"
	_v=$(_get difficulty); [ -n "$_v" ] && S_DIFF="$_v"
	return 0
}

is_uint() { case "$1" in ''|*[!0-9]*) return 1 ;; *) return 0 ;; esac; }

# validate S_* ; prints the problem, returns 1 if invalid
validate_settings() {
	if [ -z "$S_NAME" ]; then echo "The server name can't be empty."; return 1; fi
	case "$S_NAME" in *\"*) echo "The server name can't contain double quotes."; return 1 ;; esac
	if ! is_uint "$S_PORT" || [ "$S_PORT" -lt 1024 ] || [ "$S_PORT" -gt 65535 ]; then
		echo "Port must be a number from 1024 to 65535."; return 1
	fi
	if ! is_uint "$S_MAX" || [ "$S_MAX" -lt 1 ] || [ "$S_MAX" -gt 4 ]; then
		echo "Max players must be 1, 2, 3 or 4."; return 1
	fi
	if ! is_uint "$S_SEED" || [ "${#S_SEED}" -gt 10 ] || [ "$S_SEED" -gt 4294967295 ]; then
		echo "Seed must be a number from 0 to 4294967295 (0 = random world each start)."; return 1
	fi
	case "$S_LEVEL" in
		0|1|2|4|5|6|8|9|11|37|fun|FUN) ;;
		*) echo "Start level must be one of: 0 1 2 4 5 6 8 9 11 37 fun"; return 1 ;;
	esac
	case "$S_DIFF" in
		easy|medium|hard|nightmare|EASY|MEDIUM|HARD|NIGHTMARE) ;;
		*) echo "Difficulty must be one of: easy medium hard nightmare"; return 1 ;;
	esac
	return 0
}

# ask_settings -> 0 when the user confirmed valid settings, 1 on cancel
ask_settings() {
	while :; do
		if [ -n "$DIALOG" ]; then
			_out=$($DIALOG --backtitle "$IRIS_TITLE" --title " Server settings " \
				--form "Tab / arrows to move, Enter to confirm.\nSeed 0 = a fresh random world every start.\nLevels: 0 1 2 4 5 6 8 9 11 37 fun. Difficulty: easy medium hard nightmare." 18 74 7 \
				"Server name:"        1 2 "$S_NAME" 1 22 34 48 \
				"UDP port:"           2 2 "$S_PORT" 2 22 8 5 \
				"Max players (1-4):"  3 2 "$S_MAX"  3 22 3 1 \
				"World seed:"         4 2 "$S_SEED" 4 22 12 10 \
				"Start level:"        5 2 "$S_LEVEL" 5 22 4 3 \
				"Difficulty:"         6 2 "$S_DIFF" 6 22 10 9 \
				3>&1 1>&2 2>&3) || return 1
			# one value per line, in field order
			S_NAME=$(printf '%s\n' "$_out" | sed -n 1p)
			S_PORT=$(printf '%s\n' "$_out" | sed -n 2p)
			S_MAX=$(printf '%s\n' "$_out" | sed -n 3p)
			S_SEED=$(printf '%s\n' "$_out" | sed -n 4p)
			S_LEVEL=$(printf '%s\n' "$_out" | sed -n 5p)
			S_DIFF=$(printf '%s\n' "$_out" | sed -n 6p)
		else
			printf '\n== Server settings ==  (Enter keeps the value in [brackets])\n'
			printf 'Server name [%s]: ' "$S_NAME"; read -r _v </dev/tty; [ -n "$_v" ] && S_NAME="$_v"
			printf 'UDP port [%s]: ' "$S_PORT"; read -r _v </dev/tty; [ -n "$_v" ] && S_PORT="$_v"
			printf 'Max players 1-4 [%s]: ' "$S_MAX"; read -r _v </dev/tty; [ -n "$_v" ] && S_MAX="$_v"
			printf 'World seed, 0 = random [%s]: ' "$S_SEED"; read -r _v </dev/tty; [ -n "$_v" ] && S_SEED="$_v"
			printf 'Start level 0 1 2 4 5 6 8 9 11 37 fun [%s]: ' "$S_LEVEL"; read -r _v </dev/tty; [ -n "$_v" ] && S_LEVEL="$_v"
			printf 'Difficulty easy medium hard nightmare [%s]: ' "$S_DIFF"; read -r _v </dev/tty; [ -n "$_v" ] && S_DIFF="$_v"
		fi
		_problem=$(validate_settings) && break
		ui_msg "Check your settings" "$_problem"
	done
	if ui_yesno "Start on boot" "Start the Backrooms Iris server automatically when the machine boots?"; then
		S_AUTOSTART="YES"
	else
		S_AUTOSTART="NO"
	fi
	return 0
}

write_config() {
	mkdir -p "$CONF_DIR" || return 1
	cat > "$CONF.tmp" <<EOF
# Backrooms Iris dedicated server config (written by install.sh)
# Edit, then: service iris_server restart

name = "$S_NAME"
bind = 0.0.0.0
port = $S_PORT
max_players = $S_MAX
seed = $S_SEED          # 0 = new random world every start
start_level = $S_LEVEL   # 0 1 2 4 5 6 8 9 11 37 fun
difficulty = $S_DIFF     # easy medium hard nightmare
EOF
	mv "$CONF.tmp" "$CONF" && chmod 644 "$CONF"
}

# ------------------------------------------------------------------- system

lan_ip() {
	if have ifconfig; then
		ifconfig 2>/dev/null | awk '/inet / && $2 != "127.0.0.1" { print $2; exit }'
	else
		hostname -I 2>/dev/null | awk '{ print $1 }'
	fi
}

server_query() {
	[ -x "$BIN" ] || return 1
	"$BIN" --query "127.0.0.1:$S_PORT" 2>&1
}

service_status_line() {
	if [ ! -x "$BIN" ]; then echo "not installed"; return; fi
	if [ "$DRY_RUN" = 1 ]; then echo "installed (dry run)"; return; fi
	if service iris_server status >/dev/null 2>&1; then
		echo "running on UDP port $S_PORT"
	else
		echo "installed, stopped"
	fi
}

check_platform() {
	if [ "$DRY_RUN" = 0 ]; then
		if [ "$(uname -s)" != "FreeBSD" ]; then
			ui_msg "Not FreeBSD" "This installer sets up a FreeBSD rc.d service.\n\nOn Linux, build with:\n  cmake -S . -B build-server -DIRIS_BUILD_CLIENT=OFF\n  cmake --build build-server\n\n(or run this script with --dry-run to try it out)"
			exit 1
		fi
		if [ "$(id -u)" != 0 ]; then
			echo "Run me as root:  sudo sh $0   (or: su -m root -c 'sh $0')"
			exit 1
		fi
	fi
	if [ ! -f "$SRC_DIR/CMakeLists.txt" ] || [ ! -f "$SRC_DIR/src/server/main.cpp" ]; then
		echo "Can't find the Backrooms Iris source next to this script ($SRC_DIR)."
		exit 1
	fi
}

ensure_deps() {
	_missing=""
	have cmake || _missing="$_missing cmake"
	have ninja || _missing="$_missing ninja"
	if ! have c++ && ! have clang++; then
		ui_msg "No C++ compiler" "No C++ compiler found. FreeBSD ships clang in base; if you removed it:\n  pkg install llvm"
		return 1
	fi
	[ -z "$_missing" ] && return 0
	ui_yesno "Install build tools" "These packages are needed to build the server:\n\n  $_missing\n\nInstall them now with pkg?" || return 1
	ui_info "Installing$_missing with pkg..."
	# shellcheck disable=SC2086 # word-splitting the package list is intended
	if ! sys pkg install -y $_missing > "$BUILD_LOG" 2>&1; then
		ui_textfile "pkg failed" "$BUILD_LOG"
		return 1
	fi
	return 0
}

build_server() {
	ui_info "Building iris_server... (log: $BUILD_LOG)"
	_gen=""
	have ninja && _gen="-G Ninja"
	{
		echo "== configure"
		# shellcheck disable=SC2086
		cmake -S "$SRC_DIR" -B "$BUILD_DIR" $_gen -DCMAKE_BUILD_TYPE=Release \
			-DIRIS_BUILD_CLIENT=OFF -DIRIS_BUILD_SERVER=ON &&
		echo "== build" &&
		cmake --build "$BUILD_DIR" --target iris_server
	} > "$BUILD_LOG" 2>&1
	_rc=$?
	if [ $_rc -ne 0 ] || [ ! -x "$BUILD_DIR/iris_server" ]; then
		ui_textfile "Build failed (paste this to the devs)" "$BUILD_LOG"
		return 1
	fi
	return 0
}

ensure_user() {
	if [ "$DRY_RUN" = 0 ] && id "$SVC_USER" >/dev/null 2>&1; then return 0; fi
	sys pw useradd "$SVC_USER" -d /nonexistent -s /usr/sbin/nologin -c "Backrooms Iris server"
}

install_files() {
	mkdir -p "$PREFIX/bin" "$PREFIX/etc/rc.d" "$PREFIX/etc/newsyslog.conf.d" "$(dirname "$LOGFILE")" || return 1
	# stop first so the binary isn't busy
	[ "$DRY_RUN" = 0 ] && service iris_server onestop >/dev/null 2>&1
	install -m 755 "$BUILD_DIR/iris_server" "$BIN" || return 1
	install -m 555 "$SCRIPT_DIR/iris_server.rc" "$RC_SCRIPT" || return 1
	install -m 644 "$SCRIPT_DIR/newsyslog.conf" "$NEWSYSLOG" || return 1
	write_config || return 1
	touch "$LOGFILE"
	sys chown "$SVC_USER" "$LOGFILE"
	return 0
}

enable_and_start() {
	sys sysrc iris_server_enable="$S_AUTOSTART" >/dev/null
	if [ "$S_AUTOSTART" = "YES" ]; then
		sys service iris_server restart >/dev/null 2>&1
	else
		sys service iris_server onerestart >/dev/null 2>&1
	fi
}

firewall_note() {
	_note=""
	if [ "$DRY_RUN" = 0 ] && have sysrc; then
		if [ "$(sysrc -n pf_enable 2>/dev/null)" = "YES" ]; then
			_note="\n\npf is enabled. Allow the game port by adding this to /etc/pf.conf:\n  pass in quick proto udp to port $S_PORT\nthen: service pf reload"
		elif [ "$(sysrc -n firewall_enable 2>/dev/null)" = "YES" ]; then
			_note="\n\nipfw is enabled. Allow the game port, e.g.:\n  ipfw add allow udp from any to me $S_PORT"
		fi
	fi
	echo "$_note"
}

# ------------------------------------------------------------------ actions

do_install() {
	ensure_deps || return
	ask_settings || return
	build_server || return
	ui_info "Installing..."
	ensure_user
	if ! install_files; then
		ui_msg "Install failed" "Couldn't copy files into $PREFIX. Are you root?"
		return
	fi
	enable_and_start
	sleep 1
	if [ "$DRY_RUN" = 1 ]; then
		_check="(dry run: service not started; commands are in dryrun-root/commands.log)"
	else
		_check=$(server_query) || _check="No answer yet. Check the log from the menu."
	fi
	_ip=$(lan_ip)
	ui_msg "Done" "Backrooms Iris server installed.\n\nLAN address:  ${_ip:-<this machine>}:$S_PORT (UDP)\nStart on boot: $S_AUTOSTART\n\nLive check:\n$_check\n\nFiles:\n  $BIN\n  $CONF\n  $LOGFILE$(firewall_note)\n\nOnline play: forward UDP $S_PORT on your router to this machine."
}

do_settings() {
	if [ ! -x "$BIN" ]; then ui_msg "Not installed" "Install the server first."; return; fi
	ask_settings || return
	write_config
	sys sysrc iris_server_enable="$S_AUTOSTART" >/dev/null
	sys service iris_server onerestart >/dev/null 2>&1
	ui_msg "Saved" "Settings saved to $CONF and the server was restarted."
}

do_status() {
	_out="${TMPDIR:-/tmp}/iris-status.txt"
	{
		echo "== service"
		if [ "$DRY_RUN" = 1 ]; then echo "(dry run)"; else service iris_server status 2>&1; fi
		echo
		echo "== live check (UDP 127.0.0.1:$S_PORT)"
		server_query || echo "no answer"
		echo
		echo "== version"
		[ -x "$BIN" ] && "$BIN" --version
		echo
		echo "== last log lines"
		[ -f "$LOGFILE" ] && tail -n 12 "$LOGFILE"
	} > "$_out" 2>&1
	ui_textfile "Server status" "$_out"
	rm -f "$_out"
}

do_service() {
	if [ ! -x "$BIN" ]; then ui_msg "Not installed" "Install the server first."; return; fi
	_o=$(sys service iris_server "one$1" 2>&1)
	ui_msg "service iris_server $1" "${_o:-ok}"
}

do_log() {
	if [ ! -f "$LOGFILE" ]; then ui_msg "No log yet" "$LOGFILE doesn't exist yet."; return; fi
	ui_textfile "$LOGFILE" "$LOGFILE"
}

do_uninstall() {
	ui_yesno "Uninstall" "Stop the server and remove:\n\n  $BIN\n  $RC_SCRIPT\n  $NEWSYSLOG\n  the iris_server_enable rc.conf entry\n\nYour config ($CONF) and log are kept." || return
	sys service iris_server onestop >/dev/null 2>&1
	sys sysrc -x iris_server_enable >/dev/null 2>&1
	rm -f "$BIN" "$RC_SCRIPT" "$NEWSYSLOG"
	if ui_yesno "Remove user" "Also delete the '$SVC_USER' system user?"; then
		sys pw userdel "$SVC_USER"
	fi
	ui_msg "Uninstalled" "Backrooms Iris server removed. Config kept at $CONF."
}

# --------------------------------------------------------------------- main

check_platform
[ "$DRY_RUN" = 1 ] && mkdir -p "$SRC_DIR/dryrun-root" && : > "$SRC_DIR/dryrun-root/commands.log"
load_settings

while :; do
	choice=$(ui_menu) || break
	case "$choice" in
		install) do_install ;;
		settings) do_settings ;;
		status) do_status ;;
		start) do_service start ;;
		stop) do_service stop ;;
		restart) do_service restart ;;
		log) do_log ;;
		uninstall) do_uninstall ;;
		*) break ;;
	esac
done
[ -n "$DIALOG" ] && clear
exit 0
