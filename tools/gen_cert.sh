#!/usr/bin/env bash
# Generate the embedded RSA-2048 self-signed client certificate (dev).
# Mirrors the reference certificate_generator.py: CN only, SHA-256,
# serial 1000, ~10 years. The cert/key are gitignored; the TV trusts
# this exact keypair after pairing, so don't regenerate once paired.
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ -f certs/client.pem && -f certs/client.key ]]; then
    echo "certs/client.pem already exists — not overwriting (would break pairing)."
    exit 0
fi
mkdir -p certs
openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 3650 \
    -set_serial 1000 -subj "/CN=esp32-androidtv" \
    -keyout certs/client.key -out certs/client.pem
openssl x509 -in certs/client.pem -noout -subject -serial -enddate
