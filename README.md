# Art Matrix<!-- omit in toc -->

A 128×128 LED matrix that cycles through animated patterns, clocks, weather and more. Pick what it shows from your phone, and install new plugins over WiFi.

- [Specs](#specs)
- [Help](#help)
  - [Setting Up](#setting-up)
  - [Changing WiFi Details](#changing-wifi-details)
  - [Updating/Reflashing Firmware](#updatingreflashing-firmware)
  - [Installing Plugins](#installing-plugins)

## Specs

* 128×128 RGB LED matrix
* RP2350 + 16MB flash + 8MB PSRAM
* WiFi + Bluetooth
* microSD card slot
* A, B, C and USER buttons
* 4 x rear RGB LEDs
* 2 x Qw/ST connectors
* 1 x Analog sensor port

## Help

### Setting Up

* Plug in your Art Matrix. It will start playing its built-in plugins and open a WiFi network called "Art Matrix Setup".
* Join that network on your phone.
* Enter your WiFi details and pick a name for your Art Matrix.
* Once it's connected, visit `http://<name>.local` to install new plugins and change settings.

### Changing WiFi Details

* Hold the A and C buttons.
* Briefly tap the RESET button.
* Your Art Matrix will go back into setup mode. Follow the steps in [Setting Up](#setting-up).

### Updating/Reflashing Firmware

:warning: Our firmware comes in two flavours:

1. `artmatrix-vX.X.X-micropython-with-filesystem.uf2` which will replace your settings, WiFi details and plugins with the defaults, and

2. `artmatrix-vX.X.X-micropython.uf2` which will replace only the firmware.

Pick your desired firmware image from the latest release at [https://github.com/pimoroni/artmatrix/releases/latest](https://github.com/pimoroni/artmatrix/releases/latest)

Then:

* Connect your Art Matrix to your computer with a USB Type-C cable.
* Press and hold the BOOT button.
* Briefly tap the RESET button.
* A disk named "RP2350" should appear on your computer.
* Drag and drop the firmware .uf2 onto this disk.
* Your Art Matrix will update and then reboot!

### Installing Plugins

* Visit `http://<name>.local` and press "Discover more plugins".
* Pick the plugins you want and they'll be downloaded straight onto your Art Matrix.

Plugins live in their own repository at [https://github.com/pimoroni/artmatrix-plugins](https://github.com/pimoroni/artmatrix-plugins)
