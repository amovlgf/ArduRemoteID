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
 - `ArduRemoteID_AMOV_RID_OTA.bin`: unsigned application image to sign for web updates (`BOARD_ID` 13)

There is one AMOV_RID firmware target for both testing and release. It has no
test activation key, test identity or old signed-authorization import path.
Factory identity is separate per-device data; never compile a device UID or
aircraft SN into the common firmware.

For a web update, copy the application image before signing it with the
deployment's OTA private key (the signing tool modifies its input):

```sh
cp ArduRemoteID_AMOV_RID_OTA.bin ArduRemoteID_AMOV_RID_SIGNED_OTA.bin
python3 ../scripts/sign_fw.py ArduRemoteID_AMOV_RID_SIGNED_OTA.bin "$OTA_PRIVATE_KEY" 13
```

The device must already trust that key in `PUBLIC_KEY1` through `PUBLIC_KEY5`.
OTA signing keys are independent of aircraft identity and platform activation.
Signed web OTA preserves the existing factory identity, activation state and
parameters in NVS; it does not provision a missing factory identity.

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
Web OTA using the signed `ArduRemoteID_AMOV_RID_SIGNED_OTA.bin` is the preferred field-update
path. AMOV_RID supports `LOCK_LEVEL` values only up to 1 and does not support
permanent eFuse lockdown. Do not burn eFuses that disable USB Serial/JTAG or
ROM download support, because doing so may leave the module without a
recoverable wired flashing path.

AMOV_RID communicates with the flight controller only through DroneCAN at
1 Mbit/s (CAN TX GPIO4, CAN RX GPIO5); MAVLink is disabled for this target.

### AMOV_RID factory identity provisioning

#### First provisioning through QGC and the flight controller

Firmware 1.17 adds a dedicated first-provision operation to the existing
device-management channel. Use the updated QGC RID management page and a
flight-controller build that enables `CONFIG_UAVCAN_DEVICE_BRIDGE` (the local
PX4 `rid`/`ridbench` targets do). The bridge forwards the payload unchanged;
it does not require a new flight-controller build just for this operation.
Working `uavcan param` commands alone do not prove that this bridge is enabled.

Connect on the ground with the vehicle disarmed, read the RID device, and
check the actual UID. For an unprovisioned device with `LOCK_LEVEL=0`, enter
the manufacturing-assigned 20-byte aircraft SN (printable ASCII without
spaces) and aircraft class `0..4`, then confirm the binding. QGC checks the
device again before sending. The module checks UID, current boot session,
lock level, and absence of any previous factory/state records before writing.
The QGC interface prevents starting provisioning while the vehicle is armed
or underway; the transparent flight-controller bridge itself has no armed
check. Provisioning relies on a trusted local manufacturing connection, not
additional password or signature authentication.

The module creates identity revision 1 with the hardware UID, entered SN and
class; registration and UIN remain empty. It commits and reads back the
factory record, initializes an unactivated platform state, and retains all
other NVS data, including OTA public keys and settings. QGC confirms the
durable receipt and reads the resulting identity back. Then use the separate
platform synchronization action to activate the aircraft.

The operation cannot overwrite an existing identity, even with the same SN
or a new transaction ID. A repeated request with the original transaction
and contents returns its saved receipt. If communication is interrupted
after sending, the outcome is unknown until the original transaction and
identity are read again; QGC must not silently resend a new provisioning
transaction. Restart recovery observes the saved identity and never replays
an uncertain first-provision write. Invalid or partially lost old records
require maintenance and are not erased to enable provisioning.

The manufacturer must keep the UID/SN inventory and prevent assigning one
SN to multiple devices. The module can check its own hardware binding but
cannot establish fleet-wide uniqueness. Replacement of an already assigned
identity is outside this first-provision operation.

#### Offline preparation for blank factory devices

The standard firmware requires the `factory` binary value in NVS namespace
`rid_platform`. This is a 120-byte `AMOVFID1` record containing the actual
six-byte device UID, an assigned 20-byte aircraft SN, aircraft class `0..4`,
a nonzero revision, an optional registration of at most 64 bytes and a UIN of
zero or eight bytes. Identity strings use printable ASCII without spaces.
When a registration is known, the UIN must match its last eight bytes.
Source byte 114 and reserved byte 115 are zero; the last four bytes contain
the CRC32 of bytes 0..115. Firmware checks that the UID matches its hardware.

Read the device's `RID_UID` before provisioning, or read the base eFuse MAC
with ESP-IDF/esptool on a blank device and remove the separators. Set
`RID_UID`, `AIRCRAFT_SN` and `AIRCRAFT_CLASS` from the manufacturing record.
Use the same standard firmware for a test device, with that device's own
assigned test identity in the manufacturing data.

From the repository root, prepare and validate one device's data:

```sh
mkdir -p factory-output
python3 scripts/factory_identity.py \
  --uid "$RID_UID" --sn "$AIRCRAFT_SN" --class "$AIRCRAFT_CLASS" \
  --output factory-output/device.factory.bin \
  --nvs-csv factory-output/device.nvs.csv
python3 scripts/factory_identity.py --verify factory-output/device.factory.bin --uid "$RID_UID"
```

Add `--registration "$REGISTRATION" --uin "$UIN"` only when those values are
known. The tool never writes hardware, refuses to replace any output and
removes its newly created outputs if publication fails. CSV paths refer to
the generated blob by absolute path; regenerate the CSV if moving the blob.

To also produce a complete NVS image, add these options to a fresh generation
command with unused output paths:

```sh
  --nvs-bin factory-output/device.nvs.bin \
  --nvs-generator "$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
```

Use the [ESP-IDF v4.4.1 NVS partition generator](https://github.com/espressif/esp-idf/blob/v4.4.1/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py)
and its Python dependencies for the ESP32 2.0.3 toolchain used here. The
wrapper calls `generate` with NVS version 2 and size `0x5000`, and checks the
output size before publishing any artifacts. Alternatively, run that
generator against the generated CSV:

```sh
python3 "$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" \
  generate factory-output/device.nvs.csv factory-output/device.nvs.bin 0x5000 --version 2
```

For batches, use a CSV with columns `uid,sn,class` and optional
`revision,registration,uin`. Every UID and aircraft SN must be unique in the
batch. Validate all records and produce per-UID blobs and CSVs with:

```sh
python3 scripts/factory_identity.py --batch manufacturing.csv --output-dir factory-output
```

Adding `--nvs-generator` produces a complete NVS image per device too. Treat
a successful tool exit as the batch release gate; a host crash can interrupt
publication across multiple files. Keep the manufacturing UID/SN inventory
and released artifacts together to catch duplicates across separate batches.

**Complete NVS images are for blank factory devices only.** The partition is
at flash offset `0x9000`, size `0x5000`, as defined in
`RemoteIDModule/partitions.csv`. On a blank unit, write the complete firmware
first, then that unit's NVS image at `0x9000` using the factory programmer.
Writing the merged firmware again can replace the NVS range. A factory
programmer configured with esptool can write the prepared partition with:

```sh
python3 "$ESPTOOL" --chip esp32c3 --port "$RID_PORT" \
  write_flash 0x9000 factory-output/device.nvs.bin
```

Read back and compare the `0x5000` bytes before booting the device. After
boot, check its UID/SN and that activation starts as unactivated, then
synchronize platform status through the QGC RID management page. Exercise
activation, deactivation, power cycles and OTA with this same standard
firmware. Factory provisioning does not grant platform activation.

For an existing device, **do not flash a fresh complete NVS image**: it
replaces other NVS values, including OTA public keys and settings. If the
device already has a valid `rid_platform/factory` record, OTA retains it and
its valid saved platform state. If it has only an old test authorization,
the new firmware leaves it unprovisioned; those authorizations are no longer
imported. A device whose factory/platform records are genuinely absent can
use the dedicated QGC first-provision operation above, preserving all other
NVS values. Do not use whole-partition generation as a substitute. Existing
or damaged identity records require maintenance; this repository does not
provide a general in-place NVS editor. Ordinary webpage, parameter and
platform activation operations cannot create or change the factory SN.

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
