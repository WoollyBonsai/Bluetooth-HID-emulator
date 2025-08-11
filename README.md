# Bluetooth HID Emulator

A C++ program that emulates a Bluetooth HID device, allowing you to send keyboard and mouse inputs to other devices.
 This was created by a broke college student for personal use, primarily for playing Minecraft on a phone. 

## Compatibility

- **Bluez version:** 5.65 or newer (tested on 5.65)
- **Operating System:** Debian (and likely other Linux distributions)

## Disclaimer

**This program is designed to capture inputs from a keyboard and mouse only.**

This project was created for personal use and is provided as-is. It may require some troubleshooting to get working on your specific setup.
Also when it runs it captures all of your inputs , hence while its running, you wont be able to use your system.There is a workaround to this, devices connected after running the program and connecting to a client wont be captured.


## Dependencies

- `libbluetooth-dev`
- `libglib2.0-dev`

## Installation

1. **Clone the repository:**
   ```bash
   git clone https://github.com/WoollyBonsai/Bluetooth-HID-emulator.git
   cd Bluetooth-HID-emulator
   ```

2. **Install dependencies:**
   ```bash
   sudo apt-get update
   sudo apt-get install libbluetooth-dev libglib2.0-dev
   ```

3. **Compile the code:**
   ```bash
   g++ bt-hid-emulator-working.cpp -o bt-hid-emulator-working -lbluetooth
   ```

## Configuration

### Bluetooth Service

To allow the HID emulator to work correctly, you need to modify the `bluetooth.service` file to run in compatibility mode and enable the input plugin.

1. **Open the service file for editing:**
   ```bash
   sudo systemctl edit --full bluetooth.service
   ```

2. **Modify the `ExecStart` line:**
   Find the line that starts with `ExecStart=` and add the `--compat` and `-P input` flags. It should look like this:

   ```
   ExecStart=/usr/lib/bluetooth/bluetoothd --compat -P input
   ```

3. **Reload the systemd daemon and restart the Bluetooth service:**
   ```bash
   sudo systemctl daemon-reload
   sudo systemctl restart bluetooth.service
   ```

### main.conf

The `main.conf` file in this directory contains the necessary configuration for the Bluetooth adapter. The following settings are required for this application to work correctly:

- **`AutoEnable=true`**: This ensures that the Bluetooth adapter is enabled automatically on startup.
- **`DiscoverableTimeout=0`**: This keeps the device discoverable indefinitely.
- **`Class = 0x002540`**: This sets the device class to a keyboard/mouse combination.
- **`FastConnectable = true`**: This enables faster connection times.

The provided `main.conf` file already has these settings. You can to copy it to the correct location or adjust the settings if you have something else configured there.**HIGHLY RECOMMEND MAKING A BACKUP OF ORIGINAL CONFIGS**:
```bash
sudo cp main.conf /etc/bluetooth/main.conf
```
### Configuring Input Device

Before running the application, you need to specify which keyboard and mouse devices the program should capture inputs from. This is done by editing the `bt-hid-emulator-working.cpp` file.

1. **Install `evtest`:**
   This utility will help you identify the event device names for your keyboard and mouse.
   ```bash
   sudo apt-get install evtest
   ```

2. **Find your keyboard and mouse devices:**
   Run `evtest` without any arguments. It will list all the input devices and their event numbers.
   ```bash
   sudo evtest
   ```
   You will see output like this:
   ```
   No device specified, trying to scan all of /dev/input/event*
   Available devices:
   /dev/input/event0:      Lid Switch
   /dev/input/event1:      Power Button
   /dev/input/event2:      AT Translated Set 2 keyboard
   /dev/input/event3:      Logitech USB Receiver
   /dev/input/event4:      Logitech USB Receiver Mouse
   /dev/input/event5:      PC Speaker
   Select the device event number [0-5]:
   ```
   Identify the event numbers for your keyboard and mouse. In the example above, the keyboard is `event2` and the mouse is `event4`.

3. **Edit the source code:**
   Open the `bt-hid-emulator-working.cpp` file and find these lines:

   ```cpp
   const char *keyboard_path = "/dev/input/eventX"; // Change eventX to your keyboard's event
   const char *mouse_path = "/dev/input/eventY";    // Change eventY to your mouse's event
   ```

4. **Update the device paths:**
   Change `eventX` and `eventY` to the correct event numbers you found in the previous step. For example:

   ```cpp
   const char *keyboard_path = "/dev/input/event2";
   const char *mouse_path = "/dev/input/event4";
   ```

5. **Recompile the code:**
   After saving your changes, you need to recompile the program:
   ```bash
   g++ bt-hid-emulator-working.cpp -o bt-hid-emulator-working -lbluetooth
   ```

## Usage

1. **Make the launch script executable:**
   'chmod +x launch-bt.sh'

2. **Run the launch script:**
   ```bash
   sudo ./launch-bt.sh
   ```

   This script will make your device discoverable and start the HID emulator.

3. **Connect from your other device:**
   - On your phone or other device, search for Bluetooth devices.
   - You should see a new device named "HID Emulator".
   - Connect to it, and it should register as a keyboard and mouse.
   - **Note :- "Connect from the client to server ( like dont execute connect in bluetoothctl in server system ), as the program identifies incoming connections , not the outgoings."**
   - **Also if anything shows permission related errors , just chown things by your user**
## How it Works

The C++ application uses the BlueZ D-Bus API to create a virtual HID device. It registers a service record with the HID profile and then listens for incoming connections. Once a device is connected, it can send keyboard and mouse reports over the Bluetooth connection.

The `launch-bt.sh` script automates the process of setting up the device and running the emulator. It uses `bluetoothctl` to make the device discoverable and then executes the compiled `bt-hid-emulator-working` program.
