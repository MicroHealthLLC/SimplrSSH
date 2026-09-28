# PocketSSH - rules for AI assistants and contributors

PocketSSH is firmware for the **LilyGO T-Deck / T-Deck Plus** (ESP32-S3, 240 MHz dual core,
~340 KB usable internal RAM, 8 MB PSRAM, 16 MB flash). Its job: **configure WiFi, SSH into
servers with common terminal-client features, and chat with ChatGPT by text or voice**, on a
320x240 screen with a small keyboard and trackball. Every change should serve that job.

UI model: a home menu (numbered, `menu` anywhere, `exit` back to it), each app a numbered
menu of short prompts; the screen is edge-to-edge text (no borders or decoration).

## Scope: keep it lean

- If a feature isn't needed to connect to WiFi, SSH into a server, or use the terminal
  comfortably on the T-Deck, don't add it. Remove code that nothing calls.
- No telemetry, analytics, OTA, web servers, access-point mode, Bluetooth, or background
  network traffic. The only outbound connections allowed are: the WiFi network the user
  picks, DNS for the host the user typed, that SSH server, and api.openai.com - only while
  the user is using ChatGPT, with their own API key, over verified TLS (CA bundle).
- Prefer ESP-IDF / LVGL / libssh2 facilities over new dependencies. New components must be
  pinned (`main/idf_component.yml` + `dependencies.lock`) and reviewed for network use.

## Memory (internal RAM is the scarcest resource)

- Budget: ~115 KB of internal RAM is used statically; keep it there or lower. Check with
  `idf.py size` before and after a change and mention the delta in the PR.
- Large or long-lived buffers belong in PSRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`, or
  let the heap place allocations >16 KB there). Internal RAM is for stacks, DMA and small objects.
- No large arrays on task stacks. Stacks: keypad 8 KB (runs all terminal work), SSH receive
  8 KB, trackball 2 KB (only queues events), ChatGPT worker 10 KB (created per request and
  deleted when done). Don't add long-lived tasks.
- Blocking network calls (OpenAI) never run in the UI task; workers touch LVGL only under
  `bsp_display_lock()` and check `chat_generation` so a closed view is never used.
- Audio (I2S mic/speaker) is started only while used; recordings go to PSRAM.
- Bound every collection that grows from user or network input (history 100, profiles 20,
  networks 10, SD keys 8 / 48 KB, terminal text ~4 KB).
- Avoid heap churn in hot paths (SSH receive/render loop): reuse buffers, don't build
  temporary strings per byte.

## Efficiency

- Never busy-wait. Network waits use `waitsocket()`/`select()` with a timeout; loops that poll
  hardware use `vTaskDelay`.
- LVGL calls only while holding `bsp_display_lock()`. Input tasks take it with timeout 0 and
  drop input if the display is busy, so rendering stays smooth.
- Use `refresh_display_now()` before a blocking step (scan, connect, PBKDF2) so progress shows.

## Security

- **Secrets** (WiFi passwords, SSH passwords, key passphrases, PINs):
  - Store only through the vault (`main/secret_vault.cpp`), never in NVS, files or logs.
  - Mask while typing (`wizard_input_masked()`) and when echoing; keep out of command history
    (`redact_secrets()` in `ssh_terminal.cpp`).
  - Wipe after use with `vault::wipe()` / `mbedtls_platform_zeroize()`.
- **Never log** user input, commands, terminal data, passwords or key material. Log levels:
  `ESP_LOGE/W` for problems, `ESP_LOGI` sparingly and without user data.
- **SSH**: every connection goes through `ssh_begin()` (host key check) then `ssh_finish()`.
  Don't bypass known-hosts verification. All libssh2 calls on the session/channel after
  connect must hold `ssh_mutex`.
- **Input from the network or SD card is untrusted**: bound lengths, check sizes, no format
  strings built from it.
- Keep the attack surface closed: station-only WiFi, IPv4, no listening sockets. Settings
  live in `sdkconfig.defaults` with a comment explaining each one.
- CI actions are pinned to commit SHAs and the build image to a digest; keep it that way.

## Persistence

- All settings persist on internal flash and must work without an SD card or any prompt:
  WiFi that connects is saved automatically and reconnects by itself. Everything, including
  the encrypted password vault, lives in NVS (`nvs`, 256 KB), which sits **after** the app in
  `partitions.csv` so flashing the merged image at 0x0 never erases it. Never move it before
  the app or shrink it; changing its offset wipes users' settings.
- Launcher installs (`-launcher.bin`, `partitions_launcher.csv`, e.g. bmorcelli/Launcher) keep the
  launcher's bootloader, table and `nvs`; settings go in our own `pocketssh` data partition
  (spiffs subtype, NVS contents - launchers only create SPIFFS/LittleFS/FAT). Always open
  namespaces with `settings_nvs::open()` and iterate with `settings_nvs::partition()`, never
  `nvs_open()` / `NVS_DEFAULT_PART_NAME`. Never erase a launcher's `nvs`. Keep the partition's
  label, and keep it inside the image (before the app): Launcher's web installer skips
  partitions past the end of the file.
- "It just works" beats extra prompts: don't make saving depend on an optional step (like a
  PIN). Optional hardening must be opt-in.
- New persistent data goes in NVS namespaces listed in `settings_backup.cpp` (so backup,
  restore and erase cover it) or in the vault if it's a secret. Check save errors and tell
  the user.
- The SD card is optional (key files, backups). It shares the display's SPI pins: at runtime
  use `sdcard::mount()`/`unmount()` around a short operation while holding the display lock.
- Anything read from the SD card is untrusted input: validate names, sizes and formats and
  parse fully before changing device state.

## Hardware notes (T-Deck)

- Display ST7789 320x240 over SPI; touch GT911 over I2C (100 kHz, 0x5D); keyboard is an
  ESP32-C3 on I2C 0x55; trackball on GPIO 1/2/3/15 with press on GPIO 0; battery ADC on
  GPIO 4 (strapping pin: 100 ms settle before ADC init); SD card CS 39 shares SCK/MOSI/MISO
  (40/41/38) with the display: its own SPI3 bus at boot, a second device on the display's
  SPI2 bus afterwards (MISO routed in by `sdcard::share_display_bus()`).
- Board power must be enabled via GPIO 10 before peripherals are used.

## Code layout

| File | Responsibility |
|---|---|
| `main/deck_base.cpp` | Boot, hardware init, SD key loading, input tasks |
| `main/ssh_terminal.cpp` | Terminal UI, command parsing, WiFi driver, SSH connection |
| `main/known_hosts.cpp` | Host key verification (TOFU), `hosts` command |
| `main/profile_menu.cpp` | Menu/wizard plumbing, SSH profiles |
| `main/wifi_menu.cpp` | WiFi wizard, saved networks, auto-connect |
| `main/vault_menu.cpp`, `main/secret_vault.cpp` | Encrypted password storage (NVS), optional PIN |
| `main/connection_profiles.cpp` | NVS storage for profiles and networks |
| `main/settings_nvs.cpp` | Picks and opens the settings partition (`nvs` or a launcher's `pocketssh`) |
| `main/storage_menu.cpp`, `main/settings_backup.cpp` | `storage` menu; SD backup/restore/erase |
| `main/sd_card.cpp` | SD mounting (boot and runtime), SSH key loading |
| `main/home_menu.cpp` | Home and Security menus, `go_home()` |
| `main/chat_app.cpp`, `main/openai_client.cpp`, `main/audio.cpp` | ChatGPT app, OpenAI HTTPS client, mic/speaker |

## Workflow

- Build: ESP-IDF v5.5.1, `idf.py build`, release image `idf.py merge-bin -o PocketSSH-v<ver>-release.bin`,
  launcher image `idf.py launcher-bin` (-> `build/PocketSSH-v<ver>-launcher.bin`).
- Match the surrounding style (4-space indent, `snake_case`, brace on its own line for
  functions, same line for control flow).
- CI must build; the code-quality workflow is advisory but aim for zero new warnings.
- Versions come from git tags; CI computes them (`.github/scripts/plan_release.py`) and
  passes `FIRMWARE_VERSION` to the build. Every push to `main` releases the next patch;
  manual runs pick branch (`main`/`integration` = pre-release), bump or exact version.
  Don't hand-edit versions for patch releases. Work on `integration`, merge to `main` to
  release. A `### vX.Y.Z (<date>)` Version History section is optional release-note text.
- Update README.md (user-facing behavior) and SECURITY.md (anything security-relevant) with
  the change. Hardware behavior can only be confirmed on a real T-Deck; say so when it hasn't been.
