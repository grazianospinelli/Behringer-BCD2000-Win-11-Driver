# Behringer BCD2000 ASIO + MIDI Driver: Installation Guide

Graziano Spinelli 2026 · Version 1.0.0.10 · Windows 10/11 (64-bit)

## Overview

This package makes the Behringer BCD2000 work on 64-bit Windows with ASIO audio (4 inputs / 4 outputs, 44.1 kHz, 16-bit) and MIDI (through loopMIDI virtual ports). It is a user-mode driver: it talks to the card through the libusbK USB driver, so no kernel driver or driver-signing changes are needed.

Package contents:

| File | Purpose |
| --- | --- |
| bcd2000asio.dll | The ASIO driver (64-bit) |
| bcd2000asio.ini | Settings file (keep it in the same folder as the DLL) |
| install.bat | Registers the driver (run as administrator) |
| uninstall.bat | Removes the registration (run as administrator) |
| bcd2000asio.cpp, bcd2000asio.rc | Source code |

## Requirements

- Windows 10 or 11, 64-bit.
- A Behringer BCD2000 connected directly to a USB port (avoid USB hubs).
- 64-bit DJ/DAW software with ASIO support (tested with Traktor Pro 4).
- Zadig (free) to install the libusbK driver.
- loopMIDI (free) if you want to use the MIDI controls (buttons, faders, jog wheels, LEDs).

## Step 1: Install the libusbK driver with Zadig

The driver needs libusbK on the BCD2000. Do **not** choose WinUSB: it cannot handle the variable-size isochronous packets the card uses.

1. Download Zadig from https://zadig.akeo.ie and run it as administrator.
2. Connect the BCD2000 and make sure it is powered on.
3. In Zadig, open the **Options** menu and select **List All Devices**.
4. In the device drop-down, select **BCD2000** (USB ID **1397 00BD**). It is a single device with one interface.
5. In the driver selector (the box to the right of the green arrow), choose **libusbK**.
6. Click **Replace Driver** (or **Install Driver**) and wait for the success message. If Windows asks for confirmation, accept.
7. Check in Device Manager: the BCD2000 should now appear under **libusbK USB Devices** (it previously showed Code 28, "no driver").

If the BCD2000 is not listed, check the USB cable, try another port directly on the computer, and make sure **List All Devices** is enabled.

## Step 2: Install loopMIDI (for MIDI)

1. Download and install loopMIDI from https://www.tobias-erichsen.de/software/loopmidi.html
2. Create two virtual ports with exactly these names:
   - `BCD2000 to DAW` (messages from the card to your software)
   - `DAW to BCD2000` (feedback from your software to the card, for example LEDs)
3. Enable the option to start loopMIDI with Windows, so the ports exist before your DJ/DAW software starts.

If you use different port names, set them in the Windows registry under `HKEY_CURRENT_USER\Software\BCD2000Asio` (string values `MidiToHost` and `MidiFromHost`). The audio part works even without loopMIDI.

## Step 3: Install the ASIO driver

1. Copy `bcd2000asio.dll`, `bcd2000asio.ini`, `install.bat` and `uninstall.bat` to a permanent folder, for example `C:\BCD2000Asio`. Do not move the folder after installing.
2. If the files came from a download and Windows marks them as blocked, right-click each file, open **Properties** and tick **Unblock**.
3. Right-click `install.bat` and choose **Run as administrator**.
4. Wait for the message saying that `DllRegisterServer` succeeded.

The driver appears in your software as **Behringer BCD2000**. The DLL is not code-signed, so Windows or your security software may show a warning.

## Step 4: Set up audio (Traktor Pro 4 example)

1. Close any other program that uses the BCD2000, then start Traktor.
2. Open **Preferences > Audio Setup**.
3. Set **Audio Device** to **Behringer BCD2000** (ASIO) and the sample rate to **44100 Hz** (the card only supports 44.1 kHz).
4. Start with a buffer of **512 samples**. Try 256 later if everything is stable.
5. Output routing: Master to outputs 1-2 (**Master L/R**), headphones/monitor to outputs 3-4 (**Phones L/R**). Four inputs (**Input 1-4**) are available.
6. The **Settings** button next to the device opens the driver panel, which shows the MIDI status and session statistics.

## Step 5: Set up MIDI (Traktor Pro 4 example)

1. Open **Preferences > Controller Manager**.
2. Click **Add**, choose **Generic > Generic MIDI**.
3. Set **In-Port** to `BCD2000 to DAW` and **Out-Port** to `DAW to BCD2000`.
4. Import or create a mapping for the BCD2000 (a mapping file is not included in this package).
5. For the jog wheels, set the mapping's encoder mode to **3Fh/41h**.

MIDI is active while your software has the driver loaded (audio and MIDI share the card's single USB interface, so only one program can use the card at a time).

## Settings file (bcd2000asio.ini)

Changes apply the next time the driver is loaded (restart your software).

| Section | Option | Meaning |
| --- | --- | --- |
| General | Log | 0 = no log (default), 1 = write a log to `%TEMP%\bcd2000asio.log` |
| MIDI | JogMode | 0 = no change; 1, 2, 3 = absolute-to-relative conversions (2's complement, 3Fh/41h, signed bit); 4 = BCD2000 fix (default): 0x40 becomes 0x3F so the jog wheels turn both ways |
| MIDI | JogCC | CC numbers of the jog wheels (default 18,19) |
| MIDI | JogLog | 1 = also log the first 400 jog values (only when Log=1) |

## Troubleshooting

- **The driver is not listed in my software.** Run `install.bat` as administrator and read the message. The software must be 64-bit.
- **Error when selecting the driver (BCD2000 not found or in use).** Check that Zadig installed libusbK, that no other program is using the card, and try unplugging and reconnecting it.
- **No MIDI.** Check that loopMIDI is running with the two exact port names, and that the Controller Manager ports match.
- **Jog wheels only go forward.** Check that `JogMode=4` in the ini file and that the mapping uses encoder mode 3Fh/41h.
- **Crackles or dropouts.** Increase the buffer size, connect the card directly to the computer, and close heavy background programs.
- **Still stuck.** Set `Log=1` in the ini file, reproduce the problem, close your software and read `%TEMP%\bcd2000asio.log`.
- **After a Windows or driver update the card stops working.** Run Zadig again and re-install libusbK.

## Uninstall

1. Close your DJ/DAW software.
2. Right-click `uninstall.bat` and choose **Run as administrator**, then delete the folder.
3. Optional: in Device Manager, right-click the BCD2000 under **libusbK USB Devices**, choose **Uninstall device**, tick **Attempt to remove the driver for this device**, and reconnect the card. Without another driver, Windows will show it again as a device with no driver.

## Credits and license

Copyright 2026 Graziano Spinelli. Released under the GNU General Public License, version 3 or later.

This project uses Steinberg's ASIO SDK 2.3.4 (GPLv3 option), libusb (LGPL 2.1 or later, statically linked) and the libusbK driver. The MIDI/USB protocol is based on the Linux snd-bcd2000 driver (GPL). ASIO is a trademark of Steinberg Media Technologies GmbH. Behringer is a trademark of its owner; this project is not affiliated with Behringer or Steinberg.
