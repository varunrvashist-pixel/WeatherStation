# Belmont DIY Weather Station

A DIY weather station built by Varun in Belmont, California. It measures temperature, humidity, pressure, wind, rain and a fog indicator, and logs a reading every minute to a Google Sheet. The long-term goal is hyperlocal fog forecasting.

## Hardware
- Arduino Nano ESP32
- 2 x BME280 (temperature, humidity, pressure)
  - Primary: inside a radiation shield, I2C address 0x76
  - Secondary: unshielded, I2C address 0x77, used for the radiation-shield experiment
- SparkFun Weather Meter Kit (anemometer, wind vane, rain gauge)
- VCNL4200 proximity and ambient light sensor, used as an experimental fog sensor

## Pins
| Sensor | Pin |
|---|---|
| Anemometer | D2 |
| Rain gauge | D4 |
| Wind vane | A0 |
| BME280 and VCNL4200 | I2C (SDA/SCL) |

## What the code does
- Reads all sensors and prints to the serial monitor every 3 seconds.
- Averages wind speed over 60 seconds and uploads a row to Google Sheets through a Google Apps Script web app.
- Supports over-the-air (OTA) firmware updates over Wi-Fi.
- Experimental fog flag: proximity above a slow-moving baseline, humidity of at least 85%, and a 2-minute persistence rule.

## Setup
1. Install the Arduino IDE and add the Arduino ESP32 board support.
2. Install the libraries: Adafruit BME280, Adafruit Unified Sensor, Adafruit VCNL4200.
3. In the sketch, replace the placeholders with your own values:
   - `ssid` and `password` for your Wi-Fi
   - `scriptURL` for your Google Apps Script web app URL
   - `otaPassword` for firmware updates
4. Upload to the board.

The real credentials are not in this repository.

## Projects and goals
- 2027 science fair: baseline calibration of the two sensors, a radiation-shield experiment, and fog detection checked against airport METAR reports.


## Status
Work in progress. The fog sensor does not yet reliably detect fog, and the results will be documented as the experiments finish.
