FROM alpine:latest

RUN apk add --no-cache \
    asterisk \
    asterisk-srtp \
    asterisk-opus \
    asterisk-sounds-en \
    asterisk-sounds-moh \
    openssl \
    curl \
    bash

RUN mkdir -p /etc/asterisk /var/log/asterisk /var/spool/asterisk /var/run/asterisk /var/lib/asterisk \
    && chown -R asterisk:asterisk /etc/asterisk /var/log/asterisk /var/spool/asterisk /var/run/asterisk /var/lib/asterisk

COPY config/ /etc/asterisk/
COPY entrypoint.sh /entrypoint.sh
COPY pbx-callback.sh /usr/local/bin/pbx-callback.sh
RUN chmod +x /entrypoint.sh /usr/local/bin/pbx-callback.sh

EXPOSE 5060/udp 5060/tcp 5061/tcp 8088/tcp 8089/tcp 10000-10099/udp

ENTRYPOINT ["/entrypoint.sh"]
