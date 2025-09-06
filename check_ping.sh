#!/bin/bash
ping_success=false
if ping -c 1 -W 1 1.1.1.1 > /tmp/ping_tmp 2>&1; then
    ping_success=true
fi

if ! $ping_success && ping -c 1 -W 1 8.8.8.8 > /tmp/ping_tmp 2>&1; then
    ping_success=true
fi


if $ping_success; then
    cp -f /tmp/ping_tmp /tmp/ping
    chmod 644 /tmp/ping
    exit 0
else
    echo > /tmp/ping
    chmod 644 /tmp/ping
    exit 1
fi