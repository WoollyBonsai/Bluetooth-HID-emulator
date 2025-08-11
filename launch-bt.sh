#!/bin/bash
# This script prepares the Bluetooth adapter and runs the HID emulator.

echo "Restarting Bluetooth service to ensure a clean state..."
sudo systemctl restart bluetooth.service
sleep 1

echo "Bringing hci0 interface up..."
sudo hciconfig hci0 up

echo "Configuring Bluetooth adapter..."
# Set the device class to a Keyboard/Mouse combo
sudo hciconfig hci0 class 0x000540

# Set a user-friendly name
sudo hciconfig hci0 name "Woolly HID Emulator"

# Enable Secure Simple Pairing mode
sudo hciconfig hci0 sspmode 1

# Make the device discoverable and connectable
sudo hciconfig hci0 piscan

echo "Configuration complete."
echo "Starting the HID emulator..."

# Run the emulator (it will register its own HID profile)
sudo ./bt-hid-emulator-working
