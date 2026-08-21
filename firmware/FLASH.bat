@echo off
setlocal
cd /d "%~dp0"
title Fieldline - flash ESP32
echo.
echo  Fieldline firmware flasher
echo  --------------------------
echo  Plug the ESP32 in via USB. If it has a BOOT button, hold it when you see "Connecting...".
echo.
echo  Searching COM ports...
esptool.exe --chip esp32 --baud 921600 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 40m --flash_size 4MB 0x1000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 fieldline_app.bin
if errorlevel 1 (
  echo.
  echo  Fast flash failed - retrying at 115200 baud...
  esptool.exe --chip esp32 --baud 115200 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 40m --flash_size 4MB 0x1000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 fieldline_app.bin
)
if errorlevel 1 (
  echo.
  echo  Still failing. Check: USB cable carries data, CP210x/CH340 driver installed, no other program holds the COM port.
) else (
  echo.
  echo  DONE. The LCD should say "Fieldline booting".
  echo  First boot: on your phone join Wi-Fi "Fieldline-Setup", choose your home Wi-Fi, save.
  echo  Then open the IP shown on the LCD in any browser, or http://fieldline-node.local
)
echo.
pause
