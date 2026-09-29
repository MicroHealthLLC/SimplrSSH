# Security

SimplrSSH is a handheld SSH client for the LilyGO T-Deck (ESP32-S3). Its job is to join a
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
