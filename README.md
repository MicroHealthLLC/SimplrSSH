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

### v1.3.0 (September 26, 2026)
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
  artifacts (see [Deployment](#deployment)), or ESP-IDF v5.5.1 to [build it yourself](#building-from-source)

### First-Time Setup

Everything is menu driven: type a command, then answer each prompt by typing a number (or text)
and pressing Enter. You can also **roll the trackball** to cycle through the numbered choices and
**press it** to confirm. Type `cancel` (or tap Esc in the special keys panel) at any prompt to back out.

1. **Connect to WiFi** - type `wifi` and choose *Scan and connect*, pick your network, enter the
   password, and answer `y` to save it. The first time you save a password you'll create a
   **master PIN** that encrypts it (see [Saved Passwords and the Master PIN](#saved-passwords-and-the-master-pin)).

2. **Save an SSH server** - type `profile`, choose *Add*, and fill in name, host, port and user.
   Pick an SSH key from the list of keys on the SD card, or choose password login.

3. **Connect** - type `profile`, choose *Connect*, and pick the server. Or type
   `profile connect <name>` directly.

4. **Use the shell** - once connected, anything you type is sent to the server:
   ```
   ls -la
   cd /home
   vim myfile.txt
   ```

5. **Disconnect** - type `exit` (or use *Exit SSH* in the special keys panel) to close SSH;
   `disconnect` turns WiFi off.

On later boots the device **auto-connects to the strongest saved WiFi network in range** once
it boots (it asks for your PIN first if that network's password is saved).
Then just type `profile` and pick a server.

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
Save HomeNet so it auto-connects? (y/n) [y]: y
Saved HomeNet (password encrypted).
```

- **Signal bars**: `####` excellent, `###` good, `##` fair, `#` weak. `open` = no password,
  `saved` = already saved, `connected` = the current network.
- **Saved networks** connect without asking for the password (the PIN is asked once if the vault is locked).
- **Wrong password?** You're asked again; press Enter on an empty password to go back.
- **Switching networks**: pick a different network from *Scan and connect* or *Connect to saved
  network* at any time. Close SSH first (`exit`), since changing WiFi drops the session.
- **Hidden networks**: choose *Hidden network...* and type the SSID.
- **Auto-connect**: at boot the device scans and joins the strongest saved network in range,
  falling back to the next one if that fails. Up to 10 networks can be saved.
- The one-line form still works: `connect <SSID> <PASSWORD>` (use quotes for spaces:
  `connect "My WiFi" "my pass"`). It does not save the network.

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

## Saved Passwords and the Master PIN

WiFi passwords, SSH passwords and SSH key passphrases are **never stored in plain text**. They
are saved as encrypted files on the device's internal `storage` flash partition:

- Each secret is encrypted with **AES-256-GCM** using a random 256-bit data key.
- The data key is itself encrypted with a key derived from your **master PIN**
  (PBKDF2-HMAC-SHA256, 20,000 iterations, random salt). The PIN is never stored.
- File names are hashes, so SSIDs and server names don't appear on flash either.

**Creating the PIN**: the first time you save a password, you're asked to create a PIN
(at least 4 characters; longer is stronger) and type it twice.

**Unlocking**: the vault is locked at every boot. The first time a saved password is needed
(auto-connecting WiFi, or connecting a profile) you're asked for the PIN once; it stays unlocked
until you reboot or type `vault lock`. Press Enter at the PIN prompt to skip.

| Command | What it does |
|---|---|
| `vault` | Show whether a PIN is set and whether the vault is unlocked |
| `vault unlock` | Enter the PIN now |
| `vault lock` | Forget the key until the PIN is entered again |
| `vault pin` | Change the PIN (saved passwords are kept) |
| `vault reset` | Forgot the PIN? Erase all saved passwords and the PIN. Profiles and networks are kept; you'll be asked for their passwords again |

> **Security notes:** the PIN can't be recovered. Anyone who can read the device's flash can try
> to guess a short PIN offline, so use 6+ characters (letters and digits) if the device might be
> lost. Wizard answers (passwords, PINs) are never written to command history, and passwords
> typed on the `connect`/`ssh`/`sshkey` command lines are masked and not stored.

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
| `wifi` / `connect` | WiFi wizard (see [WiFi Wizard](#wifi-wizard)) |
| `connect <SSID> <PASS>` | Connect to WiFi directly (quotes for spaces) |
| `disconnect` | Turn WiFi off |
| `profile` | SSH profiles wizard (see [Connection Profiles Wizard](#connection-profiles-wizard)) |
| `ssh <HOST> <PORT> <USER> <PASS>` | SSH with a password, without a profile |
| `sshkey <HOST> <PORT> <USER> <KEYFILE> [PASSPHRASE]` | SSH with an SD card key, without a profile |
| `vault` | Master PIN and saved passwords (see [above](#saved-passwords-and-the-master-pin)) |
| `hosts` | Trusted server keys (see [Server Keys](#server-keys-known-hosts)) |
| `exit` | Close the SSH session |
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
└── ssh_keys/
    ├── rpi_key.pem          ← Raspberry Pi
    ├── server1.pem          ← Production server
    ├── backup_server.pem    ← Backup server
    └── dev_machine.pem      ← Development box
```

### Key Management

**Multiple Keys**
- Load as many keys as you need
- Each server can have its own key
- Keys are identified by filename
- All keys loaded automatically on boot

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
  (AES-256-GCM, key wrapped under a PBKDF2-derived master PIN key; see
  [Saved Passwords and the Master PIN](#saved-passwords-and-the-master-pin))
- Private keys are read from the SD card into memory only; they are not copied to flash

- SSH host keys verified with trust on first use (see [Server Keys](#server-keys-known-hosts))
- Station-only WiFi (no access point), IPv4 only, no telemetry: the only outbound connections
  are to the WiFi network you choose and the SSH server you enter (plus DNS for its name)

**Security Notes:**
- See [SECURITY.md](SECURITY.md) for the security review, threat model and residual risks
- Flash is not encrypted: profiles, known hosts and command history (non-secret) are readable by
  anyone with physical access to the flash chip. Passwords are protected by the vault PIN
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

### WiFi Connection Issues
- Use `wifi` then *Scan and connect* to check the network is visible and pick it from the list
- Auto-connect needs the network saved (`wifi list`) and in range; if you skip the PIN at boot,
  networks with saved passwords are not tried
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
- **File**: `PocketSSH-v<version>-release.bin` (e.g. `PocketSSH-v1.3.0-release.bin`)
- **Where to get it**:
  - **Releases**: the [Releases page](../../releases) has the image and its `.sha256` for each version
    (published automatically when a new version reaches `main`)
  - **Any commit**: open the *Build firmware* workflow run in the Actions tab and download the
    `PocketSSH-v<version>` artifact
  - **Local build**: `build/PocketSSH-v<version>-release.bin` (see [Building from Source](#building-from-source))
- **Flash Address**: `0x0` (single merged binary)
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

## Building from Source

Requires **ESP-IDF v5.5.1** (other 5.5.x patch releases should work).

```bash
. $IDF_PATH/export.sh
idf.py build
idf.py merge-bin -o PocketSSH-v1.3.0-release.bin   # -> build/PocketSSH-v1.3.0-release.bin
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
| **Build firmware** (`build.yml`) | Push to `main` / `integration`, pull requests, `v*` tags | Builds with ESP-IDF v5.5.1 for the ESP32-S3 and uploads `PocketSSH-v<version>-release.bin` as a workflow artifact. On `main`, a new `PROJECT_VER` is published as a GitHub release with notes, the image and its SHA-256 (see *Releasing* below) | Yes - the firmware must build |
| **Code quality** (`code-quality.yml`) | Push to `main`, pull requests | cppcheck static analysis, flawfinder security lint, and compiler warnings in `main/`, shown as annotations on the changed lines and in each job's summary | No - report only |
| **Dependency check** (`dependency-check.yml`) | Weekly, and when pins change | Compares pinned components and ESP-IDF with the latest releases | No - report only |
| **Dependabot** (`.github/dependabot.yml`) | Weekly | Opens PRs to update the GitHub Actions used above | - |

**Releasing**: releases are automatic from `main`.

1. On a branch (e.g. `integration`), set `PROJECT_VER` in the top-level `CMakeLists.txt`
   to the new version (e.g. `1.4.0`).
2. Add a `### v1.4.0 (<date>)` section to [Version History](#version-history). It becomes
   the release notes, and the build fails if it's missing.
3. Merge to `main`. The *Build firmware* workflow sees that `v1.4.0` hasn't been released,
   builds the firmware, creates the `v1.4.0` tag and a GitHub release titled
   "PocketSSH v1.4.0" with the notes, `PocketSSH-v1.4.0-release.bin` and its `.sha256`.

Pushes to `main` that don't change `PROJECT_VER` only build (the run notes the version is
already released). Pushing a `v<version>` tag or running the workflow manually on `main` also
releases; a tag that doesn't match `PROJECT_VER` fails the build.

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
