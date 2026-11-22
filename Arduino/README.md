# Arduino sketches

Compile with the Arduino IDE, or from the command line:

    arduino-cli compile --fqbn arduino:avr:mega Arduino/<folder>

Node sketches expect `credentials.h` in the sketch folder. Copy
`credentials.h.example` to `credentials.h` and configure your LoRaWAN session keys.

| Folder | What it does |
|---|---|
| `MCCI_ttn_Raw_DHT22_Dust_Gas_SmartGated_stored_readings_uy1_mega_2` | Deployed node firmware for uy1-mega-2. DHT22, GP2Y dust, gas and battery sensors over LoRaWAN ABP; status frames, stored readings and night mode. |
| `libraries/` | Vendored libraries used by the firmware: LMIC, SX12XX, DHT, OneWire, DallasTemperature, LowPower, RTCZero and others. |
