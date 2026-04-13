# ESP32-S3_Mastery
This project is about pushing the ESP32-S3 toward a more real product shape: C++, Wi-Fi, HTTPS, and a browser UI that feels more like a proper console than a debug page.

The current direction is a browser-hosted Quake 2 deathmatch prototype. The ESP32-S3 serves the setup page, the q2dm1 BSP map, the local Three.js runtime, and a small HTTP API for network setup plus continuous multiplayer state. The browser renders the map and polls the ESP32 for player positions.

## Browser Quake 2 Q2DM1

- The setup page is served from firmware at `/`.
- The map is copied from `third_party/q2dm1bsp/q2dm1.bsp` into the `web_assets` SPIFFS image during the CMake configure step.
- The browser module at `main/web/web_pages/control.js` parses Quake 2 BSP v38 geometry, renders the map with the vendored Three.js module, and uses HTTP polling for peer state.
- The active API surface is under `/api/v1/`, including `/api/v1/network`, `/api/v1/match`, `/api/v1/match/join`, and `/api/v1/player/state`.
- The match is continuous and in-memory. Rebooting the board clears joined players and runtime match state.

## Network Setup

- On first boot without STA credentials, the board starts an AP named like `ESPQUAKE-ABCD`.
- The setup page can save AP, STA, or AP + STA mode into NVS.
- Saved network changes require reboot. Use the page's reboot button or reset the board.
- `idf.py flash` will also flash the SPIFFS `web_assets` partition because the image is generated with `FLASH_IN_PROJECT`.

## Admin Setup

- The first connected user can claim admin from the setup page by setting an admin password.
- The firmware stores a salted SHA-256 password hash in NVS, not the plaintext password.
- Once admin is claimed, match settings and network save/reboot actions require the admin password.
- Admin settings include local site name, server name, FFA or teams, max players, time limit, frag limit, team score limit, and friendly fire.
- The default local site name is `espquake.local`. The first label is also used as the DHCP hostname after reboot. Arbitrary names like `ESPQUAKE.com` still require LAN DNS/router support to resolve in STA mode.

## Flash Layout
This setup assumes the board really has `8MB` flash, not the previous `2MB` header setting.

- `factory` app partition: `0x200000`
- `web_assets` SPIFFS partition: `0x5f0000`

Those values live in [`partitions.csv`](/mnt/c/LocalRepo/ESP32-S3_Mastery/partitions.csv).

## HTTPS for the Device UI
Generate a local self-signed certificate in WSL:

```bash
cd /mnt/c/LocalRepo/ESP32-S3_Mastery/main/secrets/Hidden
openssl req -x509 -newkey rsa:2048 -keyout server_key.pem -out server_cert.pem -days 365 -nodes -subj "/CN=esp32.local"
```

What this does:

- The build system auto-embeds `server_cert.pem` and `server_key.pem` if they exist.
- The firmware will prefer HTTPS on port `443`.
- If the cert files are missing, it falls back to HTTP on port `80`.

## Build and Flash (ESP-IDF + WSL)
PlatformIO is not used. Use ESP-IDF directly.

### A) Device already attached to WSL
PowerShell (Admin):

```powershell
usbipd bind --busid 1-2
usbipd attach --wsl Ubuntu --busid 1-2 --auto-attach
```

WSL (Ubuntu):

```bash
. ~/esp/esp-idf/export.sh
cd /mnt/c/LocalRepo/ESP32-S3_Mastery
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Exit monitor with `Ctrl+]`.

Clean builds:

```bash
idf.py fullclean
```

Or lighter clean:

```bash
idf.py clean
```

### B) Port missing (/dev/ttyACM0 not present)
PowerShell (Admin):

```powershell
usbipd list
usbipd bind --busid <BUSID>
usbipd attach --wsl Ubuntu --busid <BUSID> --auto-attach
```

Then repeat steps in A.

Optional aliases in `~/.bashrc`:

```bash
alias idfenv='. ~/esp/esp-idf/export.sh'
alias mon='idf.py -p /dev/ttyACM0 monitor'
```

Example:

```bash
idfenv && cd /mnt/c/LocalRepo/ESP32-S3_Mastery && idf.py build && idf.py -p /dev/ttyACM0 flash monitor
```
