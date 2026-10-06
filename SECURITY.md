# Security

SimplrSSH is a handheld SSH client for the LilyGO T-Deck (ESP32-S3) and the M5Stack Tab5
(ESP32-P4, WiFi through an ESP32-C6 co-processor), built from one source tree. Its job is to join a
WiFi network the user picks, open SSH sessions to servers the user enters, and - only when
the user opens ChatGPT with their own API key - talk to api.openai.com. It should talk to
nothing else.

## Reporting a vulnerability

Please open a private security advisory on the GitHub repository (Security tab, "Report a
vulnerability") rather than a public issue.

## Threat model

| Asset | Protected against | How |
|---|---|---|
| SSH / WiFi passwords, key passphrases at rest | Leaking through backups, logs or plain-text config | AES-256-GCM vault in NVS; data key wrapped with a random per-device secret that is never backed up. With the optional PIN, also against reading the flash chip (PBKDF2 20k iterations; PIN never stored) |
| SSH session | Network attacker impersonating the server (MITM) | Host key fingerprints, trust on first use, changed keys refused |
| Secrets in use | Shoulder surfing, serial logs, command history | Masked input and echo, no secret logging, secrets kept out of history, buffers wiped after use |
| OpenAI API key | Leaking through backups, logs or plain text | Stored in the vault like other passwords (PIN-protected if set); sent only to api.openai.com over TLS verified with the ESP-IDF CA bundle |
| Chat content and voice | Unexpected destinations | Sent only to OpenAI, only while the user is in ChatGPT; the chat stays in RAM (not saved); recordings are freed after transcription |
| Device | Unexpected network exposure | Station-only WiFi (no access point / DHCP server), IPv4 only, no listening sockets, no telemetry/OTA/cloud |

Not in scope: an attacker with physical access *and* time to brute-force a short PIN offline
(use 6+ characters), or one who can reflash the device (enable secure boot + flash encryption
for that).

## Security review (2026-09-26)

A full sweep for malicious code, unexpected data flows and vulnerabilities.

### Malware / data exfiltration: none found

- **Project code (`main/`)**: the only network activity is WiFi scan/join, DNS resolution of
  the SSH host the user typed, and the TCP connection to that host. No other sockets, HTTP,
  MQTT, SNTP, mDNS or hard-coded addresses.
- **Third-party components**: no component other than libssh2 opens sockets or resolves names.
  `skuodi/libssh2_esp` 1.1.0 contains libssh2 sources **byte-identical** to upstream commit
  [`4ed26f5`](https://github.com/libssh2/libssh2/commit/4ed26f5740bdd409269ed9fb48a28bf8f565b681)
  (2025-10-20); its ESP-IDF glue (CMake, `compat.h`, `libssh2_config.h`, Kconfig) is benign.
  Espressif (`esp_bsp_generic`, `esp_lvgl_port`, `esp_lcd_*`) and LVGL components are official
  releases, verified against the registry hashes in `dependencies.lock` on download.
- **Built firmware**: no URLs, domains or IP addresses other than SSH algorithm names
  (`...@openssh.com`) and the `192.168.1.100` example in the help text.
- **"CN" in the firmware**: this is ESP-IDF's default WiFi *regulatory* country code (it
  selects channels 1-13); it is not a network destination.
- **Residual trust**: Espressif's WiFi driver ships as closed-source binary libraries, as on
  every ESP32 product. They cannot be audited; they are the vendor's signed releases.

### Vulnerabilities found and fixed

| # | Finding | Severity | Fix |
|---|---|---|---|
| 1 | No SSH host key verification: any machine on the network could impersonate a server and capture passwords | High | Trust-on-first-use known hosts (`main/known_hosts.cpp`), changed keys refused, `hosts` command |
| 2 | libssh2 session used concurrently by the keypad and receive tasks; `exit` freed the session while the receive task was reading (use-after-free) | High | Mutex around every libssh2 call, receive task exits via a generation counter |
| 3 | Passwords from `connect`/`ssh`/`sshkey` echoed on screen and stored in plain-text command history; deleted history entries left in flash | High | Secrets masked on screen and never stored; existing entries purged on boot; stale NVS keys erased |
| 4 | Every keystroke and every command sent to the server logged (INFO) | Medium | Logging removed |
| 5 | SSH handshake/authentication loops spun the CPU without waiting | Medium | Bounded `select()` waits (max ~30 s per step) |
| 6 | Unlimited SD card keys kept in RAM | Low | Max 8 keys / 48 KB; key file buffers wiped after loading |
| 7 | GitHub Actions and the build container referenced by mutable tags | Low | Actions pinned to commit SHAs, `espressif/idf` image pinned by digest; Dependabot updates them |
| 8 | Unused attack surface and code: SoftAP + DHCP server, IPv6, libssh2 debug tracing, splash animation | Low | Disabled/removed (see `sdkconfig.defaults`) |

### Persistence review (2026-09-27)

| Finding | Fix |
|---|---|
| The WiFi driver stored its own plain-text copy of the WiFi password in NVS | `CONFIG_ESP_WIFI_NVS_ENABLED` off; networks are saved by the app with the password in the vault |
| SD card backups could carry secrets or be tampered with | Backups hold only non-secret settings plus the already-encrypted vault files; restore accepts only known namespaces, NVS-sized keys/values and vault file names (no paths), and validates the whole backup before changing anything |

### SD card review (2026-09-28)

| Finding | Fix |
|---|---|
| After unmounting, the SD driver left the card's CS pin (GPIO 39) as a floating input while the display kept clocking the shared SCK/MOSI lines, so the card could accept pixel data as commands and write to random sectors (corrupted FAT32 cards) | CS is driven high after every unmount or failed mount |
| At boot the peripheral power pin (GPIO 10) and chip selects pulsed low while being configured, and the SD card was initialised immediately after power-on: a card talked to while its supply is coming up or dipping can damage its internal tables and become unreadable | Pins get their level before becoming outputs (no glitch); 250 ms settle before the SD card is touched; boot mount uses the same 10 MHz clock as the runtime mount |
| Reading the card could write to it (boot created `/ssh_keys`) | Mounts are read-only by default (a disk driver that refuses writes); only backup and deleting `openai.key` mount writable; the card is never formatted |

### Usability change (2026-09-27)

Requiring a master PIN before any password could be saved meant networks silently weren't
saved when the PIN step was skipped, and every restart stopped at a PIN prompt. Saved
passwords now default to a random per-device key (automatic unlock, WiFi reconnects on its
own); the PIN is optional. Trade-off: **without a PIN, someone with the device and a flash
reader can recover saved passwords**, since the key is on the same chip. SD card backups
remain unreadable elsewhere (the device key is never exported). Set a PIN, or enable flash
encryption, for devices that may be lost.

### Continuous checks (2026-10-03)

Every push and pull request runs the **Tests** workflow (`.github/workflows/tests.yml`), and
a release waits for it to pass on the same commit. Security-relevant gates:

- **Secrets**: host tests check that the vault never stores or logs plaintext, that tampered or
  moved ciphertext is rejected, that PINs use the slow key derivation, and that typed passwords
  are redacted from echo and history; guard tests reject log statements that print user input
  or secrets; gitleaks scans the new commits; the built images are scanned for private keys and
  API keys (with a planted-key negative control).
- **Untrusted SD card input**: restore tests feed damaged and hostile backups (unknown
  namespaces, the device-key namespace, oversized or malformed records) and check that the
  device is left unchanged.
- **Network surface**: the linked firmware (both boards) may not contain listening sockets, HTTP
  servers, OTA (including the Tab5 co-processor's), Bluetooth, mDNS or access-point code, must
  link CA-bundle TLS verification, SSH host key checks and RAM-only WiFi credential storage,
  and its generated sdkconfig must keep SoftAP, DHCP server, IPv6 and Bluetooth off.
- **Supply chain**: actions pinned by SHA and images by digest (enforced), components installed
  only at the locked versions with verified hashes (each board has its own lock file), and a CVE
  audit (esp-idf-sbom, NVD) of ESP-IDF, its libraries and the components of both images that
  blocks on new high/critical findings.

Found while introducing these checks:

| Finding | Status |
|---|---|
| WiFi network names (SSIDs) were written to the serial log | Fixed: removed from all log lines |
| The ESP-IDF v5.5.1 component manager re-solves dependencies after installing the locked ones, so builds compiled LVGL 9.6.0~1 and cmake_utilities 1.1.1 instead of the locked 9.4.0 / 0.5.3 (builds were not reproducible from `dependencies.lock`) | Open: the exact drifted versions are pinned in `.github/scripts/check_dependencies.py`, so any further change fails the gate. Fix by regenerating the lock so both passes agree (changes the firmware; needs testing on a T-Deck) |
| ESP-IDF v5.5.1 and its bundled mbedTLS 3.6.4 have published high/critical CVEs (11 at the time) | Open: listed in `.github/audit-baseline.txt` pending an ESP-IDF upgrade (needs testing on a T-Deck); not yet assessed for reachability |

### M5Stack Tab5 support (2026-10-05)

A second firmware image (`SimplrSSH-Tab5-*`) for the M5Stack Tab5 with the Tab5 Keyboard,
built from the same sources; only the board layer differs (`main/board_tab5.cpp`,
`tab5_keyboard.cpp`, `tab5_keymap.cpp`, `battery_tab5.cpp`, `audio_tab5.cpp`).

| Change | Security review |
|---|---|
| WiFi runs on the Tab5's ESP32-C6 co-processor, reached over SDIO through `espressif/esp_hosted` 3.0.9 and `espressif/esp_wifi_remote` 1.6.5; the app keeps calling the usual `esp_wifi_*` API | The SDIO link is a local bus, not a network interface. Same rules as the T-Deck: station mode only (`CONFIG_WIFI_RMT_SOFTAP_SUPPORT` off), no Bluetooth over the co-processor (`CONFIG_ESP_HOSTED_ENABLE_BT_*` off), and the co-processor's own update mechanism (`esp_hosted_slave_ota*`) is not linked (checked in CI) |
| The C6 would keep its own plain-text copy of the WiFi password in its flash | Both boards now call `esp_wifi_set_storage(WIFI_STORAGE_RAM)` after starting WiFi, so the WiFi driver (on the Tab5: the co-processor) keeps credentials in RAM only; CI requires the call to be linked. Note: a password sent by older firmware on the C6 (e.g. M5Stack's demo) may still be in the C6's flash |
| New components: `espressif/m5stack_tab5` 1.3.1 (BSP), `esp_lvgl_port` 2.8.0~1, display/touch drivers (ILI9881C, ST7123/ST7121, GT911), PI4IOE5V6408 IO expander, `esp_codec_dev` 1.5.11, and the BSP's declared camera/USB/IMU components | Official Espressif registry releases, pinned with hashes in `dependencies.lock.esp32p4`. None opens sockets. The BSP declares camera (`esp_video`), USB host and IMU components that SimplrSSH never starts: no camera sensor drivers are built, and the linked image contains no USB host, video or `eppp` code (checked with `nm`) |
| Tab5 Keyboard on its own I2C bus (an STM32 with its own firmware) | Its key events are untrusted input: positions outside the 5x14 matrix are ignored, the event queue is bounded (16 keys), and no key data is logged |
| SD card on its own SDMMC slot | Same read-only-by-default, never-format, validate-everything rules as the T-Deck |

Hardware behaviour on the Tab5 has not been confirmed on a device yet.

### Tab5 full-screen terminal, key passphrases (2026-10-06)

| Change | Security review |
|---|---|
| The Tab5 shows SSH sessions in a terminal emulator (`main/vterm.cpp`, xterm subset) and asks the server for an `xterm-256color` pty. The T-Deck is unchanged (escape codes stripped, line input) | Server output is untrusted input to a parser: numeric parameters are capped (9999) and clamped to the screen, at most 16 parameters are kept, title/device strings are skipped without being stored, and the cell arrays are allocated once at start-up (no growth from input). Host tests include hostile parameters and 200,000 random bytes under AddressSanitizer/UBSan. The emulator answers only cursor-position, device-type and screen-size queries (bounded to 256 bytes per chunk); it never reports titles, clipboard or anything typed. Of mouse reporting only the wheel is sent (when the program enables it and the user drags on the screen); clipboard (OSC 52) and title reporting are not implemented. On-screen text, including scrollback (1000 lines, PSRAM), is not logged or saved |
| Every key goes to the server while connected on the Tab5 (Alt sends Esc + key) | Same data as typing a line on the T-Deck; nothing typed into the session is logged or kept in command history |
| Key passphrase offered for every key; if a key turns out to be locked when connecting, the passphrase is asked on the open connection (3 tries) | Masked like every secret, held in the pending-connection record and wiped after the attempt; never logged. The host key is verified before it is asked. Keys in OpenSSH format (unsupported by libssh2 with mbed TLS) are reported with a conversion hint instead of a passphrase prompt |

### Known residual risks

- Flash is not encrypted. Non-secret data (profiles, known hosts, command history, SD keys while
  inserted) can be read from the chip, and from an SD card backup. Enable secure boot and flash encryption for high-risk use.
- Private keys on the SD card are stored however you copied them; prefer passphrase-protected keys.
- Deleted NVS entries are marked erased but may physically remain until the NVS page is recycled.
- A server that adds a new host key type can trigger a "key changed" refusal; verify, then
  `hosts forget`.
- **Installed with a launcher** (`-launcher.bin`, e.g. bmorcelli/Launcher): the launcher's own
  code runs first, before SimplrSSH, and can read, back up, restore and erase every partition,
  including SimplrSSH's `simplrssh` settings partition (the vault stays encrypted; without a
  PIN its key is in the same partition). Only use a launcher you trust, and set a PIN.
  Installed from `-launcher.bin`, SimplrSSH never touches the launcher's `nvs`; installed from
  an image without its own partition it has to store its settings there (warned at boot) and
  never erases it.
- `skuodi/libssh2_esp` tracks a libssh2 development snapshot rather than a tagged release.
  Re-verify it against upstream (see above) when updating.
- **Tab5**: the ESP32-C6 runs Espressif's closed WiFi stack plus whatever esp_hosted firmware
  the Tab5 shipped with; SimplrSSH neither updates nor verifies it. A compromised C6 sees WiFi
  traffic (still TLS / SSH protected end to end), as a compromised WiFi driver would.
- The CVE audit only sees packages with a CPE in the SBOM (ESP-IDF, mbedTLS, lwIP, FreeRTOS,
  newlib, cJSON, wpa_supplicant...). Registry components such as libssh2 and LVGL are listed but
  not matched against the NVD; review their upstream advisories when updating them.
