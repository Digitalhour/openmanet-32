#!/bin/sh
# Run on an OpenMANET node (HT-HD01 V2, OpenMANET 24.10 1.8.0):
#   ssh root@<node address> 'sh -s' < docs/openmanet-test-mesh.sh
#
# Temporarily moves the node's HaLow mesh interface (wlh0) from its encrypted mesh to an
# open test mesh "OPENMANET-TEST-OPEN" on the same channel, so an openmanet-32 board can
# join without the mesh password (tick "This mesh has no password" on its setup page).
# Useful for telling radio problems from encryption problems. batman-adv keeps running on wlh0 (batmesh0).
# OpenMANET 1.8.0 brings up only one mesh interface on the HaLow radio, so the node leaves
# its encrypted mesh until the undo script puts the original config back.
# Undo: docs/openmanet-test-mesh-undo.sh
set -e

if [ -f /etc/config/wireless.pre-xiao-test ]; then
    cp /etc/config/wireless.pre-xiao-test /etc/config/wireless   # start from the original
else
    cp /etc/config/wireless /etc/config/wireless.pre-xiao-test
fi

uci set wireless.default_radio1.mesh_id='OPENMANET-TEST-OPEN'
uci set wireless.default_radio1.encryption='none'
uci commit wireless

wifi reload
sleep 25

echo "== interfaces"
iw dev | grep -E 'Interface|type|channel'
echo "== mesh"
logread | grep -E 'joining mesh|MESH-GROUP-STARTED' | tail -2
echo "== batman interfaces"
batctl if
