```
╔══════════════════════════════════════════════════════════════════════╗
║                                                                      ║
║   ██████╗  ██████╗  ██████╗██╗  ██╗███████╗████████╗                 ║
║   ██╔══██╗██╔═══██╗██╔════╝██║ ██╔╝██╔════╝╚══██╔══╝                 ║
║   ██████╔╝██║   ██║██║     █████╔╝ █████╗     ██║                    ║
║   ██╔═══╝ ██║   ██║██║     ██╔═██╗ ██╔══╝     ██║                    ║
║   ██║     ╚██████╔╝╚██████╗██║  ██╗███████╗   ██║                    ║
║   ╚═╝      ╚═════╝  ╚═════╝╚═╝  ╚═╝╚══════╝   ╚═╝                    ║
║                                                                      ║
║    ███████╗███████╗██╗  ██╗    ████████╗███████╗██████╗ ███╗   ███╗  ║
║    ██╔════╝██╔════╝██║  ██║    ╚══██╔══╝██╔════╝██╔══██╗████╗ ████║  ║
║    ███████╗███████╗███████║       ██║   █████╗  ██████╔╝██╔████╔██║  ║
║    ╚════██║╚════██║██╔══██║       ██║   ██╔══╝  ██╔══██╗██║╚██╔╝██║  ║
║    ███████║███████║██║  ██║       ██║   ███████╗██║  ██║██║ ╚═╝ ██║  ║
║    ╚══════╝╚══════╝╚═╝  ╚═╝       ╚═╝   ╚══════╝╚═╝  ╚═╝╚═╝     ╚═╝  ║
║                                                                      ║
╚══════════════════════════════════════════════════════════════════════╝
```
<div align="center">
  <img src="misc/pipboy.gif" alt="PocketSSH 1.2.0">
  <p><em>PocketSSH 1.2.0</em></p>
  <p><a href="https://youtu.be/iMoiEmd5Mq8">📺 Watch Demo Video</a></p>
</div>


# PocketSSH

A portable SSH terminal client for the ESP32-S3 T-Deck Plus, featuring a hardware keyboard, trackball navigation, and touch gesture controls. Connect to remote servers, execute commands, and navigate your terminal with ease on this compact handheld device.

![PocketSSH Screenshot](misc/screenshot_01.jpeg)

## Version History

### v1.4.0 (September 27, 2026)
- **Added**: Home menu at startup - WiFi, SSH servers, ChatGPT, Storage, Security. `menu`
  shows it anytime; `exit` returns to it (closing SSH first) instead of leaving you at a prompt
- **Added**: ChatGPT app - chat with an OpenAI model over WiFi: your messages on the right,
  replies on the left, streaming as they're written. One chat session; `clear` starts over
- **Added**: Voice - hold the trackball to talk (T-Deck microphone -> OpenAI Whisper), let go to
  send; optional spoken replies through the T-Deck speaker
- **Added**: Choose the model (gpt-4o-mini, gpt-4.1-mini, gpt-4.1-nano, gpt-4o or any name); the
  API key is typed or loaded from `openai.key` on the SD card, saved encrypted (PIN-protected if set)
- **Improved**: Full-screen terminal - no borders, one-line status bar, more lines of text; drag
  to scroll back; old lines are trimmed instead of the screen being wiped; SSH servers are told
  the real screen width so output wraps to fit
- **Fixed**: Long messages (over 256 characters) lost their beginning on screen
- **Added**: Install with a launcher such as [bmorcelli/Launcher](https://github.com/bmorcelli/Launcher):
  each release has `PocketSSH-v<version>-launcher.bin`, installed from the launcher's SD menu or
  web UI into a free slot (no flashing at `0x0`). Settings get their own `pocketssh` partition,
  kept across updates, and the launcher's own settings are never touched

### v1.3.2 (September 27, 2026)
- **Fixed**: WiFi networks now always save. Every network you connect to (with the `wifi` menu
  or `connect <SSID> <PASSWORD>`) is saved on the device right away, with its password
  encrypted - no questions, no PIN, no SD card. Previously saving required creating a PIN, and
  skipping that step silently saved nothing
- **Fixed**: After a restart the device reconnects to your saved WiFi by itself, with no PIN
  prompt. If WiFi drops or you're out of range, it keeps reconnecting in the background
- **Added**: Hidden networks auto-connect too; up to 20 networks are remembered (most recent first)
- **Changed**: Saved passwords are encrypted with a key unique to this device and unlock
  automatically. A master PIN is now optional (`vault pin`), for extra protection
- **Changed**: Saved passwords live in the same built-in settings storage as everything else
  (the separate SPIFFS partition is gone, which also removes a slow first-boot format)
- **Improved**: The boot screen shows what is saved ("Saved on this device: 2 WiFi networks, ...")

### v1.3.0 (September 26, 2026)
- **Fixed**: Settings no longer disappear. Saved networks, profiles, trusted servers and
  history live in a larger (256 KB) settings area placed after the app, so flashing a new
  release image keeps them, and it no longer fills up and gets wiped at boot. *Flashing
  v1.3.0 over an older version starts with empty settings one last time.*
- **Added**: `storage` menu - see what is saved on the device, back it up to the SD card,
  restore it, load SSH keys from the SD card without rebooting, or erase the device's settings.
  Everything persists on the device without an SD card
- **Added**: `connect <SSID> <PASSWORD>` offers to save the network (password encrypted), so it
  reconnects after a restart
- **Improved**: Long file names on the SD card (e.g. `backup_server.pem`)
- **Security**: The WiFi driver no longer keeps its own plain-text copy of the WiFi password
- **Security**: SSH server host keys are verified (trust on first use). A new server's
  `SHA256:` fingerprint is shown for you to confirm; a changed key is refused. `hosts` command
  lists and forgets trusted keys
- **Security**: Passwords on `connect`/`ssh`/`sshkey` command lines are masked on screen and kept
  out of command history; old history entries containing them are purged; deleted history
  entries are erased from flash
- **Security**: Removed keystroke and command logging; private-key buffers are wiped after loading
- **Fixed**: Crash/memory corruption when disconnecting while data was arriving (the SSH session
  is now used by one task at a time); SSH handshake and login no longer busy-spin the CPU
- **Removed**: Splash animation (saves ~314 KB flash; the terminal opens immediately)
- **Improved**: ~67 KB less internal RAM (LVGL uses the system heap, WiFi/LWIP/mbedTLS buffers go
  to PSRAM, trackball task stack 8 KB -> 2 KB); image ~366 KB smaller (no SoftAP, IPv6,
  libssh2 debug, unused fonts)
- **Added**: WiFi setup wizard (`wifi` command, or `connect` with no arguments)
  - Scans for networks and shows them strongest first with signal bars; pick one by number
  - Password is typed masked; hidden networks supported
  - Save several networks (up to 10) and switch between them from the menu
  - At boot, auto-connects to the strongest saved network in range
- **Added**: SSH connection profiles wizard (`profile` command)
  - List, add, edit, delete and connect with saved profiles from a numbered menu
  - SSH keys are picked from a list of the keys on the SD card, so nothing has to be typed
- **Added**: Encrypted password vault protected by a master PIN
  - WiFi passwords, SSH passwords and SSH key passphrases are stored as AES-256-GCM encrypted
    files on the internal `storage` flash partition, never in plain text
  - `vault` command to lock, unlock, change the PIN or erase all saved passwords
- **Added**: Passphrase-protected SSH keys (`sshkey ... <PASSPHRASE>` or a saved profile)
- **Added**: Host names are resolved via DNS for `ssh`, `sshkey` and profiles (previously IP addresses only)
- **Improved**: Trackball up/down cycles menu choices; trackball press confirms
- **Improved**: WiFi driver starts once and switches networks cleanly; a dropped connection is reported
- **Fixed**: Touch screen initialization no longer fails to compile with current component
  versions (GT911 I2C config is set field by field at 100 kHz)
- **Build**: All ESP-IDF component versions pinned (`main/idf_component.yml` + `dependencies.lock`)
- **CI**: GitHub Actions build the merged release image, run non-blocking code quality checks
  (cppcheck, flawfinder, compiler warnings), and report outdated dependencies

### v1.2.0 (Faburary 1, 2026)
- **Added**: SSH public key authentication support
  - SD card SSH key management - automatically loads all `.pem` files from `/sdcard/ssh_keys/` on boot
  - Keys stored in memory for use with `sshkey` command
  - Terminal displays list of loaded keys at startup
  - Case-insensitive file extension matching (`.pem`, `.PEM`, etc.)
  - Supports multiple keys for different servers
  - SPI mode SD card access (compatible with T-Deck Plus hardware)
  - New syntax: `sshkey <HOST> <PORT> <USER> <KEYFILE>` (e.g., `sshkey 192.168.1.100 22 pi rpi_key.pem`)
  - Uses `libssh2_userauth_publickey_frommemory()` for in-memory key authentication
  - Supports PEM format RSA keys (generated with `ssh-keygen -m PEM`)
  - More secure alternative to password authentication
  - Displays available keys on error
- **Added**: Quote-aware argument parsing for WiFi credentials
  - Supports SSIDs and passwords with spaces using double quotes
  - Example: `connect "My WiFi Network" "my password"`
  - Works with mixed quoted/unquoted arguments
  - Backwards compatible with space-free credentials
- **Improved**: SSH authentication flexibility with two methods (password and public key)
- **Improved**: Enhanced logging for SD card operations (INFO level)
- **Fixed**: WiFi connection failures with networks containing spaces in SSID

### v1.1.0 (January 14, 2026)
- **Fixed**: Boot loop issue on quick power cycles caused by GPIO4 (strapping pin) ADC initialization
  - Added 100ms delay before ADC initialization to allow GPIO4 to settle after boot
  - Prevents residual voltage from interfering with boot strapping sequence
  - Device now boots reliably regardless of power cycle timing
- **Added**: Touch-to-position cursor functionality
  - Tap anywhere on input text to move cursor to that position
  - Improves text editing experience with precise cursor control
- **Improved**: System stability on rapid power on/off cycles

### v1.0.0 (Initial Release)
- Full SSH2 terminal with PTY support
- Hardware keyboard and trackball navigation
- Touch gesture controls
- Battery voltage monitoring
- Command history with NVS storage

## Key Features

### SSH Terminal
- Full SSH2 protocol support with password and public key authentication
- PTY terminal emulation (vt100) for proper shell interaction
- Real-time command execution and output display
- Handles large data transfers without freezing

### Hardware Controls
- **Physical Keyboard**: Full typing capability via C3 keyboard module
- **Trackball Navigation**: 
  - Up/Down: Navigate through command history
  - Click: Select/execute (when applicable)
- **Touch Screen Gestures**:
  - Swipe right-to-left: Show special keys panel
  - Swipe left-to-right: Hide special keys panel

### Special Keys Panel
Quick access to commonly used control sequences:
- **Ctrl+C**: Interrupt running process
- **Ctrl+Z**: Suspend process
- **Ctrl+D**: EOF signal / logout
- **Ctrl+L**: Clear screen
- **Tab**: Command completion
- **Esc**: Escape key
- **Exit SSH**: Close SSH session
- **Clear**: Clear terminal display

### Display Features
- 320x240 color LCD with LVGL graphics
- Real-time byte counter for data transfer monitoring
- Battery voltage indicator
- Connection status icons (WiFi, SSH)
- Non-blocking display updates prioritize rendering over input

## Hardware Requirements

- **ESP32-S3 T-Deck Plus**
  - ST7789 LCD Display (320x240)
  - GT911 Touch Controller
  - C3 Keyboard Module
  - Trackball (GPIO 1,2,3,15)
  - Battery monitoring (GPIO 4)

## Quick Start

### Prerequisites
- LilyGO T-Deck / T-Deck Plus (ESP32-S3)
- The firmware image `PocketSSH-v<version>-release.bin` from the GitHub releases or Actions
  artifacts (see [Deployment](#deployment)), or ESP-IDF v5.5.1 to [build it yourself](#building-from-source).
  If your T-Deck runs a launcher (e.g. [bmorcelli/Launcher](https://github.com/bmorcelli/Launcher)),
  use `PocketSSH-v<version>-launcher.bin` instead - see [Installing with a Launcher](#installing-with-a-launcher)

### First-Time Setup

The device starts at the **home menu**:

```
== PocketSSH ==
WiFi: HomeNet | Saved: 2 WiFi, 3 SSH
 1) WiFi
 2) SSH servers
 3) ChatGPT
 4) Storage (device / SD card)
 5) Security (PIN, server keys)
Select 1-5, or type a command:
```

Type a number and press Enter (or roll the trackball to pick, press it to confirm). Every
screen has `0) Back`; `menu` or `exit` always brings you home, and `cancel` (or Esc in the
special keys panel) backs out of any prompt. The numbers 1-5 work even when the menu isn't
on screen.

1. **WiFi** (1) - *Scan and connect*, pick your network and enter the password. That's it: the
   network is saved on the device (password encrypted) and it reconnects automatically.

2. **Save an SSH server** (2) - choose *Add*, and fill in name, host, port and user. Pick an
   SSH key from the list of keys on the SD card, or choose password login.

3. **Connect** (2) - choose *Connect* and pick the server. Or type `profile connect <name>`.

4. **Use the shell** - once connected, anything you type is sent to the server:
   ```
   ls -la
   cd /home
   vim myfile.txt
   ```

5. **Disconnect** - type `exit` (or use *Exit SSH* in the special keys panel) to close SSH and
   return to the menu; `disconnect` turns WiFi off.

6. **ChatGPT** (3) - add your OpenAI API key once, then chat by typing or by voice
   (see [ChatGPT](#chatgpt)).

**The screen**: the whole display is text - a one-line status bar at the top (battery, WiFi,
SSH), the terminal, and the input line at the bottom. **Drag up/down on the screen to scroll
back**; the newest ~4 KB of output is kept. Swipe left for the special keys panel.

From then on the device **connects to the strongest saved WiFi network in range by itself**
every time it starts, and reconnects if WiFi drops. No SD card is needed - everything is
saved on the device. Then just type `profile` and pick a server.

## ChatGPT

Chat with an OpenAI model over WiFi: home menu → **3) ChatGPT**, or type `chat`.

```
== ChatGPT ==
Model: gpt-4o-mini | Key: saved | Spoken replies: off
 1) Chat
 2) API key
 3) Model
 4) Spoken replies: turn on
 5) New chat (clear)
 0) Back
```

**First time**: *Chat* asks for your API key (from platform.openai.com → API keys):
*Type it*, or - easier on the T-Deck keyboard - put the key alone in a file named
`openai.key` at the top of the SD card and choose *Load from SD card*. The key is saved
encrypted on the device (PIN-protected if you've set a PIN), and you're offered to delete the
file from the card.

**Chatting**: your messages appear on the right, replies on the left as they're written; the
input line stays at the bottom. There is one chat session; it's kept until you clear it or
restart. Type these words alone as commands:

| Type | What it does |
|---|---|
| `clear` | Start a new chat |
| `settings` | Model, API key, spoken replies |
| `exit` or `menu` | Back to the home menu (the chat is kept) |

**Voice**: **hold the trackball** and speak; the input line shows `* Recording 3s - release to
send`. Let go to send: the T-Deck microphone recording (up to 30 s) is transcribed by OpenAI
Whisper and sent to the model. Turn on *Spoken replies* to hear answers through the speaker.

**Model**: gpt-4o-mini (default, fast and low cost), gpt-4.1-mini, gpt-4.1-nano, gpt-4o, or type
any other model name. Replies are kept short and plain for the small screen.

> **Privacy**: messages and voice recordings are sent to OpenAI (api.openai.com, HTTPS with
> certificate verification) only while you use ChatGPT, and billed to your API key.

## WiFi Wizard

Type `wifi` (or `connect` on its own) to open the WiFi menu:

```
== WiFi ==
Not connected
 1) Scan and connect
 2) Connect to saved network
 3) Forget saved network
 4) Disconnect
 0) Exit menu
Select [0-4]: 1
Scanning for networks...
Networks (strongest first):
 1) Office  ####
 2) HomeNet  ### saved
 3) Cafe  ## open
 4) Hidden network...
 5) Scan again
 0) Back
Select [1-5, 0=back]: 2
Password for HomeNet (Enter = back): ********
Connecting to HomeNet...
Connected to HomeNet.
Saved HomeNet on this device - it will reconnect automatically.
```

- **Signal bars**: `####` excellent, `###` good, `##` fair, `#` weak. `open` = no password,
  `saved` = already saved, `connected` = the current network.
- **Every network that connects is saved** automatically (password encrypted on the device) and
  connects without asking again. Remove one with `wifi forget`.
- **Wrong password?** You're asked again; press Enter on an empty password to go back.
- **Switching networks**: pick a different network from *Scan and connect* or *Connect to saved
  network* at any time. Close SSH first (`exit`), since changing WiFi drops the session.
- **Hidden networks**: choose *Hidden network...* and type the SSID.
- **Auto-connect**: at boot the device joins the strongest saved network in range (then saved
  hidden networks), falling back to the next one if that fails. If WiFi drops or no saved
  network is in range, it keeps trying quietly in the background (after 30 s, then less often,
  up to every 5 minutes). `wifi off` / `disconnect` pauses this until you connect again.
- Up to 20 networks are remembered; connecting to a 21st forgets the one used least recently.
- The one-line form works too: `connect <SSID> <PASSWORD>` (use quotes for spaces:
  `connect "My WiFi" "my pass"`), and saves the network the same way.

| Command | What it does |
|---|---|
| `wifi` | Open the WiFi menu |
| `wifi scan` or `connect` | Scan and pick a network |
| `wifi saved` | Pick a saved network to connect to |
| `wifi list` | List saved networks |
| `wifi forget [SSID or #]` | Forget a saved network and its password |
| `wifi off` or `disconnect` | Disconnect WiFi |

## Connection Profiles Wizard

Save an SSH server once, then connect by picking it from a menu. Type `profile`:

```
== Connection Profiles ==
 1) Connect
 2) List
 3) Add
 4) Edit
 5) Delete
 0) Exit menu
Select [0-5]: 3
-- New profile ('cancel' or Esc to abort) --
Name: pi
Host/IP: 192.168.1.100
Port [22]:
Username: pi
Authentication:
 1) SSH key from SD card
 2) Password
Select [1]: 1
SSH keys on SD card:
 1) rpi_key.pem
 2) server1.pem (passphrase)
Select key [1-2, 0=back]: 1
-- Review --
 Name: pi
 pi@192.168.1.100 [key rpi_key.pem]
 1) Save
 2) Save and connect
 3) Change something
 0) Discard
Select [1]: 2
```

- **Keys** are chosen from the `.pem` files loaded from `/sdcard/ssh_keys/` at boot. Keys marked
  `(passphrase)` are passphrase-protected; you're asked for the passphrase next.
- **Passwords and passphrases** are masked while typing. Type one to save it encrypted, or press
  Enter without typing to be asked each time you connect.
- **Editing** shows the current value in `[brackets]`; press Enter to keep it. For a saved
  password, Enter keeps it and `-` switches to asking each time. Switching to a different key or
  login method drops the old saved secret.
- Host can be an IP address or a host name. Up to 20 profiles.

| Command | What it does |
|---|---|
| `profile` | Open the profiles menu |
| `profile list` | List saved profiles |
| `profile add` | Add a profile |
| `profile connect [NAME or #]` | Connect (omit the name to pick from a list) |
| `profile edit [NAME or #]` | Edit a profile |
| `profile delete [NAME or #]` | Delete a profile and its saved password |

## Saved Passwords

WiFi passwords, SSH passwords and SSH key passphrases are **saved encrypted on the device** and
used automatically - there is nothing to set up.

- Each password is encrypted with **AES-256-GCM** using a random 256-bit data key, stored in the
  device's built-in settings storage (NVS). Record names are hashes, so SSIDs and server names
  don't appear in them either.
- By default the data key is wrapped with a random secret unique to this device, so saved
  passwords **unlock automatically** at boot and WiFi reconnects with no prompts.
- **Optional master PIN** (`vault pin`): the data key is then wrapped with a key derived from
  your PIN (PBKDF2-HMAC-SHA256, 20,000 iterations). You enter the PIN once after each restart
  before saved passwords can be used (WiFi auto-connect asks for it). The PIN is never stored.

| Command | What it does |
|---|---|
| `vault` | How many passwords are saved, and whether a PIN protects them |
| `vault pin` | Add a PIN (or change it) |
| `vault nopin` | Remove the PIN; passwords unlock automatically again |
| `vault lock` / `vault unlock` | With a PIN: lock now / enter the PIN now |
| `vault reset` | Erase all saved passwords (profiles and networks are kept; you'll be asked again) |

> **Security notes:** without a PIN, anyone who can read the device's flash chip can recover the
> saved passwords (the key is on the same chip); SD card backups can't be decrypted on another
> device. Set a PIN (6+ characters) if the device might be lost; the PIN can't be recovered.
> Passwords are never written to command history or logs, and are masked on screen.

## Storage and Backups

**Everything is saved on the device itself** (internal flash) and survives restarts, power-offs
and firmware updates - no SD card needed. That includes saved WiFi networks, SSH profiles,
trusted server keys, command history and the encrypted passwords. The SD card is only used for
SSH key files and optional backups.

Type `storage` to see what is saved and copy it between the device and the SD card:

```
== Storage ==
This device (kept without an SD card):
  3 profiles, 2 WiFi networks, 4 trusted servers
  Saved passwords: encrypted, PIN set
  SSH keys loaded: 2
SD card backup: 3 profiles, 2 WiFi networks, 4 trusted servers
 1) Back up this device to SD card
 2) Restore from SD card to this device
 3) Load SSH keys from SD card
 4) Erase all settings on this device
 0) Exit menu
Select [0-4]:
```

- **Back up** writes `/pocketssh/settings.dat` to the SD card, replacing an older backup only
  once the new one is complete. Saved passwords are included only in encrypted form; the
  device's own key is never copied, so they can't be read from the card.
- **Restore** replaces the device's settings with the backup (after confirming). On the same
  device (or with a PIN) saved passwords keep working; restored onto a different device
  without a PIN, the networks and profiles come back and their passwords are asked for once.
  A damaged or tampered backup is rejected without changing anything.
- **Load SSH keys** reads new `.pem` files from `/ssh_keys` without restarting.
- **Erase** removes all profiles, networks, trusted servers, history and saved passwords
  from the device (the SD card backup is kept).

Shortcuts: `storage backup`, `storage restore`, `storage keys`.

> Flashing a new release image keeps your settings. Choosing "Erase flash" in a flashing tool
> wipes them - back up to the SD card first.
>
> Installed with a launcher, the settings live in PocketSSH's own `pocketssh` partition, which the
> launcher keeps when you install a newer `-launcher.bin`. Uninstalling PocketSSH or letting the
> launcher "remove data" to make room erases them - back up first. Settings don't move between a
> direct-flash install and a launcher install; use *Back up* / *Restore* to carry them over.

## Server Keys (Known Hosts)

The first time you connect to a server, PocketSSH shows the server's host key fingerprint and
asks whether to trust it:

```
New server 192.168.1.100:22
  ED25519 SHA256:uNiVztksCsDhcc0u9e8BujQXVUpKZIDTMczCvj3tD2s
Check it on the server: ssh-keygen -lf /etc/ssh/ssh_host_*_key.pub
Trust this server and continue? (y/n) [n]:
```

Compare it with the output of `ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub` on the server
(or with what your desktop's `ssh` showed the first time). Once trusted, the key is remembered
per `host:port`. If the server ever presents a different key, the connection is **refused** -
someone may be intercepting it. If you know the server was reinstalled, remove the old key and
connect again to review the new one.

| Command | What it does |
|---|---|
| `hosts` | List trusted servers and their key fingerprints |
| `hosts forget <HOST[:PORT] or #>` | Remove a trusted key (port defaults to 22) |

## All Local Commands

These run on the device (they are not sent to the SSH server):

| Command | What it does |
|---|---|
| `menu` | Home menu (1-5 work as shortcuts anytime) |
| `chat` | ChatGPT (see [ChatGPT](#chatgpt)) |
| `wifi` / `connect` | WiFi wizard (see [WiFi Wizard](#wifi-wizard)) |
| `connect <SSID> <PASS>` | Connect to WiFi directly (quotes for spaces) |
| `disconnect` | Turn WiFi off |
| `profile` | SSH profiles wizard (see [Connection Profiles Wizard](#connection-profiles-wizard)) |
| `ssh <HOST> <PORT> <USER> <PASS>` | SSH with a password, without a profile |
| `sshkey <HOST> <PORT> <USER> <KEYFILE> [PASSPHRASE]` | SSH with an SD card key, without a profile |
| `vault` | Saved passwords and the optional PIN (see [above](#saved-passwords)) |
| `hosts` | Trusted server keys (see [Server Keys](#server-keys-known-hosts)) |
| `storage` | What is saved; SD card backup/restore; load keys (see [Storage](#storage-and-backups)) |
| `exit` | Close the SSH session and return to the menu |
| `clear` | Clear the screen |
| `help` | Show the command list |

## SSH Key Setup Guide

PocketSSH supports SSH public key authentication using keys stored on SD card, eliminating the need to type long private keys on the hardware keyboard.

### Quick Start

**1. Generate SSH Key Pair**

On your computer, generate a PEM format RSA key:

```bash
ssh-keygen -t rsa -b 4096 -m PEM -f rpi_key -C "" -N ""
```

This creates two files:
- `rpi_key` - Private key (goes on SD card)
- `rpi_key.pub` - Public key (goes on server)

**2. Install Public Key on Server**

Copy the public key to your server's authorized_keys:

```bash
cat rpi_key.pub | ssh pi@raspberrypi.local 'mkdir -p ~/.ssh && cat >> ~/.ssh/authorized_keys'
```

**3. Copy Private Key to SD Card**

Create the directory structure on your SD card and copy the private key:

```bash
# Insert SD card and mount it (path may vary)
mkdir -p /Volumes/SDCARD/ssh_keys
cp rpi_key /Volumes/SDCARD/ssh_keys/rpi_key.pem
```

**Important**: The directory must be named exactly `ssh_keys` (lowercase) at the root of your SD card.

**4. Use on PocketSSH**

Insert the SD card into your T-Deck Plus and power on. The device will automatically load all `.pem` files from `/sdcard/ssh_keys/` and display them at startup.

Connect using the key filename:

```
wifi MyWiFi password123
sshkey 192.168.1.100 22 pi rpi_key.pem
```

### SD Card Directory Structure

```
/sdcard/
├── ssh_keys/
│   ├── rpi_key.pem          ← Raspberry Pi
│   ├── server1.pem          ← Production server
│   ├── backup_server.pem    ← Backup server
│   └── dev_machine.pem      ← Development box
└── pocketssh/               ← created by 'storage' backups
    └── settings.dat
```

### Key Management

**Multiple Keys**
- Load as many keys as you need
- Each server can have its own key
- Keys are identified by filename
- All keys loaded automatically on boot; after adding keys, `storage keys` loads them without
  restarting
- Up to 8 keys (48 KB in total) are kept in memory

**Supported Formats**
- File extension: `.pem` or `.PEM` (case-insensitive)
- Key type: RSA keys in PEM format only
- Passphrase: Optional. Protected keys show `(passphrase)` in the profile key list;
  generate them with `ssh-keygen -t rsa -b 4096 -m PEM -f rpi_key -N "your passphrase"`
  (AES-128-CBC). Keys encrypted with DES/3DES (e.g. `openssl genrsa -des3`) are not supported.
- Max file size: 16KB per key

**Available Keys Display**
On startup, PocketSSH shows all loaded keys:
```
PocketSSH v1.3.0

Loaded SSH keys:
  - rpi_key.pem
  - server1.pem

Type 'help' for commands
>
```

If no keys are found:
```
No SSH keys found on SD card.
Place .pem files in /sdcard/ssh_keys/
```

**Error Handling**
If you reference a key that doesn't exist:
```
> sshkey 192.168.1.100 22 pi missing_key.pem
Error: SSH key 'missing_key.pem' not found.
Available keys:
  - rpi_key.pem
  - server1.pem
```

### Troubleshooting

**SD card not detected**
- Ensure SD card is FAT32 formatted
- Check SD card is properly inserted before powering on
- Look for "SD card mounted successfully" in startup logs

**Keys not loading**
- Verify directory is named `ssh_keys` (lowercase)
- Check file extension is `.pem` or `.PEM`
- Ensure files are valid PEM format RSA keys
- Check ESP-IDF Monitor logs for detailed error messages

**Authentication fails**
- Verify public key is in server's `~/.ssh/authorized_keys`
- Check file permissions: `chmod 600 ~/.ssh/authorized_keys`
- Ensure private key matches the public key on server
- Try password authentication first to verify credentials

### Advanced: Converting Existing Keys

If you have existing OpenSSH format keys, convert them to PEM:

```bash
# Convert OpenSSH private key to PEM format
ssh-keygen -p -m PEM -f ~/.ssh/id_rsa
```

**Warning**: This modifies your existing key file. Make a backup first:
```bash
cp ~/.ssh/id_rsa ~/.ssh/id_rsa.backup
```



## Architecture

### Framework & Libraries
- **ESP-IDF**: v5.5.1 - Official Espressif IoT Development Framework
- **LVGL**: v9.x - Graphics library with thread-safe display locking (`bsp_display_lock/unlock`)
- **libssh2**: SSH2 protocol implementation (see details below)
- **FreeRTOS**: Multi-task architecture for concurrent operations

### System Tasks
- **keypad_task** (8 KB stack): the only task that runs the terminal - keyboard input, trackball
  events, menus, WiFi scans, encryption and SSH connects
- **trackball_task** (2 KB stack): polls the trackball GPIOs and queues events for keypad_task
- **ssh_receive_task** (8 KB stack): reads SSH data and renders it; all libssh2 calls on the
  session are serialized by a mutex, and a stale task exits after a disconnect
- **LVGL task**: GUI rendering and touch gesture detection

### Display Performance
- Non-blocking input system: `bsp_display_lock(0)` prioritizes screen rendering over input
- Buffered display updates prevent watchdog timeouts during large transfers
- Zero-timeout locks drop input gracefully when display is busy

## Using libssh2

PocketSSH uses **libssh2** for SSH2 protocol implementation, providing secure remote shell access.

### Integration
- **Library**: `skuodi__libssh2_esp` component from ESP Component Registry
- **Configuration**: Managed via `idf_component.yml` in the `main/` directory
- **Version**: Compatible with ESP-IDF v5.x

### Implementation Details

#### Connection Flow
```cpp
1. libssh2_session_init() - Initialize SSH session
2. libssh2_session_handshake() - Perform SSH handshake
2b. libssh2_hostkey_hash(SHA256) - Verify the server key against known hosts
    (new: ask the user to trust it; changed: refuse the connection)
3a. libssh2_userauth_password() - Authenticate with username/password
   OR
3b. libssh2_userauth_publickey_frommemory() - Authenticate with private key
4. libssh2_channel_open_session() - Open SSH channel
5. libssh2_channel_request_pty() - Request PTY for terminal emulation
6. libssh2_channel_shell() - Start shell session
```

#### Public Key Authentication

PocketSSH supports SSH public key authentication using keys loaded from SD card:

**Key Storage:**
- Keys loaded from `/sdcard/ssh_keys/` directory on boot
- Stored in memory using `std::map<std::string, std::string>`
- Referenced by filename in `sshkey` command

**Key Requirements:**
- **Format**: PEM format RSA keys (OpenSSH format not supported by mbedTLS)
- **File Extension**: `.pem` or `.PEM` (case-insensitive)
- **Passphrase**: Optional (AES-encrypted PEM keys); passed to libssh2 from a profile's saved passphrase or the `sshkey` command
- **Max Size**: 16KB per key file

**Usage:**
```
sshkey 192.168.1.100 22 pi rpi_key.pem
```

**Implementation:**
- Uses `libssh2_userauth_publickey_frommemory()`
- Keys loaded via SPI SD card interface before LVGL initialization
- Memory-efficient: Keys stored as std::string in map container
- Automatic discovery: All `.pem` files loaded from `/sdcard/ssh_keys/`

**Security Benefits:**
- No password transmitted over network
- Keys can be easily revoked from server's authorized_keys
- Stronger cryptographic guarantees than passwords
- Immune to brute-force password attacks

See **SSH Key Setup Guide** section above for detailed setup instructions.

#### PTY Terminal Mode
- **Terminal Type**: vt100 emulation
- **PTY Size**: 80 columns × 24 rows (configurable)
- **Mode**: Provides proper terminal interaction with command prompts, colors, and cursor control

#### Data Handling
- **Non-blocking mode**: `libssh2_session_set_blocking(session, 0)`
- **Read buffer**: 4KB chunks processed by `ssh_receive_task`
- **Write operations**: Direct keyboard input → `libssh2_channel_write()`
- **Control sequences**: Special keys (Ctrl+C, Tab, etc.) sent as escape sequences

#### Security Features
- **Protocol**: SSH2 with encryption
- **Authentication**: 
  - Password-based (username/password)
  - Public key-based (RSA keys in PEM format)
- **Session**: Secure encrypted channel for all data transfer
- **Key handling**: Private keys are read from the SD card into memory; they are not copied to flash

### Component Configuration

Add to your `main/idf_component.yml`:
```yaml
dependencies:
  skuodi__libssh2_esp: "^1.1.0"
```

The library automatically handles:
- TCP socket management
- Encryption/decryption
- SSH protocol negotiation
- Keep-alive packets
- Session cleanup

### Troubleshooting libssh2

**Connection fails**: Check network connectivity and SSH server accessibility
```bash
# Test SSH server from another machine
ssh user@host -p port
```

**Authentication errors**: Verify username/password, check server auth methods:
```bash
ssh -v user@host  # Verbose output shows auth methods
```

**Data corruption**: Ensure PTY mode is enabled for proper terminal handling
```cpp
libssh2_channel_request_pty(channel, "vt100");
```

## Example Usage

### Typical Session Flow
```
> connect MyWiFi password123
Connecting to WiFi: MyWiFi
WiFi connected successfully!

> ssh 192.168.1.100 22 pi raspberry
Connecting to 192.168.1.100...
Socket connected, initializing SSH session...
Performing SSH handshake...
SSH handshake successful
Authenticating as pi...
Authentication successful
Opening SSH channel...
SSH channel opened - connected!

pi@raspberrypi:~ $ ls -la
total 32
drwxr-xr-x  4 pi   pi   4096 Jan  7 10:30 .
drwxr-xr-x  3 root root 4096 Jan  1 00:00 ..
-rw-r--r--  1 pi   pi    220 Jan  1 00:00 .bash_logout
...

pi@raspberrypi:~ $ uname -a
Linux raspberrypi 5.15.84-v8+ #1613 SMP PREEMPT Thu Jan 5 12:03:08 GMT 2023 aarch64 GNU/Linux

> disconnect
Disconnected from SSH server
```

## Security Considerations

**Current Security Features:**
- Uses SSH2 protocol with encryption
- Public key authentication, including passphrase-protected keys
- Saved WiFi passwords, SSH passwords and key passphrases are encrypted at rest
  (AES-256-GCM; the key is unique to the device, optionally protected by a PIN; see
  [Saved Passwords](#saved-passwords))
- Private keys are read from the SD card into memory only; they are not copied to flash

- SSH host keys verified with trust on first use (see [Server Keys](#server-keys-known-hosts))
- Station-only WiFi (no access point), IPv4 only, no telemetry: the only outbound connections
  are to the WiFi network you choose and the SSH server you enter (plus DNS for its name)

**Security Notes:**
- See [SECURITY.md](SECURITY.md) for the security review, threat model and residual risks
- Flash is not encrypted: profiles, known hosts and command history (non-secret) are readable by
  anyone with physical access to the flash chip. Saved passwords are only protected from that
  if you set a PIN (`vault pin`)
- **Recommended for production**: enable ESP32 secure boot and flash encryption

## Memory Requirements

- ESP32-S3 with 8 MB PSRAM: bulk buffers (WiFi/LWIP, large mbedTLS and LVGL allocations) live
  in PSRAM; internal RAM is kept for stacks, DMA and small allocations
- About 115 KB of internal RAM is used statically; tasks: 8 KB (keypad), 8 KB (SSH receive),
  2 KB (trackball)
- ~40-50 KB heap per SSH session; up to 8 SD card keys (48 KB total) are kept in RAM

## Known Limitations

**Screen-Intensive Applications:**
- Applications with continuous screen updates (e.g., `htop`, `top`, `watch`) may be difficult to use
- The 320x240 display and rendering performance make real-time updates challenging
- **Recommended**: Use static commands like `ps`, `df`, `ls` instead of interactive/updating tools
- **Alternative**: Use `top -n 1` for single-iteration output instead of continuous mode

**Best Practices:**
- Standard shell commands work well: `ls`, `cd`, `cat`, `grep`, `find`
- Text editors like `nano` and `vim` are usable with patience
- Scripts and one-time commands execute perfectly
- Avoid continuous monitoring tools (`htop`, `tail -f`, `watch`)

## Troubleshooting

### Settings Missing After a Restart or Update
- v1.3.0 and later keep settings across restarts and firmware updates. Updating *to* v1.3.0
  from an older version starts with empty settings once (the settings area moved)
- Flashing with "Erase flash" wipes settings; restore them with `storage` → *Restore*
- If the device shows "Settings storage was unreadable and has been reset", restore a backup
- "Installed without its settings partition": a launcher installed the `-release.bin` image or
  the bare app. PocketSSH then saves into the launcher's own small settings area, which fills up
  quickly. Back up with `storage`, install `PocketSSH-v<version>-launcher.bin` instead, and restore

### WiFi Connection Issues
- Use `wifi` then *Scan and connect* to check the network is visible and pick it from the list
- `wifi list` shows what is saved; any network you've connected to is there. The boot screen
  also shows "Saved on this device: N WiFi networks"
- Auto-connect retries in the background; after `wifi off` / `disconnect` it waits until you
  connect again. With a PIN set, the PIN must be entered after a restart
- Check SSID and password are correct (use quotes if they contain spaces)
- Verify WiFi router is in range
- Ensure WPA2-PSK authentication is supported

### SSH Connection Issues
- **"HOST KEY HAS CHANGED"**: the server presented a different key than the one you trusted.
  Verify with the server's admin; if it was reinstalled, `hosts forget <host>` and reconnect
- Verify server IP and port (default SSH is port 22)
- Check firewall settings on target server
- Ensure SSH server is running (`sudo systemctl status ssh`)
- **For password auth**: Verify username and password are correct
- **For password auth**: Check SSH server allows password authentication (`PasswordAuthentication yes` in sshd_config)
- **For key auth**: Ensure public key is in server's `~/.ssh/authorized_keys`
- **For key auth**: Verify private key is in PEM format (use `ssh-keygen -m PEM`)
- **For key auth**: Check key permissions on server (authorized_keys should be 600)

### Keyboard Not Working
- Check I2C connection to keyboard
- Verify keyboard I2C address (0x55)
- Check I2C bus initialization

### Display Issues
- Ensure LVGL is properly initialized
- Check SPI connection to display
- Verify display driver configuration

## Hardware Pinout

### ESP32-S3 T-Deck Plus
- **Display**: ST7789 SPI LCD (320×240 pixels)
- **Touch**: GT911 I2C touch controller
- **Keyboard**: C3 module via I2C
- **Trackball GPIOs**:
  - GPIO 1: Trackball Up
  - GPIO 2: Trackball Left
  - GPIO 3: Trackball Down
  - GPIO 15: Trackball Right
- **Battery**: GPIO 4 (ADC1 CH3) - Voltage monitoring

## Deployment

### Release Firmware Image

A ready-to-deploy **merged binary** includes the bootloader, partition table, and application:
- **File**: `PocketSSH-v<version>-release.bin` (e.g. `PocketSSH-v1.3.0-release.bin`). Using a
  launcher? Take `PocketSSH-v<version>-launcher.bin` instead, see [Installing with a Launcher](#installing-with-a-launcher)
- **Where to get it**:
  - **Releases**: the [Releases page](../../releases) has the image and its `.sha256` for each version
    (published automatically when a new version reaches `main`)
  - **Any commit**: open the *Build firmware* workflow run in the Actions tab and download the
    `PocketSSH-v<version>` artifact
  - **Local build**: `build/PocketSSH-v<version>-release.bin` (see [Building from Source](#building-from-source))
- **Flash Address**: `0x0` (single merged binary). Flashing it keeps your saved settings, which
  live after the app in flash - don't use "Erase flash" unless you want a clean device
- **Flash Settings**: 
  - **Mode**: DIO (Dual I/O)
  - **Frequency**: 80MHz
  - **Size**: 16MB

### Web-Based Installation (Recommended)

Flash firmware via browser using ESP Web Flasher:
1. Visit: https://espressif.github.io/esptool-js/
2. Connect ESP32-S3 device via USB
3. Click "Connect" and select serial port
4. Add file: `PocketSSH-v1.3.0-release.bin` at offset `0x0`
5. Click "Program" to flash

### Command Line Installation

```bash
esptool.py --chip esp32s3 --baud 921600 write_flash 0x0 build/PocketSSH-v1.3.0-release.bin
```

### Installing with a Launcher

Launchers such as [bmorcelli/Launcher](https://github.com/bmorcelli/Launcher) keep their own
bootloader and partition table, so they can't take an image at `0x0`. Each release has a second
image for them, **`PocketSSH-v<version>-launcher.bin`** (next to the `-release.bin`):

1. Copy `PocketSSH-v<version>-launcher.bin` to the SD card
2. In the launcher, open the SD card, pick the file and install it (the web UI works too)
3. The launcher writes the app to a free slot and creates PocketSSH's `pocketssh` settings
   partition (about 256-448 KB). PocketSSH starts; restart into the launcher as usual to switch apps

To update, install the newer `-launcher.bin` the same way; the `pocketssh` partition and your
settings are kept. The image holds the same app as the release image; only its partition
table differs (`partitions_launcher.csv`). Installed from this image, PocketSSH never touches
the launcher's own `nvs` settings. Installed from the `-release.bin` or the bare app instead, it
has to fall back to that small shared area (a warning is shown at boot) and never erases it.
The launcher runs before PocketSSH and can read or erase any partition - use one you trust.

## Building from Source

Requires **ESP-IDF v5.5.1** (other 5.5.x patch releases should work).

```bash
. $IDF_PATH/export.sh
idf.py build
idf.py merge-bin -o PocketSSH-v1.3.0-release.bin   # -> build/PocketSSH-v1.3.0-release.bin
idf.py launcher-bin                                # -> build/PocketSSH-v<version>-launcher.bin
# Optional: build with a specific version (reconfigure so CMake picks it up)
FIRMWARE_VERSION=1.3.5 idf.py reconfigure build
idf.py -p /dev/ttyACM0 flash monitor               # or flash the merged image at 0x0
```

The first build downloads the ESP-IDF components listed in `main/idf_component.yml` into
`managed_components/`, at the exact versions recorded in `dependencies.lock`.

### Pinned Dependencies

All dependencies are pinned so every build uses the same, tested code:

| Dependency | Version | Pinned in |
|---|---|---|
| ESP-IDF | v5.5.1 | CI workflows (`.github/workflows/*.yml`), `~5.5.1` in `main/idf_component.yml` |
| espressif/esp_bsp_generic | 3.1.1 | `main/idf_component.yml` |
| espressif/esp_lcd_touch_gt911 | 1.2.1 | `main/idf_component.yml` |
| lvgl/lvgl | 9.4.0 | `main/idf_component.yml` |
| skuodi/libssh2_esp | 1.1.0 | `main/idf_component.yml` |
| Everything else (esp_lvgl_port, esp_lcd_touch, ...) | see file | `dependencies.lock` |

To update one: change its version in `main/idf_component.yml`, run `idf.py update-dependencies`,
build, test on a T-Deck, and commit both files. The *Dependency check* workflow lists what is out of date.

### Continuous Integration

| Workflow | When | What it does | Blocks merges? |
|---|---|---|---|
| **Build firmware** (`build.yml`) | Push to `main` / `integration`, pull requests, `v*` tags, manual *Run workflow* | Builds with ESP-IDF v5.5.1 for the ESP32-S3 and uploads `PocketSSH-v<version>-release.bin` (flash at `0x0`) and `PocketSSH-v<version>-launcher.bin` (for launchers) as a workflow artifact. Pushes to `main` release the next patch version automatically; manual runs choose the branch, bump or exact version (see *Releasing* below) | Yes - the firmware must build |
| **Code quality** (`code-quality.yml`) | Push to `main`, pull requests | cppcheck static analysis, flawfinder security lint, and compiler warnings in `main/`, shown as annotations on the changed lines and in each job's summary | No - report only |
| **Dependency check** (`dependency-check.yml`) | Weekly, and when pins change | Compares pinned components and ESP-IDF with the latest releases | No - report only |
| **Dependabot** (`.github/dependabot.yml`) | Weekly | Opens PRs to update the GitHub Actions used above | - |

**Releasing**: versions come from git tags (`v1.3.0`, `v1.3.1`, ...) and releases are automatic.

- **Every merge/push to `main` publishes a release** with the next patch number
  (latest `v1.3.4` → `v1.3.5`). The release is titled "PocketSSH v1.3.5" and has the
  notes, `PocketSSH-v1.3.5-release.bin`, `PocketSSH-v1.3.5-launcher.bin` and their `.sha256`
  files; the device shows the same version.
- **Manual release**: Actions → *Build firmware* → *Run workflow*, then pick:

  | Field | Choices | Default |
  |---|---|---|
  | Branch to build and release | `main` or `integration` | `main` |
  | Version bump | `patch`, `minor`, `major` | `patch` |
  | Exact version | e.g. `1.4.0` (overrides the bump) | empty |

  Leaving everything as is releases the next patch from `main`. Releases from
  `integration` are published as **pre-releases** (not "Latest"), for testing before a merge.
- **Pushing a tag** `vX.Y.Z` releases exactly that version.
- Pushes to `integration` and pull requests only build; their artifacts are versioned
  `<next>-dev.<commit>` (e.g. `1.3.5-dev.4733f93`).

**Release notes**: if [Version History](#version-history) has a `### vX.Y.Z (<date>)` section
for the version, it heads the release notes; GitHub then lists the merged pull requests and
commits since the previous release. Without a section, that generated list is the notes -
nothing is required for a patch release.

**Bigger version jumps**: pick `minor`/`major` or an exact version when running the workflow,
or raise the fallback version in `CMakeLists.txt` (`set(PROJECT_VER "1.4.0")`); a fallback
higher than the latest release is used as the next version. Local builds show that fallback
version; CI sets the real one (`FIRMWARE_VERSION`).

A release never overwrites an existing one: if the version already exists, the run fails
with a message instead.

## Credits

- ESP-IDF framework by Espressif Systems
- LVGL graphics library
- libssh2 SSH protocol library
- FreeRTOS real-time operating system

## License

This project uses various ESP-IDF components and open-source libraries:
- **ESP-IDF**: Apache License 2.0
- **LVGL**: MIT License
- **libssh2**: BSD-style license

See individual component licenses for complete details.
