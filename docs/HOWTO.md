# How to build an openmanet-32 meshpoint

This guide takes you from parts on the desk to a meshpoint on your OpenMANET mesh, with its
own Wi-Fi hotspot. Plan on about an hour, most of it installing the ESP-IDF toolchain the
first time.

1. [What you need](#1-what-you-need)
2. [Put the hardware together](#2-put-the-hardware-together)
3. [Read your mesh settings from an OpenMANET node](#3-read-your-mesh-settings-from-an-openmanet-node)
4. [Install ESP-IDF 5.4.4](#4-install-esp-idf-544)
5. [Get the code](#5-get-the-code)
6. [Match the radio settings (only if yours differ)](#6-match-the-radio-settings-only-if-yours-differ)
7. [Build and flash](#7-build-and-flash)
8. [Set it up from your phone](#8-set-it-up-from-your-phone)
9. [Check that it joined the mesh](#9-check-that-it-joined-the-mesh)
10. [Using it](#10-using-it)
11. [Reset and start over](#11-reset-and-start-over)
12. [Updating the firmware](#12-updating-the-firmware)
13. [Troubleshooting](#13-troubleshooting)

## 1. What you need

- **Seeed Studio XIAO ESP32S3**: 8 MB flash, 8 MB PSRAM. It comes with a 2.4 GHz antenna.
- **Wio-WM6180 Wi-Fi HaLow Module for XIAO** (Seeed 109100041): the XIAO-sized board with
  the HaLow radio (Quectel FGH100M-H, Morse Micro MM6108, 902 to 928 MHz).
- **A 900 MHz antenna** for the HaLow module, with a u.FL (I-PEX) connector or an adapter.
- **A USB-C data cable**: some cables only charge. Also a 5 V supply that can deliver 2 A,
  or a good computer port.
- **A computer**: macOS, Linux or Windows.
- **A working OpenMANET mesh**, and access to one node's web page (LuCI) or SSH, to read
  its settings.
- **A phone or laptop with Wi-Fi**, to run the setup page.

## 2. Put the hardware together

1. Fit the XIAO ESP32S3 into the module's header, following Seeed's
   [Getting Started with Wi-Fi HaLow Module for XIAO](https://wiki.seeedstudio.com/getting_started_with_wifi_halow_module_for_xiao/)
   for the orientation.
2. Attach the 900 MHz antenna to the HaLow module **before you power the board**. A radio
   that transmits with no antenna can damage itself.
3. Attach the 2.4 GHz antenna to the XIAO ESP32S3. The hotspot uses it.

## 3. Read your mesh settings from an OpenMANET node

The meshpoint must use the same mesh name, password and radio settings as your nodes.

**Mesh name and password.** In the node's web page (LuCI), open Network, then Wireless, and
edit the HaLow (802.11ah) mesh interface. The mesh name is "Mesh Id", and the password is
the "Key" under Wireless Security. Or over SSH (on a Heltec HT-HD01 the HaLow mesh interface is
`default_radio1`; `uci show wireless | grep mesh_id` finds it on other hardware):

```sh
uci get wireless.default_radio1.mesh_id
uci get wireless.default_radio1.key
```

You type these on the meshpoint's setup page later. They are not built into the firmware.

**Radio settings.** On the node, over SSH:

```sh
grep -E "country|channel|op_class|s1g_prim|beacon_int|ieee80211w" /var/run/wpa_supplicant-*.conf
```

Compare what it prints with OpenMANET's US defaults, which the firmware already uses:

| On the node | OpenMANET default | Firmware option |
|---|---|---|
| `country` | `US` | `CONFIG_HALOW_COUNTRY_CODE` in `sdkconfig.defaults` |
| `channel` | `28` | `CONFIG_HOP_S1G_CHANNEL` |
| `op_class` | `71` (8 MHz) | `CONFIG_HOP_S1G_OPCLASS` |
| `s1g_prim_chwidth` | `1` (means 2 MHz; `0` means 1 MHz) | `CONFIG_HOP_PRI_BW_MHZ` (`2` or `1`) |
| `s1g_prim_1mhz_chan_index` | `3` | `CONFIG_HOP_PRI_1MHZ_IDX` |
| `beacon_int` | `1000` | `CONFIG_HOP_BEACON_TUS` |
| `ieee80211w` | `2` (protected frames required) | always on for a mesh with a password |

If everything matches, skip step 6.

## 4. Install ESP-IDF 5.4.4

ESP-IDF is Espressif's toolchain for the ESP32. Use version 5.4.4; other versions are not
tested.

**Windows:** use the
[ESP-IDF Windows installer](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/get-started/windows-setup.html),
pick v5.4.4, and run the commands below in the "ESP-IDF 5.4 CMD" or PowerShell window it
installs.

**macOS and Linux:** first install the prerequisites for your system
([macOS and Linux steps](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/get-started/linux-macos-setup.html)).
On macOS with Homebrew that is `brew install cmake ninja dfu-util`. Then:

```sh
mkdir -p ~/esp && cd ~/esp
git clone -b v5.4.4 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3
```

In every new terminal where you build, load the tools first:

```sh
. ~/esp/esp-idf/export.sh
```

## 5. Get the code

On this project's GitHub page, copy the address from the green **Code** button, then:

```sh
git clone <the address you copied>
cd openmanet-32
```

(Or use Code, then Download ZIP, and unpack it.)

## 6. Match the radio settings (only if yours differ)

Open `sdkconfig.openmanet` in a text editor and change the values to match the table in
step 3:

```
CONFIG_HOP_S1G_CHANNEL=28
CONFIG_HOP_S1G_OPCLASS=71
CONFIG_HOP_PRI_BW_MHZ=2
CONFIG_HOP_PRI_1MHZ_IDX=3
CONFIG_HOP_BEACON_TUS=1000
CONFIG_HOP_TX_DBM=8
```

The country code is `CONFIG_HALOW_COUNTRY_CODE` in `sdkconfig.defaults`. Set it for where
you operate.

`CONFIG_HOP_TX_DBM` caps the HaLow transmit power. 8 dBm keeps the board stable on USB
power. With a proper supply you can raise it, within your local legal limit.

You can also change all of these interactively with `idf.py menuconfig`, under
"openmanet-32".

> If you have built before, delete the file `sdkconfig` in the project folder after
> changing these files. The build keeps an existing `sdkconfig` and would ignore your
> changes.

## 7. Build and flash

```sh
idf.py set-target esp32s3
idf.py build
```

The first build downloads the Morse Micro HaLow driver from Espressif's component registry,
so it needs internet.

Plug the board into the computer and find its port:

- **macOS:** `ls /dev/cu.usbmodem*`
- **Linux:** usually `/dev/ttyACM0`. You may need to be in the `dialout` group.
- **Windows:** Device Manager, Ports (COM & LPT). Look for "USB Serial Device (COMx)".

Then flash it and watch its log:

```sh
idf.py -p <port> flash monitor
```

If flashing can't connect: hold the BOOT button on the XIAO, plug in the USB cable (or
press RESET), release BOOT, and try again. After flashing in this mode, press RESET once.

In the log you should see the board come up in setup mode:

```
settings: Nothing saved: setup mode
hotspot: Setup hotspot (open) "Meshpoint-XXXX-Setup" on channel 6, page at http://192.168.4.1/
```

The log calls the firmware "hopnode", its working name. Press `Ctrl+]` to leave the
monitor; the board keeps running.

## 8. Set it up from your phone

The board's LED blinks slowly while it waits to be set up.

1. On your phone, open Wi-Fi settings and join **Meshpoint-XXXX-Setup**. It is open (no
   password). XXXX is the end of the board's MAC address, so two boards don't clash.
2. A sign-in page should open by itself. If it doesn't, open a browser and go to
   `http://192.168.4.1`. If the phone complains the network has no internet, choose to stay
   connected.
3. Sign in with user **root** and password **openmanet**.
4. **Step 1, new root password:** choose the password you will use to sign in to this page
   from now on, 8 to 63 characters, and type it twice.
5. **Step 2, your OpenMANET network:** enter the mesh name and password from step 3,
   exactly as on the node. They are case-sensitive. If your mesh has no password, tick
   "This mesh has no password".
6. **Step 3, the hotspot:** choose the Wi-Fi name and password (8 to 63 characters) that
   phones and laptops will use to connect to this meshpoint. Tap **Show** to check what
   you typed.
7. Write the root password and hotspot password down, or put them in a password manager.
8. Tap **Finish setup**. The board saves, restarts and joins the mesh. The setup network
   goes away.

## 9. Check that it joined the mesh

About 20 seconds after Finish:

- **LED:** it goes off.
- **Status page:** join your new hotspot, open `http://192.168.4.1` and sign in as root
  with your new password. Under Mesh, your OpenMANET nodes are listed as **linked**, with
  their signal strength. The page also shows the meshpoint's own mesh address.
- **Serial log:** each node shows up as

  ```
  mesh_sec: SAE with xx:xx:xx:xx:xx:xx: done, PMK agreed ...
  mesh: Peering xx:xx:xx:xx:xx:xx: PEERED (peered)
  ```

- **From an OpenMANET node, over SSH:** `iw dev wlh0 station dump | grep -E "Station|plink"`
  lists the meshpoint with `mesh plink: ESTAB`, and `batctl n` lists it as a neighbour.
  You can ping its mesh address from the node.

## 10. Using it

- Join the meshpoint's hotspot to reach devices on the mesh by IP address, for example an
  OpenMANET node's web page at its address. Traffic is translated (NAT), so devices on the
  mesh see the meshpoint's mesh address, not your phone's.
- The hotspot gives no DNS and no internet route, so use IP addresses.
- To change the mesh or hotspot settings, edit them on the page and tap
  **Save and restart**. Leave a password field blank to keep the current one.
- To change the root password, use the **Root password** section. It asks for the current
  one first and signs everyone out.
- Settings can only be changed from the meshpoint's own hotspot. Signed in over the mesh,
  you can look but not change anything.
- Placement: higher is better for HaLow. Keep the antenna vertical and clear of metal, and
  use a power supply that can deliver 2 A.

## 11. Reset and start over

Hold the **BOOT** button for 5 seconds while the board runs. The LED blinks fast; let go.
The board erases every setting, root password included, and restarts in setup mode:
`Meshpoint-XXXX-Setup`, user root, password openmanet. The **Erase settings and set up
again** link at the bottom of the status page does the same.

Holding BOOT while you plug the board in is different: that starts the ESP32's download
mode for flashing (step 7).

## 12. Updating the firmware

```sh
git pull
idf.py build
idf.py -p <port> flash
```

Flashing keeps the saved settings. `idf.py erase-flash` wipes them, so the board starts in
setup mode again. If `sdkconfig.defaults` or `sdkconfig.openmanet` changed, delete
`sdkconfig` before building (see step 6).

## 13. Troubleshooting

**The setup page doesn't open.**
Browse to `http://192.168.4.1` yourself. Turn off mobile data if the phone keeps trying the
internet. Make sure you joined the network ending in `-Setup`.

**"Wrong user name or password" on the setup page.**
The user is `root` in lower case, and the password is `openmanet`. After setup, only your
new root password works.

**"Too many wrong tries."**
After five wrong passwords, sign-in is locked for 30 seconds. Wait and try again.

**The phone can't join the hotspot after setup.**
The hotspot password is wrong. The serial log shows the phone joining and leaving with
`reason = 15`. Do the BOOT reset (step 11) and set it up again.

**The status page says "No mesh nodes heard yet".**
The radio settings don't match your nodes. Check them again (step 3 and step 6). Then check
the HaLow antenna and the country code, and move the board closer to a node for the first
test.

**Nodes are heard but never "linked".**
Usually the mesh name or password doesn't match. The serial log repeats lines like
`SAE with xx:xx:xx:xx:xx:xx failed: ... Trying again in 10 s`. Check both, character by character, against the node. To
tell a radio problem from a password problem, `docs/openmanet-test-mesh.sh` moves one node
to an open test mesh: set the meshpoint up with that mesh name and "no password", then put
the node back with `docs/openmanet-test-mesh-undo.sh`.

**The log says a node "wants hash-to-element SAE".**
That node uses a newer kind of SAE than this firmware does. OpenMANET 1.8.0's mesh does not.

**The board keeps restarting, often when the HaLow radio starts.**
That is usually power. The log's first line after a restart reads
`hopnode starting (reset reason N)`:

| N | Meaning |
|---|---|
| 1 | power dip or power-on |
| 3 | normal restart after saving settings |
| 4 | crash |
| 9 | brownout |
| 11 | reset over USB (flashing) |

Repeated 1 or 9 while the board stays plugged in means the supply can't keep up. Use a
2 A supply or a powered hub and a short cable, and keep `CONFIG_HOP_TX_DBM` low on USB power.

**I forgot the root password.**
BOOT reset (step 11), then set it up again.

**The build fails while fetching `morsemicro/halow`.**
The first build needs internet to download the HaLow driver. Check the connection and run
`idf.py build` again.
