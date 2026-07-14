# WiFiProvisioner

A self-contained WiFi provisioning sketch with a web-based GPIO dashboard for the Arduino UNO R4 WiFi.

On first boot the board broadcasts an open setup network. Join it, pick your WiFi network from a scanned list, enter the password, and the board saves the credentials to non-volatile memory and reboots onto your network. From then on it reconnects automatically after every power cycle and serves a dashboard showing the live state of every header pin, with the ability to drive pins as digital outputs or PWM from any browser on the LAN.

Current version: 1.3.0

## Hardware

- Arduino UNO R4 WiFi (Renesas RA4M1 + ESP32-S3)
- Arduino UNO R4 board package installed in the Arduino IDE (provides the WiFiS3 and EEPROM libraries; no third-party libraries required)

This sketch does not run on the UNO Q. On that board, WiFi belongs to the Linux side (Debian on the Qualcomm QRB2210) and sketches on the STM32 have no direct radio access, so provisioning there is a Linux-side job (NetworkManager), not an .ino.

## Installation

1. Place `WiFiProvisioner.ino` in a folder named `WiFiProvisioner`.
2. Open in the Arduino IDE, select Arduino UNO R4 WiFi, and upload.
3. Open the serial monitor at 115200 baud to watch the boot log.

Updating the ESP32-S3 co-processor firmware (Arduino IDE > Tools > Firmware Updater) is strongly recommended before first use. Early firmware versions have known station-mode server and power-save bugs. The sketch prints the current firmware version in its boot banner.

## Usage

### First-time setup

1. Power the board. With no saved credentials it opens an unencrypted access point named `UNO-R4-Setup` (built-in LED blinks slowly).
2. Join that network from a phone or laptop and browse to `http://192.168.4.1`.
3. Pick your network from the scanned list (or type a hidden SSID), enter the password, and press Connect.
4. The board saves the credentials and reboots onto your network. Its new IP address is printed on the serial monitor and appears in your router's client list.

### GPIO dashboard

Browse to the board's IP address on your network. The dashboard shows all twenty header pins (D0-D13, A0-A5) refreshed every three seconds:

- Input pins show their live digital reading; A0-A5 also show the raw analog reading.
- Any pin can be switched to a digital output and toggled HIGH/LOW. New outputs always start LOW.
- D3, D5, D6, D9, D10, and D11 can be switched to PWM mode with a 0-255 duty slider. The value is saved when you release the slider.
- All pin modes and states persist in EEPROM and are restored within a couple of seconds of power-up, before WiFi even connects.
- The built-in LED (D13) doubles as the status indicator (blinking in setup mode, solid when connected) and yields automatically while you own it as an output.

The dashboard also provides Change WiFi network (reboots into setup mode; old credentials and pin config are kept until you save a new network), Factory reset (wipes credentials and pin config), and a Serial logging on/off toggle.

### HTTP API

Everything the dashboard does is plain HTTP, so pins can be scripted from anything on the LAN:

| Endpoint | Effect |
|---|---|
| `GET /api/status` | JSON snapshot of RSSI, logging state, and all pins |
| `GET /api/mode?pin=D7&m=0\|1\|2` | Set pin mode (0 input, 1 output, 2 PWM) |
| `GET /api/toggle?pin=D7` | Toggle a digital output |
| `GET /api/pwm?pin=D3&duty=0-255` | Set PWM duty |
| `GET /api/log?on=0\|1` | Toggle serial logging |
| `GET /change` | Reboot into setup mode, keeping saved state |
| `GET /forget` | Factory reset |

### Serial commands

Newline-terminated commands at 115200 baud, usable even when WiFi is unreachable:

| Command | Effect |
|---|---|
| `status` | Print mode, SSID, IP, and RSSI |
| `portal` | Reboot into setup mode (credentials kept) |
| `forget` | Factory reset and reboot |
| `log on` / `log off` | Toggle timestamped logging |
| `help` | List commands |

With logging on, the serial monitor shows boot, join attempts and their failure cause (association timeout vs. no DHCP lease), the assigned IP, every HTTP connection with the client IP and request line, pin actions, and link loss.

## EEPROM layout

| Address | Contents |
|---|---|
| 0 | Credentials block (magic + SSID + password, 102 bytes) |
| 120 | Boot-behavior flag byte (0xA5 = force setup mode once) |
| 128 | Pin configuration block (magic + mode and value per pin) |

## Security notes

- The setup access point is open by design, so the WiFi password you type crosses that one hop in plaintext. Provision at home, not in public.
- The dashboard and API have no authentication. Anyone on your LAN can read and drive the pins.
- Because pin state persists, an output left HIGH comes back HIGH after a power cycle. Convenient for lamps, hazardous for motors. Do not hang anything safety-critical off these GPIOs.
- Scanned SSIDs are HTML-escaped before rendering, so a maliciously named network cannot inject markup into the setup page.

## Known Limitations

- No captive-portal DNS. WiFiS3 provides no DNS server, so devices joining the setup AP do not get an automatic pop-up; browse to `http://192.168.4.1` manually.
- Provisioning ends in a deliberate reboot. The WiFiS3/ESP32-S3 stack does not survive a hot switch from AP to station mode (it reports connected before DHCP completes and the web server never rebinds), so the sketch reboots into a clean station-mode boot instead. Your phone will drop off the setup AP when you press Connect; this is expected.
- Rescanning in setup mode tears down and rebuilds the AP, so the connected device must rejoin `UNO-R4-Setup` after pressing Rescan.
- Setup-mode logging shows HTTP requests only. WiFiS3 does not expose AP association events, so the log cannot show the moment a device joins the setup network.
- 2.4 GHz only, a hardware limit of the ESP32-S3 module. If your router isolates bands or SSIDs (guest networks, IoT VLANs, AP/client isolation), the board may join successfully yet be unreachable from other devices. The serial `status` command shows which subnet the board actually landed on, which is the fastest way to diagnose this.
- WPA2 personal networks only. Enterprise (802.1X) networks are not supported by this sketch.
- One HTTP client is served at a time; the server is a simple blocking loop. Fine for a dashboard, not a load balancer.
- Hidden SSIDs must be typed manually; they are excluded from the scan list.
- PWM state is stored as an 8-bit duty at the default PWM frequency; no frequency control is exposed.

## Version history

- 1.3.0 - Provisioning-by-reboot fix for the AP-to-station switch bugs (0.0.0.0 IP, unreachable server), DHCP lease verification, change-network and factory-reset paths, boot-into-portal EEPROM flag, timestamped serial logging with dashboard toggle, serial command console, ESP32-S3 firmware version in the boot banner.
- 1.2.0 - Pin configuration persisted to EEPROM and restored at boot; PWM mode with duty sliders on D3/D5/D6/D9/D10/D11; factory reset wipes pin config as well as credentials.
- 1.1.0 - Connected-mode welcome page replaced by the live GPIO dashboard (all 20 pins, output toggling, analog readings, JSON API).
- 1.0.0 - Initial release: open setup AP, SSID picker with password entry, credentials in EEPROM, auto-reconnect after power cycle, basic welcome page.

## License

GPL-3.0
