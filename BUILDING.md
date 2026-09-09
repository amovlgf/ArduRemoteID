# Building from Sources under linux

### Step1: get prerequisites

 - sudo apt install arduino
 - pip install pymavlink

### Step2: get code

 - cd ~
 - git clone https://github.com/ardupilot/arduremoteid
 - cd arduremoteid/
 - git submodule init
 - git submodule update --recursive
 - ./scripts/install_build_env.sh
 - ./scripts/regen_headers.sh
 - ./scripts/add_libraries.sh

## Building with make and arduino-cli

### Step1: Use make to install ESP32 support

 - cd RemoteIDModule
 - make setup

### Step2: Use make to build

 - cd RemoteIDModule
 - make

AMOV RemoteID is an ESP32-C3 module integrated into the
[FlyCore I225U V1](https://www.amovlab.top/pages/wiki-flycore),
not a standalone product. To build only this target, run:

```
cd RemoteIDModule
make amov-rid
```

This creates two files:

 - `ArduRemoteID-AMOV_RID.bin`: complete image for initial flashing
 - `ArduRemoteID_AMOV_RID_OTA.bin`: application image for signed web updates (`BOARD_ID` 13)

### Step3: Use make to upload

 - cd RemoteIDModule
 - make upload

If the board does not flash, hold-down the BOOT pushbutton on the PCB while pressing the RESET pushbutton briefly [to force it into bootloader mode] and retry.
The ESP32-S3 is now running and emitting test/demo remote-id bluetooth

### AMOV_RID initial flashing

AMOV_RID initial programming uses the ESP32-C3 native USB Serial/JTAG
interface. The module does not expose a manual BOOT recovery path.

Validate initial flashing with at least one blank ESP32-C3FH4 device, then
verify that a module running the normal firmware can be flashed repeatedly
through the same USB interface.
Web OTA using `ArduRemoteID_AMOV_RID_OTA.bin` is the preferred field-update
path. AMOV_RID supports `LOCK_LEVEL` values only up to 1 and does not support
permanent eFuse lockdown. Do not burn eFuses that disable USB Serial/JTAG or
ROM download support, because doing so may leave the module without a
recoverable wired flashing path.

AMOV_RID communicates with the flight controller only through DroneCAN at
1 Mbit/s (CAN TX GPIO4, CAN RX GPIO5); MAVLink is disabled for this target.

If you get an error about missing serial support from python, install it with `python -m pip install pyserial`.

### Optional

Plugin your ep32-s3 with ANOTHER usb cable using the port labeled
"UART" on the pcb - this is where mavlink and debug is coming/going,
and you can connect to this with mavproxy, etc.

Plugin your ep32-s3 into a flight-controller UART using pins
RX(17)/TX(18)/GND on the pcb - this listens for mavlink from an
autopilot, and expects to find REMOTE_ID* packets in the mavlink
stream, and it broadcast/s this information from the drone as
bluetooth/wifi on 2.4ghz in a manner that can be received by Android
mobile phone App
[https://play.google.com/store/apps/details?id=org.opendroneid.android_osm]
and hopefully other open-drone-id compliant receivers.

Plugin your ep32-s3 into a flight-controller CAN port by wiring a standard CAN Tranciever (such as VP231 or similar) to pins 47(tx),38(rx),GND on the pcb.

Setup/Configuration of ArduPilot/Mavlink/CAN to communicate together is not documented here, please go to ArduPilot wiki for more, eg: https://ardupilot.org/copter/docs/common-remoteid.html
