# Haven nRF5340 firmware — nRF Connect SDK (Zephyr) application
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(haven_zephyr_app)

target_sources(app PRIVATE
  src/main.c
  src/ble_transport.c
  src/protocol.c
  src/adau1860_control.c
  src/gatt_audio_service.c
)
