# ESP32-S3_Mastery
This project is about pushing the ESP32-S3 toward a more real product shape: C++, Wi-Fi, HTTPS, and a browser UI that feels more like a proper console than a debug page.

The current direction is browser-hosted Doom. The ESP32-S3 does not run WebGPU itself; it serves a secure page and the Doom web bundle, while the browser runs the Doom core in WebAssembly and uses WebGPU to present frames and capture web inputs. That split is useful because the board stays focused on embedded work like networking and TLS, while the browser handles the heavier graphics and input layer.

<img width="953" height="599" alt="doom image" src="https://github.com/user-attachments/assets/c7685fc6-e3f9-4af8-af80-2d78e2bb310d" />


## Browser Doom over HTTPS + WebGPU
WebGPU requires a secure context, so the Doom page needs to be opened over HTTPS on the device.

- The ESP32-S3 serves a tiny shell page from firmware and the larger Doom bundle from a SPIFFS partition.
- The browser runs the Doom runtime from `doomgeneric.js` and `doomgeneric.wasm`.
- WebGPU is used as the presentation layer for the Doom framebuffer.
- The page now autostarts Doom on load, so `doomgeneric.data` must include a bundled IWAD.

If the device falls back to plain HTTP because local certs are missing, the page can still load, but WebGPU launch will be blocked by the browser.

## Build the Doom Browser Bundle (WSL)
The repo includes a custom browser port source at `tools/doom_webgpu/doomgeneric_webgpu.c` and a build helper at `tools/doom_webgpu/build_webgpu_doom.sh`.

Build the browser bundle with an embedded IWAD:

```bash
cd /mnt/c/LocalRepo/ESP32-S3_Mastery
bash tools/doom_webgpu/build_webgpu_doom.sh --embed-iwad /full/path/to/doom1.wad
```

Notes:

- `third_party/doomgeneric/doomgeneric` must exist locally.
- `emcc` must be available in your active WSL shell.
- The build outputs land in `main/web/web_pages/doom/`.
- The repo does not include a WAD or IWAD. For this ESP32 flash layout, prefer a smaller IWAD such as shareware `doom1.wad`.
- `.data` and `.wad` files in that folder are ignored by git to avoid accidentally committing game data.
- `idf.py flash` will also flash the SPIFFS `web_assets` partition because the image is generated with `FLASH_IN_PROJECT`.
- Large IWADs like `freedm.wad` do not fit in the current internal flash asset partition. `doom1.wad` is the practical target here.

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
