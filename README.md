# SimplrSSH

An SSH terminal for the **LilyGO T-Deck** and the **M5Stack Tab5** with its keyboard. Join
WiFi, save your servers, and SSH into them from the keyboard. You can also chat with ChatGPT by
text or voice.

> SimplrSSH is a fork of [**PocketSSH**](https://github.com/0015/PocketSSH) by
> [Eric (@0015)](https://github.com/0015). Thanks to him for the original project. It is
> released under the same MIT license (see [LICENSE](LICENSE)).

## Supported devices

- **LilyGO T-Deck Plus** and **LilyGO T-Deck**: files named `SimplrSSH-v<version>-...`
- **M5Stack Tab5** with the **Tab5 Keyboard** (all screen versions): files named
  `SimplrSSH-Tab5-v<version>-...`. The Tab5 runs it in landscape, on the full 1280x720 screen.
  Tab5 support is new and has not been tested on a Tab5 yet: please report what you find.

Each device has its own files; the other's won't start. No other boards are supported. You
also need a USB-C cable to install, and a 2.4 GHz WiFi network. A FAT32 microSD card is
optional; it holds SSH keys and backups.

## Install

Download the latest firmware from the [**Releases**](../../releases) page.

**If your device runs a launcher** (such as [bmorcelli/Launcher](https://github.com/bmorcelli/Launcher)):

1. Copy `SimplrSSH-v<version>-launcher.bin` (Tab5: `SimplrSSH-Tab5-v<version>-launcher.bin`)
   to the SD card.
2. In the launcher, open the SD card and install the file.

**Otherwise, flash from your browser** (Chrome or Edge):

1. Connect the device over USB.
2. Open https://espressif.github.io/esptool-js/, click **Connect** and pick the device's port.
3. Add `SimplrSSH-v<version>-release.bin` (Tab5: `SimplrSSH-Tab5-v<version>-release.bin`) at
   address **`0x0`** and click **Program**.
4. Press the reset button when it finishes.

If a T-Deck isn't detected, hold the trackball down while pressing reset, then try again. For
a Tab5, hold its reset button for about 2 seconds, until the green LED flashes quickly.

**To update**, install the newer file the same way. Your settings are kept. Avoid "Erase flash".

**Coming from PocketSSH?** Your existing settings and backups carry over automatically.

## Configure

The device starts at the home menu. Type a number and press Enter, or roll the trackball
left/right (Tab5: Up/Down) to pick and press it. `0` goes back, and `menu` returns home from
anywhere. To leave an app from any screen - even an SSH session or ChatGPT, where what you
type goes to the server or the chat - press **Ctrl+Del** on the Tab5 keyboard, or swipe left
and tap **Menu** (the T-Deck's keyboard has no Ctrl key).

```
== SimplrSSH ==
 1) WiFi
 2) SSH servers
 3) ChatGPT
 4) Storage (device / SD card)
 5) Security (PIN, server keys)
 6) Power (sleep, turn off)
```

1. **WiFi**: choose **1) WiFi** → **Scan and connect**, pick your network and type the
   password. It is saved, the menu comes back, and the device reconnects on its own from then
   on (after a restart too, also when it first asks for your PIN).
2. **Add a server**: choose **2) SSH servers** → **Add**. Enter a name, host, port (Enter for
   22) and username, then choose a password or an SSH key. Finish with **Save and connect**.
3. **Trust the server**: on the first connection you are shown the server's key fingerprint.
   Type `y` to trust it. If that server's key ever changes, SimplrSSH refuses to connect.

Saved passwords are encrypted on the device. For extra protection, set a PIN under
**5) Security**. You then enter the PIN once after each restart.

### Optional: SSH keys

1. On your computer, create a key: `ssh-keygen -t rsa -b 4096 -m PEM -f mykey`
2. Add it to the server: `ssh-copy-id -i mykey.pub user@server`
3. Copy `mykey` to the SD card as `/ssh_keys/mykey.pem`, and insert the card.
4. Pick the key when you add a server. If the key has a passphrase, type it when asked (it is
   saved encrypted), or press Enter if it has none. If a key needs a passphrase you didn't
   save, SimplrSSH asks for it when you connect.

Keys must be RSA in PEM format (the file starts with `-----BEGIN RSA PRIVATE KEY-----`). A key
starting with `-----BEGIN OPENSSH PRIVATE KEY-----` can be converted on your computer, keeping
its passphrase: `ssh-keygen -p -m PEM -f mykey`. If you add a card while the device is running,
type `storage keys` to load them.

### Optional: ChatGPT

Choose **3) ChatGPT** and add your OpenAI API key. You can type it, or put it in a file named
`openai.key` at the top of the SD card and load it from there. Messages are sent only to
OpenAI, and only while you use ChatGPT.

## Use

- **Connect**: go to **2) SSH servers** → **Connect** and pick a server. Once you are
  connected, everything you type goes to the server. On the Tab5 the session fills the screen
  as a full terminal (see below).
- **Scroll**: roll the trackball up or down (Tab5: Sym+Up / Sym+Down), or drag on the screen.
  Works everywhere: SSH, menus and ChatGPT. While you are scrolled back, new output doesn't
  move the screen; scroll back to the bottom or start typing to follow it again.
- **Special keys**: swipe left to open a panel with Ctrl+C, Ctrl+D, Ctrl+Z, Tab and Esc. On
  the Tab5 keyboard just press them: Ctrl+letter, Tab and Esc go straight to the server.
- **History**: roll the trackball left (older) or right (newer). Tab5: Up / Down.
- **Disconnect**: type `exit` to return to the menu (on the Tab5 this ends the shell on the
  server).
- **Back to the menu from anywhere**: **Ctrl+Del** (Tab5), or swipe left and tap **Menu**
  (both boards). It ends an SSH session, closes ChatGPT (stopping a recording, reply or
  speech) or cancels a menu, clears the input line, and shows the home menu.
- **ChatGPT**: type to chat, or hold the trackball (Tab5: hold Ctrl) to speak and let
  go to send. Start talking
  once `* Recording` shows. Your speech is transcribed by OpenAI's speech-to-text model, then
  sent as your message to the chat model you picked. If the microphone picks up nothing, the
  device says so and doesn't upload the recording. Press **Esc** (T-Deck: swipe left, then Esc)
  to stop a recording, a reply being written, or a reply being read aloud.
- **Back up**: choose **4) Storage** → **Back up** to copy your settings to the SD card.
- **Sleep**: choose **6) Power** → **Sleep**, or type `sleep`. The screen and WiFi turn off;
  press any key or touch the screen to wake it (T-Deck: the trackball works too). The key or
  touch that wakes it does nothing else. WiFi reconnects by itself, and with a PIN set the
  saved passwords are locked again. Close an SSH session first (`exit`): while connected,
  `sleep` and `shutdown` are sent to the server.
- **Turn off**: choose **6) Power** → **Turn off**, or type `shutdown` (or `poweroff`).
  Command history is saved first. *Tab5*: the Tab5 switches itself off; press the power button
  to turn it on. On USB power it may stay on - then it sleeps instead. *T-Deck*: the screen,
  keyboard, SD card and radio lose power and the processor goes into deep sleep (a very small
  drain); press the trackball to start it again. Only the power switch disconnects the battery
  completely.
- **Help**: type `help` to list all commands.

**T-Deck**: full-screen live tools such as `htop` or `watch` are hard to use, since input is
sent a line at a time. Use one-shot commands instead, for example `top -n 1`.

**Tab5**: full-screen programs work - `vim`, `nano`, `htop`, `top`, `less`, `man`, `tmux`, `mc`
and the like. The SSH session is a color terminal of about 116 x 31 characters
(`xterm-256color`) with frames and line drawing, and every key goes straight to the server, so
the shell's own history, completion and editing work as on a computer. Drag on the screen or
press Sym+Up / Sym+Down to scroll back through the last 1000 lines of output. Inside a
full-screen program, dragging scrolls the program itself (as a mouse wheel if it uses the mouse,
for example `less`, `htop`, or `tmux` with `set -g mouse on`, otherwise as arrow keys), and
Sym+Up / Sym+Down are Page Up / Page Down.

### Tab5 keyboard

| Keys | Does |
|---|---|
| Up / Down | Older / newer command; previous / next choice in a menu |
| Left / Right | Move the cursor. Sym+Left / Sym+Right: start / end of the line |
| Sym+Up / Sym+Down | Scroll the screen |
| Del | Delete the character at the cursor |
| Ctrl+Del | Back to the home menu from any screen: ends an SSH session, closes ChatGPT |
| Aa | Hold for capitals; tap for one capital; tap twice for caps lock |
| Sym + key | The key's second symbol (`?` `/` `<` `>` `{` `}` `\|` `~` `:` `"` `=` `,`) |
| Ctrl + letter | Control code to the server (Ctrl+C, Ctrl+D, Ctrl+Z, Ctrl+L ...) |
| Ctrl (hold on its own) | Talk to ChatGPT while held; held for a second elsewhere, deletes the shown history entry |

In an SSH session the keys go to the server:

| Keys | Sends |
|---|---|
| Arrows | Arrow keys. Sym+Left / Sym+Right: Home / End |
| Sym+Up / Sym+Down | Scroll back through the output; Page Up / Page Down in a full-screen program (dragging on the screen scrolls too) |
| Sym+1 ... Sym+0, Sym+-, Sym++ | F1 ... F10, F11, F12 (for example F10 quits `htop`) |
| Alt + key | The key with Alt (Meta), for programs that use it |
| Enter, Backspace, Tab, Esc, Del, Ctrl + letter | As on a computer |
| Ctrl+Del | Not sent: ends the session and returns to the home menu |

Held keys repeat. The keyboard can be attached or removed while SimplrSSH runs, and the Tab5
charges its battery over USB-C while SimplrSSH runs.

## Troubleshooting

- **WiFi won't connect**: make sure the network is 2.4 GHz and shows up in the scan.
- **SSH "Could not connect"**: the message says why. *Connection refused*: no SSH server on that
  port, or the server is blocking the device (for example fail2ban after failed logins).
  *No answer* / *no route*: wrong address, or the server isn't reachable from this WiFi (a
  different network or a guest network that isolates devices). A host name also shows the
  address it resolved to.
- **"HOST KEY HAS CHANGED"**: if you know the server was reinstalled, run
  `hosts forget <host>` and connect again.
- **Keys not found**: the card must be FAT32 and the folder must be named `ssh_keys`.
- **"Installed without its settings partition"**: the launcher installed the wrong file.
  Install `-launcher.bin` instead.
- **Tab5 power button**: it is wired to the Tab5's power circuit, not to the processor, so
  SimplrSSH can't see it or make it put the device to sleep. A single press turns the Tab5 on,
  a quick double press turns it off. To sleep with wake on any key or touch, use `sleep` (or
  **6) Power** → **Sleep**). If a press leaves the screen dark and the next press brings back
  the boot messages, the Tab5 was off (no drain), not asleep.

## More

- Security details and how to report a vulnerability are in [SECURITY.md](SECURITY.md).
- To build it yourself you need ESP-IDF v5.5.1. T-Deck: run `idf.py build`, then
  `idf.py merge-bin -o SimplrSSH-v<version>-release.bin` and `idf.py launcher-bin`. Tab5: the
  same commands with `-B build-tab5 -D IDF_TARGET=esp32p4 -D SDKCONFIG=sdkconfig.tab5` after
  `idf.py`, and `SimplrSSH-Tab5-v<version>-release.bin` as the image name.
- Every push and pull request runs the **Tests** checks (`.github/workflows/tests.yml`). To run
  the unit tests on a PC (Linux, with `cmake`, `g++` and `libmbedtls-dev`):
  `cmake -S test/host -B build-host && cmake --build build-host && ctest --test-dir build-host`,
  and `python3 -m unittest discover -s test/guards`.
- Contributor rules are in [AI_RULES.md](AI_RULES.md).
