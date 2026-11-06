#!/bin/sh
# Undo docs/openmanet-test-mesh.sh on the OpenMANET node:
#   ssh root@<node address> 'sh -s' < docs/openmanet-test-mesh-undo.sh
set -e
if [ -f /etc/config/wireless.pre-xiao-test ]; then
    cp /etc/config/wireless.pre-xiao-test /etc/config/wireless
    rm /etc/config/wireless.pre-xiao-test
fi
wifi reload
sleep 20
iw dev | grep -E 'Interface|type|channel'
batctl if
