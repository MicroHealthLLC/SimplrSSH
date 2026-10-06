# SimplrSSH - rules for AI assistants and contributors

SimplrSSH is firmware for the **LilyGO T-Deck / T-Deck Plus** (ESP32-S3, 240 MHz dual core,
~340 KB usable internal RAM, 8 MB PSRAM, 16 MB flash; 320x240 screen, small keyboard and
trackball) and the **M5Stack Tab5 with the Tab5 Keyboard** (ESP32-P4, 360 MHz, 32 MB PSRAM,
16 MB flash, WiFi on an ESP32-C6 co-processor; 1280x720 landscape touchscreen, 70-key
keyboard). Its job: **configure WiFi, SSH into servers with common terminal-client features,
and chat with ChatGPT by text or voice**. Every change should serve that job, on both boards.

One source tree builds both: the IDF target picks the board (esp32s3 = T-Deck, esp32p4 =
Tab5). Shared code calls the board layer (`main/include/board.hpp`) for anything
board-specific - power, display, keyboard, UI fonts and sizes, hint texts - and never includes
board pins or fixed font sizes (test/guards/test_firmware_rules.py, `Boards`).

This file is the single source of rules for every AI assistant and contributor.
`CLAUDE.md` is a symbolic link to it (so Claude Code loads it) and `AGENTS.md` points here:
edit this file only, and never replace the link with a copy (test/guards/test_rules_file.py).

UI model: a home menu (numbered, `menu` anywhere, `exit` back to it), each app a numbered
menu of short prompts; the screen is edge-to-edge text (no borders or decoration). SSH sessions:
on the T-Deck, scrolling text and a line sent with Enter; on the Tab5, a full-screen xterm
(TUI programs) with every key sent to the server. Keep the T-Deck's terminal as it is.

## Scope: keep it lean

- If a feature isn't needed to connect to WiFi, SSH into a server, or use the terminal
  comfortably on the device, don't add it. Remove code that nothing calls.
- No telemetry, analytics, OTA, web servers, access-point mode, Bluetooth, or background
  network traffic. The only outbound connections allowed are: the WiFi network the user
  picks, DNS for the host the user typed, that SSH server, and api.openai.com - only while
  the user is using ChatGPT, with their own API key, over verified TLS (CA bundle).
- Prefer ESP-IDF / LVGL / libssh2 facilities over new dependencies. New components must be
  pinned (`main/idf_component.yml`, with a `target ==` rule if only one board uses them, and
  the board's lock: `dependencies.lock` or `dependencies.lock.esp32p4`) and reviewed for
  network use.

## Memory (internal RAM is the scarcest resource)

- Budget: ~115 KB of internal RAM is used statically on the T-Deck and ~118 KB on the Tab5;
  keep it there or lower. Check with `idf.py size` (both boards) before and after a change and
  mention the delta in the PR. The Tests build job fails above `INTERNAL_RAM_BUDGET` (118 KiB)
  and `TAB5_INTERNAL_RAM_BUDGET` (122 KiB) in `.github/scripts/check_firmware.py`.
- Large or long-lived buffers belong in PSRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`, or
  let the heap place allocations >16 KB there). Internal RAM is for stacks, DMA and small objects.
- No large arrays on task stacks. Stacks: keypad 8 KB (runs all terminal work; also polls the
  Tab5 keyboard), SSH receive 8 KB, trackball 2 KB (T-Deck only; only queues events), ChatGPT
  worker 10 KB (created per request and deleted when done). Don't add long-lived tasks.
- Blocking network calls (OpenAI) never run in the UI task; workers touch LVGL only under
  `bsp_display_lock()` and check `chat_generation` so a closed view is never used.
- Audio (I2S mic/speaker) is started only while used; recordings go to PSRAM.
- Bound every collection that grows from user or network input (history 100, profiles 20,
  networks 20, SD keys 8 / 48 KB, terminal text ~6 KB, Tab5 SSH scrollback 1000 lines in PSRAM).
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
    (`redact_secrets()` in `command_redact.cpp`).
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
  launcher's bootloader, table and `nvs`; settings go in our own `simplrssh` data partition
  (spiffs subtype, NVS contents - launchers only create SPIFFS/LittleFS/FAT). Always open
  namespaces with `settings_nvs::open()` and iterate with `settings_nvs::partition()`, never
  `nvs_open()` / `NVS_DEFAULT_PART_NAME`. Never erase a launcher's `nvs`. Keep the partition's
  label, and keep it inside the image (before the app): Launcher's web installer skips
  partitions past the end of the file. Installs from before the rename have a `pocketssh`
  partition, `/pocketssh` SD backups and a `POCKETSSH-BACKUP 1` header; keep reading those
  (`settings_nvs.cpp`, `storage_menu.cpp`, `settings_backup.cpp`) so their settings survive.
- "It just works" beats extra prompts: don't make saving depend on an optional step (like a
  PIN). Optional hardening must be opt-in.
- New persistent data goes in NVS namespaces listed in `settings_backup.cpp` (so backup,
  restore and erase cover it) or in the vault if it's a secret. Check save errors and tell
  the user.
- The SD card is optional (key files, backups). On the T-Deck it shares the display's SPI
  pins: at runtime use `sdcard::mount()`/`unmount()` around a short operation while holding the
  display lock (do the same on the Tab5, where it has its own SDMMC slot).
- Anything read from the SD card is untrusted input: validate names, sizes and formats and
  parse fully before changing device state.

## Hardware notes (T-Deck)

- Display ST7789 320x240 over SPI; touch GT911 over I2C (100 kHz, 0x5D); keyboard is an
  ESP32-C3 on I2C 0x55; trackball on GPIO 1/2/3/15 with press on GPIO 0; battery ADC on
  GPIO 4 (strapping pin: 100 ms settle before ADC init); SD card CS 39 shares SCK/MOSI/MISO
  (40/41/38) with the display: its own SPI3 bus at boot, a second device on the display's
  SPI2 bus afterwards (MISO routed in by `sdcard::share_display_bus()`).
- Board power must be enabled via GPIO 10 before peripherals are used.

## Hardware notes (Tab5)

- Display: 5" 720x1280 MIPI-DSI panel (ILI9881C+GT911 touch, or ST7123 / ST7121 with built-in
  touch, detected by the `espressif/m5stack_tab5` BSP), rotated to 1280x720 landscape by the
  PPA (`LV_DISPLAY_ROTATION_90`, keyboard at the bottom); LVGL buffers in PSRAM. Backlight
  GPIO 22. System I2C on GPIO 31/32 (BSP): IO expanders, touch, codecs, INA226 battery monitor.
- IO expanders (PI4IOE5V6408), reset by the BSP at init: 0x43 P0 antenna (low = internal),
  P1 speaker amp, P2 5 V to the side port (the keyboard needs it), P4 LCD reset, P5 touch
  reset; 0x44 P0 ESP32-C6 power, P3 USB-A 5 V, P4 power-off pulse, P5 quick charge (low = on),
  P7 charger enable. The reset leaves every pin high-impedance: an output only drives once set
  push-pull (`esp_io_expander_set_output_mode()`). `board::init_power()` sets them: the battery
  charges only while firmware enables the charger.
- Keyboard: STM32 at I2C 0x6D on its own bus (G0 SDA, G1 SCL), INT G50 low while events are
  queued, raw mode (press/release by row/column; reading 0x20 pops an event, so never retry
  it). Key map, Sym/Aa/Ctrl, repeat and push-to-talk: `tab5_keymap.cpp` (host-tested).
- WiFi: ESP32-C6 on SDIO slot 1 (CLK 12, CMD 13, D0-D3 11/10/9/8, reset 15) via esp_hosted
  and esp_wifi_remote; started by `board::wifi_prepare()` after the C6 is powered. The SD card
  uses SDMMC slot 0 (GPIO 39-44, I/O power from LDO channel 4) of the same controller;
  unmounting releases only slot 0.
- Audio: ES7210 mics and ES8388 speaker codec on shared I2S pins (MCLK 30, BCLK 27, WS 29,
  DOUT 26, DIN 28), never used at the same time.

## Code layout

| File | Responsibility |
|---|---|
| `main/deck_base.cpp` | Boot, SD key loading, keypad_task (all input and terminal work) |
| `main/board_tdeck.cpp`, `main/board_tab5.cpp` | Board layer (`board.hpp`): power, display, touch, keyboard, UI metrics, hints |
| `main/tab5_keyboard.cpp`, `main/tab5_keymap.cpp` | Tab5 Keyboard: I2C driver; key map (host-tested) |
| `main/ssh_terminal.cpp`, `main/command_redact.cpp` | Terminal UI, command parsing, WiFi driver, SSH connection; password redaction |
| `main/vterm.cpp`, `main/term_view.cpp`, `main/font_term_mono_18.c` | Tab5 full-screen SSH terminal (`term_screen.hpp`, `board::term_screen()`; NULL on the T-Deck): xterm emulator (host-tested); LVGL grid view; its monospace font |
| `main/known_hosts.cpp` | Host key verification (TOFU), `hosts` command |
| `main/profile_menu.cpp` | Menu/wizard plumbing, SSH profiles |
| `main/wifi_menu.cpp` | WiFi wizard, saved networks, auto-connect |
| `main/vault_menu.cpp`, `main/secret_vault.cpp` | Encrypted password storage (NVS), optional PIN |
| `main/connection_profiles.cpp` | NVS storage for profiles and networks |
| `main/settings_nvs.cpp` | Picks and opens the settings partition (`nvs` or a launcher's `simplrssh`) |
| `main/storage_menu.cpp`, `main/settings_backup.cpp` | `storage` menu; SD backup/restore/erase |
| `main/sd_card.cpp` | SD mounting (boot and runtime), SSH key loading |
| `main/home_menu.cpp` | Home and Security menus, `go_home()` |
| `main/chat_app.cpp`, `main/openai_client.cpp`, `main/audio.cpp` | ChatGPT app, OpenAI HTTPS client, recording processing |
| `main/audio_tdeck.cpp`, `main/audio_tab5.cpp` | Microphone and speaker drivers per board |

## Workflow

- Build: ESP-IDF v5.5.1, `idf.py build`, release image `idf.py merge-bin -o SimplrSSH-v<ver>-release.bin`,
  launcher image `idf.py launcher-bin` (-> `build/SimplrSSH-v<ver>-launcher.bin`). Tab5: add
  `-B build-tab5 -D IDF_TARGET=esp32p4 -D SDKCONFIG=sdkconfig.tab5` to each command, image
  `SimplrSSH-Tab5-v<ver>-release.bin` (launcher: `build-tab5/SimplrSSH-Tab5-v<ver>-launcher.bin`).
  Tab5 settings: `sdkconfig.defaults` then `sdkconfig.defaults.esp32p4`, committed result
  `sdkconfig.tab5`. Both boards share `partitions.csv` / `partitions_launcher.csv`.
- After changing `main/idf_component.yml`, refresh both lock files. The component manager
  re-solves after installing (see `check_dependencies.py`, KNOWN_DRIFT), so keep the locked
  versions: only the solver's first pass belongs in a lock file.
- Match the surrounding style (4-space indent, `snake_case`, brace on its own line for
  functions, same line for control flow).
- CI must build, and the **Tests** workflow (`.github/workflows/tests.yml`) must pass: it runs
  on every push and pull request, and releases wait for it. Its jobs, in order: install
  (components match each board's lock), lint (actionlint, ruff, cppcheck), typecheck,
  unit-tests, persistence, build (both boards: RAM budget, zero compiler warnings in `main/`, no
  key material), audit (CVEs), network-surface (linked symbols, generated sdkconfig), secret-scan.
  The code-quality workflow stays advisory.
- Tests live in `test/`: `test/host` compiles hardware-independent firmware sources unchanged
  for Linux against in-memory stand-ins for NVS, partitions and logging (`idf_stubs/`,
  `fake_idf.cpp`); `test/guards` holds Python checks of the rules in this file. Keep logic that
  can run off-device free of ESP-IDF/LVGL headers (like `command_redact.cpp`) and test it there;
  extend the guards when adding a rule. Reviewed CVEs go in `.github/audit-baseline.txt`, with a
  reason, only until the fix ships.
- Versions come from git tags; CI computes them (`.github/scripts/plan_release.py`) and
  passes `FIRMWARE_VERSION` to the build. Every push to `main` releases the next patch;
  manual runs pick branch (`main`/`integration` = pre-release), bump or exact version.
  Don't hand-edit versions for patch releases. Work on `integration`, merge to `main` to
  release. The README has no version history (keep it to install/configure/use); release
  notes are GitHub's generated change list, or an optional `### vX.Y.Z (<date>)` README section.
- Update README.md (user-facing behavior) and SECURITY.md (anything security-relevant) with
  the change. Hardware behavior can only be confirmed on a real T-Deck or Tab5; say so when it
  hasn't been.
