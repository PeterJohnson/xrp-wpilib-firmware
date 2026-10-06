# WPILib HAL Simulation - XRP Edition
## Introduction
This repository contains a reference implementation of a [XRP robot](https://www.sparkfun.com/products/22230) that can be controlled via the WPILib XRP extension.

The firmware implements a [custom binary protocol](https://github.com/wpilibsuite/allwpilib/tree/main/simulation/halsim_xrp) over Bluetooth LE. It advertises both a custom GATT service and an LE L2CAP Credit-Based Mode channel so clients can use the best packet transport available on each operating system.

## Documentation
Official documentation for the XRP and how to use it with WPILib can be found on the [WPILib Docs site](https://docs.wpilib.org/en/latest/docs/xrp-robot/index.html). The documentation below is also reflected in the official WPILib documentation.

## Installation and Usage

### Firmware installation and upgrades
To install the latest firmware on your XRP, do the following:

* Download the latest firmware UF2 file from Releases
* Plug the XRP into your computer with a USB cable. You should see a red power LED that lights up.
* While holding the BOOTSEL button (the white button on the green Pico W, near the USB connector), quickly press the reset button (middle left side of the XRP board), and then release the BOOTSEL button
* The board will temporariloy disconnect from your computer, and then reconnect as a USB storage device named "RPI-RP2"
  * If this drive does not appear, you can also try unplugging the XRP from your computer, holding the BOOTSEL button down, reconnecting the XRP to your computer and then releasing the BOOTSEL button.
* Drag the UF2 firmware file into the "RPI-RP2" drive, and it will automatically update the firmware
* Once complete, the "RPI-RP2" device will disconnect, and the board should automatically reconnect as a serial device running the WPILib firmware
* At this point, you can disconnect the XRP board from your computer and run it off battery power

### Basic usage
The firmware provides a custom Bluetooth packet endpoint for the WPILib Simulation layer that allows WPILib robot programs to interact with real hardware on the XRP.

Upon boot up, the following will happen:
* The IMU will calibrate itself. This lasts approximately 3-5 seconds, and will be indicated by the green LED rapidly blinking.
* The Bluetooth LE transport will start advertising
  * By default, the device name will have the form "WPIXRP-AAAA-BBBB" where "AAAA-BBBB" are hexadecimal digits representing the unique ID of a particular XRP board
  * The device name can be customized with the Bluetooth configuration in `/config.ini`; the firmware always advertises names with the `WPIXRP-` prefix
  * The primary advertisement includes the complete Bluetooth device name
  * The scan response includes the WPILib XRP GATT service UUID `7d2ea28a-f7bd-485d-9d6a-2c3f0b214a3f`
  * The optional high-performance packet channel uses LE L2CAP Credit-Based Mode on PSM `0x0081`

For ideal use, the XRP should be placed on a flat surface prior to power up, and if necessary, users can hit the reset button to restart the firmware and IMU calibration process.

The configured Bluetooth name should appear in your operating system's Bluetooth pairing UI. Windows clients should use the custom GATT service. Linux and macOS clients may use either GATT or the LE L2CAP channel on PSM `0x0081`.

The Bluetooth name can also be found by connecting the XRP to a computer, navigating to the PICODISK removable drive and opening the `XRP-Status.txt` file. This file contains the firmware version, chip ID, Bluetooth name, and configuration information. It is written at startup. Connection events are available over USB Serial at 115200 baud.

### Serial Diagnostics

USB Serial reports loop timing and connection state every five seconds, plus
connection, enable/disable, and error events. Output uses the nonblocking logger.

### XRP Configuration
The firmware stores its persistent configuration in `/config.ini` on LittleFS. It is a plain text INI file so it can be edited by hand in a text editor. If the file is missing, invalid, or uses an older schema version, the firmware rewrites it with the default Bluetooth configuration template on boot.

The current configuration schema is version `2`:

```ini
# XRP firmware configuration
# Edit this file with a plain text editor, then restart the XRP.
# Lines starting with # or ; are comments.
# Inline comments are allowed after whitespace.

config_version = 2

[bluetooth]
# The firmware always advertises Bluetooth names with the WPIXRP- prefix.
# Use either a full WPIXRP- name or just the suffix after WPIXRP-.
# Suffix length: 1-19 printable ASCII characters.
# Default: WPIXRP-AAAA-BBBB
# deviceName = "WPIXRP-AAAA-BBBB"
```

`deviceName` in the `[bluetooth]` section controls the advertised Bluetooth name. Leave it commented out to use the generated default. To customize the name, uncomment the setting and change the value:

```ini
[bluetooth]
deviceName = "My-XRP"
```

The value may be either the full `WPIXRP-AAAA-BBBB` name or the `AAAA-BBBB` suffix; the firmware always enforces the `WPIXRP-` prefix. The suffix after `WPIXRP-` must be 1-19 printable ASCII characters. The generated `WPIXRP-AAAA-BBBB` name is used as the default and fallback value. Restart the XRP after changing this value.

### Bluetooth Transport
The firmware exposes two packet transports:

* GATT service UUID: `7d2ea28a-f7bd-485d-9d6a-2c3f0b214a3f`
  * Control characteristic UUID: `7d2ea28b-f7bd-485d-9d6a-2c3f0b214a3f`
  * Control property: `WRITE_WITHOUT_RESPONSE`
  * Status characteristic UUID: `7d2ea28c-f7bd-485d-9d6a-2c3f0b214a3f`
  * Status property: `NOTIFY`
* LE L2CAP Credit-Based Mode PSM: `0x0081`

Each GATT write value, GATT notification value, or L2CAP SDU contains exactly one WPILib XRP protocol packet. There is no additional length prefix inside the Bluetooth payload.

Each protocol packet starts with a 5-byte header:

```text
[seq:u16][ctrl:u8][fieldMask:u16][payload...]
```

All multi-byte values are big-endian. The payload contains each field selected by `fieldMask`, emitted in ascending bit order. Packets with unknown field bits or payload sizes that do not exactly match the selected fields are ignored.

Control packets sent to the XRP use these field bits:

| Bit | Field | Payload |
|-----|-------|---------|
| 0-3 | Motor 0-3 | `pwm:i16` |
| 4-7 | Servo 4-7 | `degrees:u8` |
| 8 | DIO 0-7 | `presentMask:u8`, `valueMask:u8` |

Status packets sent by the XRP use these field bits:

| Bit | Field | Payload |
|-----|-------|---------|
| 0-3 | Encoder 0-3 | `count:i32`, `periodNumerator:u32` |
| 4 | DIO 0-7 | `presentMask:u8`, `valueMask:u8` |
| 5 | Gyro | `rateX:f32`, `rateY:f32`, `rateZ:f32`, `angleX:f32`, `angleY:f32`, `angleZ:f32` |
| 6 | Accel | `accelX:f32`, `accelY:f32`, `accelZ:f32` |
| 7-9 | Analog 0-2 | `value:u16` |
| 11 | Timing | `lastControlSeq:u16`, `controlRxAge10Us:u16` |

Motor `pwm` values are clamped to `-255` to `255`, which maps directly to the XRP motor PWM magnitude plus direction. Servo `degrees` values are clamped to `0` to `180`, matching the integer degree value applied by the XRP servo library. DIO payload bits are channel-indexed; bit `n` in `presentMask` means DIO channel `n` is included, and bit `n` in `valueMask` is that channel's value. XRP status currently reports DIO 0, the user button. Analog values are scaled over `0` to `5 V`, where `0` is `0 V` and `65535` is `5 V`.

Encoder period uses a fixed denominator of `1000000`; `periodNumerator >> 1` is the period in microseconds, and the low bit is the direction bit (`1` for forward, `0` for reverse). A `periodNumerator` of `0xffffffff` indicates no valid period.

The XRP status `ctrl` byte is a copy of the most recent accepted control packet `ctrl` byte. The timing field's `lastControlSeq` echoes the most recent accepted motor/servo/DIO control packet sequence number, and `controlRxAge10Us * 10` is the number of microseconds between receiving that control packet and producing the status packet. A `controlRxAge10Us` value of `0xffff` indicates no control packet has been accepted yet or the age exceeded the representable range. Clients can use this echo with their local control-packet send timestamps to estimate application-level round-trip latency.

Control sequences use 16-bit modular ordering, accepting forward distances of 1-32767 and ignoring duplicates or stale packets. Disconnects and L2CAP channel closure reset control state, so the next session may start at any sequence number.

The firmware advertises preferred connection parameters of 7.5 ms minimum interval, 15 ms maximum interval, and latency 0. The central device ultimately decides the actual connection parameters. GATT clients should negotiate an ATT MTU large enough for the largest WPILib XRP packet they expect to receive; the firmware does not fragment packets across multiple notifications.

#### Note
As of 10/13/2023, you MUST use the [2024 Beta 1 version](https://github.com/wpilibsuite/allwpilib/releases/tag/v2024.1.1-beta-1) (or later) of WPILib to write XRP programs. There are also examples and templates available (look for "XRP" in the examples/templates dropdown when creating a new project).

## Built-in IO Mapping

### Digital I/O Map
| DIO Port # | Function          |
|------------|-------------------|
| 0          | XRP User Button   |
| 1          | XRP Onboard LED   |
| 2          | RESERVED          |
| 3          | RESERVED          |
| 4          | Left Encoder A    |
| 5          | Left Encoder B    |
| 6          | Right Encoder A   |
| 7          | Right Encoder B   |
| 8          | Motor 3 Encoder A |
| 9          | Motor 3 Encoder B |
| 10         | Motor 4 Encoder A |
| 11         | Motor 4 Encoder B |

### Analog I/O Map
| Analog Port # | Function          |
|---------------|-------------------|
| 0             | Left Reflectance  |
| 1             | Right Reflectance |
| 2             | Rangefinder       |

NOTE: The analog I/O mapping assumes that the reflectance sensor and rangefinder are plugged into the XRP as directed in the setup instructions. All analog I/O channels return values in the range 0-5V.

#### Reflectance Sensors
The reflectance sensors return a value from 0.0V (white) to 5.0V (black).

#### Rangefinder
The maximum range of the rangefinder is 4m, and the minimum detectable range is 2cm. Any values outside of this range will cause the rangefinder to saturate and return the maximum value.

The rangefinder will return a value between 0.0V (min distance) to 5.0V (4m).

### Motor and Servo Map

Instead of pure PWM channels, the XRP uses SimDevices, specifically the `XRPMotor` and `XRPServo` devices. 

| Device  # | Device Type | Function    |
|-----------|-------------|-------------|
| 0         | XRPMotor    | Left Motor  |
| 1         | XRPMotor    | Right Motor |
| 2         | XRPMotor    | Motor 3     |
| 3         | XRPMotor    | Motor 4     |
| 4         | XRPServo    | Servo 1     |
| 5         | XRPServo    | Servo 2     |
