# HaleHound M5StickS3 port

Standalone source copy updated through commit `f571645` of the HaleHound source tree.
This folder contains the M5StickS3 Wi-Fi/BLE port and its shared source dependencies.
Sub-GHz, nRF24, and RFID modules are outside this build.

Build with `pio run -e m5stick-s3`. To flash a connected board, use `pio run -e m5stick-s3 -t upload --upload-port COMx` with its detected port.
Captures and wardriving logs use LittleFS on internal flash.
