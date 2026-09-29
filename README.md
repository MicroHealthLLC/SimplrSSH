# SimplrSSH

An SSH terminal for the **LilyGO T-Deck**. Join WiFi, save your servers, and SSH into them
from the built-in keyboard and trackball. You can also chat with ChatGPT by text or voice.

> SimplrSSH is a fork of [**PocketSSH**](https://github.com/0015/PocketSSH) by
> [Eric (@0015)](https://github.com/0015). Thanks to him for the original project. It is
> released under the same MIT license (see [LICENSE](LICENSE)).

## Supported devices

- **LilyGO T-Deck Plus**
- **LilyGO T-Deck**

No other boards are supported. You also need a USB-C cable to install, and a 2.4 GHz WiFi
network. A FAT32 microSD card is optional; it holds SSH keys and backups.

## Install

Download the latest firmware from the [**Releases**](../../releases) page.

**If your T-Deck runs a launcher** (such as [bmorcelli/Launcher](https://github.com/bmorcelli/Launcher)):

1. Copy `SimplrSSH-v<version>-launcher.bin` to the SD card.
2. In the launcher, open the SD card and install the file.

**Otherwise, flash from your browser** (Chrome or Edge):

1. Connect the T-Deck over USB.
2. Open https://espressif.github.io/esptool-js/, click **Connect** and pick the T-Deck's port.
3. Add `SimplrSSH-v<version>-release.bin` at address **`0x0`** and click **Program**.
4. Press the T-Deck's reset button when it finishes.

If the T-Deck isn't detected, hold the trackball down while pressing reset, then try again.

**To update**, install the newer file the same way. Your settings are kept. Avoid "Erase flash".

**Coming from PocketSSH?** Your existing settings and backups carry over automatically.

## Configure

The device starts at the home menu. Type a number and press Enter, or roll the trackball and
press it. `0` goes back, and `menu` returns home from anywhere.

```
== SimplrSSH ==
 1) WiFi
 2) SSH servers
 3) ChatGPT
 4) Storage (device / SD card)
 5) Security (PIN, server keys)
```

1. **WiFi**: choose **1) WiFi** → **Scan and connect**, pick your network and type the
   password. It is saved, and the device reconnects on its own from then on.
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
4. Pick the key when you add a server.

Keys must be RSA in PEM format. If you add a card while the device is running, type
`storage keys` to load them.

### Optional: ChatGPT

Choose **3) ChatGPT** and add your OpenAI API key. You can type it, or put it in a file named
`openai.key` at the top of the SD card and load it from there. Messages are sent only to
OpenAI, and only while you use ChatGPT.

## Use

- **Connect**: go to **2) SSH servers** → **Connect** and pick a server. Once you are
  connected, everything you type goes to the server.
- **Scroll**: drag up or down on the screen.
- **Special keys**: swipe left to open a panel with Ctrl+C, Ctrl+D, Ctrl+Z, Tab and Esc.
- **History**: roll the trackball up or down.
- **Disconnect**: type `exit` to return to the menu.
- **ChatGPT**: type to chat, or hold the trackball to speak and let go to send.
- **Back up**: choose **4) Storage** → **Back up** to copy your settings to the SD card.
- **Help**: type `help` to list all commands.

Full-screen live tools such as `htop` or `watch` are hard to use on the small screen. Use
one-shot commands instead, for example `top -n 1`.

## Troubleshooting

- **WiFi won't connect**: make sure the network is 2.4 GHz and shows up in the scan.
- **"HOST KEY HAS CHANGED"**: if you know the server was reinstalled, run
  `hosts forget <host>` and connect again.
- **Keys not found**: the card must be FAT32 and the folder must be named `ssh_keys`.
- **"Installed without its settings partition"**: the launcher installed the wrong file.
  Install `-launcher.bin` instead.

## More

- Security details and how to report a vulnerability are in [SECURITY.md](SECURITY.md).
- To build it yourself you need ESP-IDF v5.5.1: run `idf.py build`, then
  `idf.py merge-bin -o SimplrSSH-v<version>-release.bin` and `idf.py launcher-bin`.
- Contributor rules are in [CLAUDE.md](CLAUDE.md).
