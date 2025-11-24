#!/bin/sh
# Auto-load timezone from /etc/TZ
if [ -f /etc/TZ ]; then
    export TZ=$(cat /etc/TZ)
fi
