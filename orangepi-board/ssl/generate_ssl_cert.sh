#!/usr/bin/env bash
#
# generate_ssl_cert.sh
# -----------------------------------------------------------------------
# Generates a self-signed TLS certificate for the board's HTTPS server.
#
# Usage:
#   sudo ./generate_ssl_cert.sh

set -euo pipefail

STUDENT_ID="402102668"

OUT_DIR="/etc/smart-security/ssl"
DAYS_VALID=825   # under the 825-day CA/Browser Forum cap some browsers still enforce for leaf certs

mkdir -p "$OUT_DIR"

echo "Generating self-signed certificate with CN=${STUDENT_ID} ..."

openssl req \
    -x509 \
    -newkey rsa:2048 \
    -keyout "${OUT_DIR}/server.key" \
    -out "${OUT_DIR}/server.crt" \
    -days "${DAYS_VALID}" \
    -nodes \
    -subj "/CN=${STUDENT_ID}/O=SmartSecuritySystem/C=IR"

chmod 600 "${OUT_DIR}/server.key"
chmod 644 "${OUT_DIR}/server.crt"

echo ""
echo "Done. Files written to ${OUT_DIR}/server.crt and ${OUT_DIR}/server.key"
echo ""
echo "Verify the CN field with:"
echo "  openssl x509 -in ${OUT_DIR}/server.crt -noout -subject"
echo ""
echo "Expected output:"
echo "  subject=CN = ${STUDENT_ID}, O = SmartSecuritySystem, C = IR"
