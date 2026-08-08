#!/usr/bin/env bash
#
# uninstall_laptop_mosquitto.sh
# -----------------------------------------------------------------------
# Reverses everything SETUP_GUIDE.md section 3 ("Mosquitto broker setup")
# installed. Run this ON THE LAPTOP as root or with sudo.
#
# This does NOT touch:
#   - the Orange Pi board (use uninstall_board.sh on the board for that)
#   - the laptop_cam_server.py webcam bridge (it's just a script you run
#     manually per SETUP_GUIDE.md section 2 -- nothing to "uninstall,"
#     just stop running it and optionally uninstall the opencv-python
#     pip package if you don't need it for anything else)
#
# Usage:
#   sudo ./uninstall_laptop_mosquitto.sh                # remove config, keep mosquitto installed
#   sudo ./uninstall_laptop_mosquitto.sh --remove-package # also apt-remove mosquitto itself
#   sudo ./uninstall_laptop_mosquitto.sh --dry-run       # print what would happen, change nothing
#
# Flags can be combined, e.g.: sudo ./uninstall_laptop_mosquitto.sh --remove-package --dry-run

set -euo pipefail

REMOVE_PACKAGE=0
DRY_RUN=0

for arg in "$@"; do
    case "$arg" in
        --remove-package) REMOVE_PACKAGE=1 ;;
        --dry-run) DRY_RUN=1 ;;
        -h|--help)
            awk '/^#!/{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"
            exit 0
            ;;
        *)
            echo "Unknown option: $arg (use --help for usage)" >&2
            exit 1
            ;;
    esac
done

run() {
    if [ "$DRY_RUN" -eq 1 ]; then
        echo "[dry-run] $*"
    else
        echo "+ $*"
        eval "$@"
    fi
}

if [ "$(id -u)" -ne 0 ] && [ "$DRY_RUN" -eq 0 ]; then
    echo "This script needs root (systemctl, /etc/mosquitto changes)." >&2
    echo "Re-run with: sudo $0 $*" >&2
    exit 1
fi

echo "=============================================================="
echo " Smart Security System -- laptop Mosquitto uninstall"
[ "$DRY_RUN" -eq 1 ] && echo " (DRY RUN -- no changes will be made)"
echo "=============================================================="
echo

# ---------------------------------------------------------------
# 1. Stop the broker before touching its config (reverses 3.5)
# ---------------------------------------------------------------
echo "--- Step 1/4: Stopping the Mosquitto broker ---"
mosquitto_service_exists=0
if [ -f /lib/systemd/system/mosquitto.service ] || [ -f /usr/lib/systemd/system/mosquitto.service ] || \
   [ -f /etc/systemd/system/mosquitto.service ]; then
    mosquitto_service_exists=1
elif command -v systemctl >/dev/null 2>&1 && systemctl list-unit-files 2>/dev/null | grep -q '^mosquitto\.service'; then
    mosquitto_service_exists=1
fi

if [ "$mosquitto_service_exists" -eq 1 ]; then
    run "systemctl stop mosquitto || true"
    run "systemctl disable mosquitto || true"
else
    echo "  (mosquitto.service not found -- skipping stop/disable;"
    echo "  if you started it manually with 'mosquitto -c ...', stop that process yourself)"
fi
echo

# ---------------------------------------------------------------
# 2. Remove our specific broker config (reverses 3.4)
# ---------------------------------------------------------------
echo "--- Step 2/4: Removing smart-security's broker config ---"
if [ -f /etc/mosquitto/conf.d/smart-security.conf ]; then
    run "rm -f /etc/mosquitto/conf.d/smart-security.conf"
else
    echo "  (/etc/mosquitto/conf.d/smart-security.conf not present, skipping)"
fi
echo

# ---------------------------------------------------------------
# 3. Remove the password file and ACL file (reverses 3.2, 3.3)
# ---------------------------------------------------------------
echo "--- Step 3/4: Removing password file and ACL file ---"
echo "  NOTE: these files are specific to this project's setup guide"
echo "  (they only contain the 'board_<student_id>' and 'monitor' users"
echo "  and this project's topic ACLs) -- if you've since added other"
echo "  Mosquitto users/rules to these SAME files for unrelated purposes,"
echo "  back them up before continuing, since this deletes the whole file"
echo "  rather than trying to surgically remove just our entries."
echo
if [ -f /etc/mosquitto/passwordfile ]; then
    run "rm -f /etc/mosquitto/passwordfile"
else
    echo "  (/etc/mosquitto/passwordfile not present, skipping)"
fi
if [ -f /etc/mosquitto/aclfile ]; then
    run "rm -f /etc/mosquitto/aclfile"
else
    echo "  (/etc/mosquitto/aclfile not present, skipping)"
fi
echo

# ---------------------------------------------------------------
# 4. Restart Mosquitto with default config, or remove the package
#    entirely (reverses 3.1)
# ---------------------------------------------------------------
echo "--- Step 4/4: Restoring default Mosquitto state ---"
if [ "$REMOVE_PACKAGE" -eq 1 ]; then
    if command -v apt-get >/dev/null 2>&1; then
        run "apt-get remove -y mosquitto mosquitto-clients"
        echo "  Removed the mosquitto and mosquitto-clients packages."
        echo "  (config files under /etc/mosquitto/ not covered above may"
        echo "  still remain -- 'sudo apt-get purge mosquitto' removes those"
        echo "  too, or 'sudo rm -rf /etc/mosquitto' for a full manual wipe.)"
    else
        echo "  ERROR: apt-get not found -- remove the mosquitto package" >&2
        echo "  using your distro's package manager manually." >&2
    fi
else
    echo "  Leaving mosquitto and mosquitto-clients installed, since other"
    echo "  projects on this machine may use them. Password file, ACL file,"
    echo "  and our conf.d override are gone, so a plain 'systemctl start"
    echo "  mosquitto' now runs with only whatever config shipped with the"
    echo "  package (typically anonymous-local-only, distro-dependent)."
    echo "  Re-run with --remove-package if you want the packages gone too."
    if [ "$mosquitto_service_exists" -eq 1 ]; then
        run "systemctl start mosquitto || true"
    fi
fi
echo

echo "=============================================================="
if [ "$DRY_RUN" -eq 1 ]; then
    echo " Dry run complete -- no changes were made."
else
    echo " Uninstall complete."
    echo
    echo " Remaining, deliberately NOT removed by this script:"
    [ "$REMOVE_PACKAGE" -eq 0 ] && echo "   - mosquitto / mosquitto-clients packages -- rerun with --remove-package"
    echo "   - laptop_cam_server.py and its opencv-python pip dependency"
    echo "     (just a script you run manually -- nothing installed system-wide"
    echo "     to undo; 'pip uninstall opencv-python' if you want that gone too)"
fi
echo "=============================================================="
