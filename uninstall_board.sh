#!/usr/bin/env bash
#
# uninstall_board.sh
# -----------------------------------------------------------------------
# Reverses everything SETUP_GUIDE.md section 4 ("Orange Pi board setup")
# installed. Run this ON THE ORANGE PI as root or with sudo.
#
# This does NOT touch:
#   - the laptop's Mosquitto broker (use uninstall_laptop_mosquitto.sh
#     on the laptop for that)
#   - the SSH hardening changes in /etc/ssh/sshd_config (see the
#     separate --restore-ssh flag below, off by default since reverting
#     SSH config is a more sensitive operation you may not want bundled
#     into a routine uninstall)
#   - build tool packages (build-essential, libssl-dev, etc.) since
#     those are generic system packages you may be using for other
#     things -- only smart-security-specific state is removed
#
# Usage:
#   sudo ./uninstall_board.sh              # standard uninstall
#   sudo ./uninstall_board.sh --purge      # also deletes the SQLite
#                                           # black box and email/MQTT
#                                           # credential files (see below)
#   sudo ./uninstall_board.sh --restore-ssh  # also reverts the SSH
#                                             # hardening block added to
#                                             # sshd_config
#   sudo ./uninstall_board.sh --dry-run    # print what would happen,
#                                           # change nothing
#
# Flags can be combined, e.g.: sudo ./uninstall_board.sh --purge --restore-ssh

set -euo pipefail

PURGE=0
RESTORE_SSH=0
DRY_RUN=0

for arg in "$@"; do
    case "$arg" in
        --purge) PURGE=1 ;;
        --restore-ssh) RESTORE_SSH=1 ;;
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
    echo "This script needs root (systemctl, /etc, /opt, sudoers changes)." >&2
    echo "Re-run with: sudo $0 $*" >&2
    exit 1
fi

echo "=============================================================="
echo " Smart Security System -- Orange Pi board uninstall"
[ "$DRY_RUN" -eq 1 ] && echo " (DRY RUN -- no changes will be made)"
echo "=============================================================="
echo

# ---------------------------------------------------------------
# 1. Stop and disable the three systemd services (reverses 4.8)
# ---------------------------------------------------------------
echo "--- Step 1/7: Stopping and disabling systemd services ---"
for svc in person-detector.service swagger-gateway.service security-board.service; do
    # Check for the unit *file* existing, not "systemctl list-unit-files",
    # since a just-installed-but-never-reloaded unit file won't show up
    # in list-unit-files until a daemon-reload has happened -- checking
    # the file directly is a more reliable signal of "did our setup
    # guide ever install this," independent of systemd's current cache
    # state.
    #
    # Stop dependents (person-detector, swagger-gateway) before their
    # dependency (security-board) -- reverse of the start order the
    # SYSTEMD_DEPENDENCY_DIAGRAM.md documents, which matters here since
    # security-board.service being the Requires= target of the other two
    # means stopping it first would just have systemd stop them anyway,
    # but stopping them explicitly first keeps the sequence intentional
    # and avoids relying on that cascade.
    if [ -f "/etc/systemd/system/${svc}" ]; then
        run "systemctl stop '${svc}' || true"
        run "systemctl disable '${svc}' || true"
    else
        echo "  (${svc} not installed, skipping)"
    fi
done
echo

# ---------------------------------------------------------------
# 2. Remove the systemd unit files (reverses 4.8)
# ---------------------------------------------------------------
echo "--- Step 2/7: Removing systemd unit files ---"
for f in /etc/systemd/system/security-board.service \
         /etc/systemd/system/person-detector.service \
         /etc/systemd/system/swagger-gateway.service; do
    if [ -f "$f" ]; then
        run "rm -f '$f'"
    else
        echo "  ($f not present, skipping)"
    fi
done
run "systemctl daemon-reload || true"
run "systemctl reset-failed || true"
echo

# ---------------------------------------------------------------
# 3. Remove the sudoers rule (reverses 4.7)
# ---------------------------------------------------------------
echo "--- Step 3/7: Removing sudoers rule ---"
if [ -f /etc/sudoers.d/smart-security ]; then
    run "rm -f /etc/sudoers.d/smart-security"
else
    echo "  (/etc/sudoers.d/smart-security not present, skipping)"
fi
echo

# ---------------------------------------------------------------
# 4. Remove deployed application files (reverses 4.6)
# ---------------------------------------------------------------
echo "--- Step 4/7: Removing deployed application files ---"
if [ -d /opt/smart-security ]; then
    run "rm -rf /opt/smart-security"
else
    echo "  (/opt/smart-security not present, skipping)"
fi
echo

# ---------------------------------------------------------------
# 5. Remove config + SSL cert (reverses 4.3, 4.4, 4.5)
# ---------------------------------------------------------------
echo "--- Step 5/7: Removing SSL certificate and config files ---"
if [ "$PURGE" -eq 1 ]; then
    if [ -d /etc/smart-security ]; then
        run "rm -rf /etc/smart-security"
    else
        echo "  (/etc/smart-security not present, skipping)"
    fi
else
    echo "  Keeping /etc/smart-security (SSL cert, email.conf, mqtt.conf)"
    echo "  since it contains your SMTP/MQTT credentials -- re-run with"
    echo "  --purge if you want these deleted too."
fi
echo

# ---------------------------------------------------------------
# 6. Remove the black box database (reverses runtime state from
#    /var/lib/smart-security, created at first run, not in the setup
#    guide's explicit steps but tracked here since it's this project's
#    only other piece of persistent state on disk)
# ---------------------------------------------------------------
echo "--- Step 6/7: Removing the SQLite black box database ---"
if [ "$PURGE" -eq 1 ]; then
    if [ -d /var/lib/smart-security ]; then
        run "rm -rf /var/lib/smart-security"
    else
        echo "  (/var/lib/smart-security not present, skipping)"
    fi
else
    echo "  Keeping /var/lib/smart-security (detection history database)"
    echo "  since it's your recorded data -- re-run with --purge to delete it."
fi
echo

# ---------------------------------------------------------------
# 7. Remove the dedicated 'security' service user (reverses 4.6)
# ---------------------------------------------------------------
echo "--- Step 7/7: Removing the 'security' service user ---"
if id security >/dev/null 2>&1; then
    run "userdel security 2>/dev/null || true"
else
    echo "  (user 'security' not present, skipping)"
fi
echo

# ---------------------------------------------------------------
# Optional: revert SSH hardening (reverses section 5)
# ---------------------------------------------------------------
if [ "$RESTORE_SSH" -eq 1 ]; then
    echo "--- Optional: reverting SSH hardening block ---"
    if [ -f /etc/ssh/sshd_config.bak ]; then
        echo "  Found /etc/ssh/sshd_config.bak from the original setup."
        run "cp /etc/ssh/sshd_config /etc/ssh/sshd_config.before-uninstall-revert"
        run "cp /etc/ssh/sshd_config.bak /etc/ssh/sshd_config"
        if [ "$DRY_RUN" -eq 0 ]; then
            if ! command -v sshd >/dev/null 2>&1; then
                echo "  ERROR: 'sshd' binary not found -- cannot validate the restored" >&2
                echo "  config before restarting the SSH service. Leaving the restored" >&2
                echo "  (pre-hardening) config in place at /etc/ssh/sshd_config, but NOT" >&2
                echo "  restarting sshd/ssh automatically -- validate and restart it" >&2
                echo "  yourself once openssh-server's sshd is available:" >&2
                echo "    sudo sshd -t && sudo systemctl restart ssh" >&2
            elif sshd -t; then
                run "systemctl restart ssh 2>/dev/null || systemctl restart sshd 2>/dev/null || true"
                echo "  Restored original sshd_config and restarted the SSH service."
            else
                echo "  ERROR: restored sshd_config failed 'sshd -t' validation." >&2
                echo "  Not restarting sshd -- your current (hardened) config is" >&2
                echo "  still in place and still valid. Investigate before retrying." >&2
                run "cp /etc/ssh/sshd_config.before-uninstall-revert /etc/ssh/sshd_config"
            fi
        else
            echo "  [dry-run] would validate with 'sshd -t' and restart the SSH service on success"
        fi
    else
        echo "  No /etc/ssh/sshd_config.bak found (the backup the setup guide"
        echo "  creates in section 5) -- cannot safely auto-revert since there's"
        echo "  no known-good prior config to restore. Manually remove the"
        echo "  '--- Smart Security System hardening ---' block from"
        echo "  /etc/ssh/sshd_config instead, then 'sudo sshd -t && sudo systemctl restart sshd'."
    fi
    echo
else
    echo "--- SSH hardening left in place ---"
    echo "  (PermitRootLogin no, etc. in /etc/ssh/sshd_config are untouched)."
    echo "  Re-run with --restore-ssh if you want this reverted too."
    echo
fi

echo "=============================================================="
if [ "$DRY_RUN" -eq 1 ]; then
    echo " Dry run complete -- no changes were made."
else
    echo " Uninstall complete."
    echo
    echo " Remaining, deliberately NOT removed by this script:"
    echo "   - Generic build packages (libssl-dev, libcurl4-openssl-dev,"
    echo "     libsqlite3-dev, libmosquitto-dev, python3-venv, etc.) --"
    echo "     these are common system libraries, not project-specific."
    [ "$PURGE" -eq 0 ] && echo "   - /etc/smart-security (SSL cert + email/MQTT credentials) -- rerun with --purge"
    [ "$PURGE" -eq 0 ] && echo "   - /var/lib/smart-security (black box database) -- rerun with --purge"
    [ "$RESTORE_SSH" -eq 0 ] && echo "   - SSH hardening in /etc/ssh/sshd_config -- rerun with --restore-ssh"
fi
echo "=============================================================="
