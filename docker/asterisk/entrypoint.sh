#!/bin/bash
set -e

KEY_DIR="/etc/asterisk/keys"
mkdir -p "$KEY_DIR" /var/run/asterisk /var/log/asterisk /var/spool/asterisk /var/lib/asterisk

# 1. Generate self-signed TLS/DTLS certificate if missing
if [ ! -f "$KEY_DIR/asterisk.pem" ]; then
    echo "==> Generating self-signed TLS/DTLS certificate for WebRTC & WSS..."
    openssl req -new -newkey rsa:2048 -days 3650 -nodes -x509 \
        -subj "/CN=sipsdk-asterisk/O=SipSdk/C=US" \
        -keyout "$KEY_DIR/asterisk.key" \
        -out "$KEY_DIR/asterisk.crt" 2>/dev/null
    cat "$KEY_DIR/asterisk.crt" "$KEY_DIR/asterisk.key" > "$KEY_DIR/asterisk.pem"
fi

# 2. Inject EXTERNAL_IP if provided (for NAT traversal to physical devices / LAN clients)
if [ -n "$EXTERNAL_IP" ] && [ "$EXTERNAL_IP" != "127.0.0.1" ]; then
    echo "==> Configuring external NAT IP: $EXTERNAL_IP"
    sed -i "s/;EXTERNAL_IP_PLACEHOLDER_UDP/external_media_address=$EXTERNAL_IP\nexternal_signaling_address=$EXTERNAL_IP/" /etc/asterisk/pjsip.conf
    sed -i "s/;EXTERNAL_IP_PLACEHOLDER_TCP/external_media_address=$EXTERNAL_IP\nexternal_signaling_address=$EXTERNAL_IP/" /etc/asterisk/pjsip.conf
    sed -i "/type=endpoint/a media_address=$EXTERNAL_IP" /etc/asterisk/pjsip.conf
fi

# 3. Ensure permissions for asterisk user
chown -R asterisk:asterisk /etc/asterisk "$KEY_DIR" /var/run/asterisk /var/log/asterisk /var/spool/asterisk /var/lib/asterisk

echo "==> Starting Asterisk PBX..."
exec asterisk -U asterisk -G asterisk -f -vvv
