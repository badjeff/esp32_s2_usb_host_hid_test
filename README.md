# ESP32 S2/S3 HID Host Test

An experimental project to test USB-OTG Host stack on Espressif ESP32 S2/S3 module. This project is based on sample code on [ESP-IDF Programming Guide for ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/index.html). Only tested to build on PlatformIO with platform:espressif32@4.4.0 on ESP32-S2

## What does this experiment do?
- Create tasks like the doc said to intecept as demon and driver tasks
- Fetch endpoints from interfaces, pick one EP-IN to interrupt for in-streaming later
- Get HID Report Descriptor, print it to serial. No parser implemented, need some manual post-processing
- Repeatly, request report packet from EP-IN, print (some of) report packet bytes in binary format to monitoring port

Note: HID Report Descriptor is not parsed due to a simple calibration is need and will do the job in application.

## License
MIT
