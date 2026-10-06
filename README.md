# WPILib HAL Simulation - XRP Edition
## Introduction
This repository contains a reference implementation of a [XRP robot](https://www.sparkfun.com/products/22230) that can be controlled via the WPILib XRP extension.

The firmware implements a [custom binary protocol](https://github.com/wpilibsuite/allwpilib/tree/main/simulation/halsim_xrp) over UDP to account for the less performant hardware on the XRP.

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
The firmware provides an endpoint for the WPILib Simulation layer that allows WPILib robot programs to interact with real hardware on the XRP over UDP. 

Upon boot up, the following will happen:
* The IMU will calibrate itself. This lasts approximately 3-5 seconds, and will be indicated by the green LED rapidly blinking.
* The network will be configured
  * By default, a WiFi Access point will be created
    * The Access Point will have an SSID of the form "XRP-AAAA-BBBB" where "AAAA-BBBB" are hexadecimal digits representing the unique ID of a particular XRP board
    * The password for the access point is set to "xrp-wpilib" (without the quotes)
  * If set as such (see the section on XRP Configuration), the XRP will either start a custom-named AP, or connect to an existing network

For ideal use, the XRP should be placed on a flat surface prior to power up, and if necessary, users can hit the reset button to restart the firmware and IMU calibration process.

If setup as an Access Point, the configured AP name should appear in the list of available WiFi networks. The XRP will be available at the IP address 192.168.42.1.

If setup in STA mode (i.e. connected to an existing network), the IP address can be determined by either using a tool like Angry IP Scanner, or (more easily), by connecting the XRP to a computer, navigating to the PICODISK removable drive and opening the `xrp-status.txt` file. This file contains information about which network the XRP is connected to, as well as the IP address.

### Serial Diagnostics

USB Serial reports a runtime summary every five seconds, IMU timing
about every four seconds, and connection, enable/disable, and error events.

Logs use a fixed 4 KiB RAM queue. The main loop drains at most 64 bytes per
iteration after control and watchdog processing, using only available USB space.
A busy USB interface or stalled reader does not make the logger wait. Messages
are dropped when the queue is full or in use, and messages longer than 767 bytes
are truncated. Repeated encoder-overrun errors are limited
to one message per second. The five-second summary includes
`log_drop`, `log_supp`, and `log_trunc` counters for dropped, rate-limited, and
truncated messages. Logging does not write to flash.

### XRP Configuration
The XRP provides a simple web-based configuration screen that allows users to adjust the network settings. This screen is available at `http://<IP ADDRESS OF XRP>:5000`. By default, this will be `http://192.168.42.1:5000`.

Users can manually edit the JSON configuration to change the AP name/password, or provide a list of networks to connect to in STA mode. Note that an AP name and password must always be provided as the XRP will fallback to generating an AP if it cannot connect to any listed networks. The `mode` field can be switched between `AP` or `STA` depending on the user's preference.

After saving changes, make sure the restart the XRP.

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

## Packet Protocol

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

Control sequences use 16-bit modular ordering, accepting forward distances of 1-32767 and ignoring duplicates or stale packets. Watchdog expiry resets control state, so the next session may start at any sequence number.
