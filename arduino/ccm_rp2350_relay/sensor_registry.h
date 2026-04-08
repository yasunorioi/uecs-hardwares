#pragma once
#include <Arduino.h>

// Sensor type enum
enum SensorType {
  SENSOR_NONE = 0,
  SENSOR_SCD41,    // 0x62 - CO2/Temp/Hum
  SENSOR_SHT40,    // 0x44 - Temp/Hum
  SENSOR_BMP280,   // 0x76 or 0x77 - Pressure/Temp
  SENSOR_BH1750,   // 0x23 or 0x5C - Lux (future)
  SENSOR_ADS1110,  // 0x48 - 16bit ADC (M5Stack ADC Unit V1.1)
};

// I2C address -> sensor mapping
struct SensorEntry {
  uint8_t addr;
  SensorType type;
  const char* name;
};

static const SensorEntry SENSOR_REGISTRY[] = {
  {0x62, SENSOR_SCD41,  "SCD41"},
  {0x44, SENSOR_SHT40,  "SHT40"},
  {0x76, SENSOR_BMP280, "BMP280"},
  {0x77, SENSOR_BMP280, "BMP280"},  // alternate address
  {0x23, SENSOR_BH1750, "BH1750"},
  {0x5C, SENSOR_BH1750, "BH1750"},  // alternate address
  {0x48, SENSOR_ADS1110, "ADS1110"},  // M5Stack ADC Unit V1.1
  {0x49, SENSOR_ADS1110, "ADS1110"},  // alternate address (ADDR=Vs)
};
static const int SENSOR_REGISTRY_SIZE = sizeof(SENSOR_REGISTRY) / sizeof(SENSOR_REGISTRY[0]);

// HA Discovery field definition
struct HAField {
  const char* key;        // MQTT payload JSON key
  const char* name;       // HA display name
  const char* unit;       // Unit of measurement
  const char* dev_class;  // HA device_class
  const char* icon;       // MDI icon (nullptr = dev_class default)
  SensorType source;      // Which sensor provides this
};

static const HAField HA_FIELDS[] = {
  {"co2",               "CO2",                 "ppm",  "carbon_dioxide",       "mdi:molecule-co2", SENSOR_SCD41},
  {"temperature",       "Temperature (SCD41)", "\xC2\xB0""C", "temperature",  nullptr,            SENSOR_SCD41},
  {"humidity",          "Humidity (SCD41)",     "%",    "humidity",             nullptr,            SENSOR_SCD41},
  {"sht40_temperature", "Temperature (SHT40)", "\xC2\xB0""C", "temperature",  nullptr,            SENSOR_SHT40},
  {"sht40_humidity",    "Humidity (SHT40)",     "%",    "humidity",             nullptr,            SENSOR_SHT40},
  {"pressure",          "Pressure (BMP280)",    "hPa",  "atmospheric_pressure", nullptr,           SENSOR_BMP280},
  {"bmp280_temperature","Temperature (BMP280)", "\xC2\xB0""C", "temperature",  nullptr,            SENSOR_BMP280},
};
static const int HA_FIELDS_SIZE = sizeof(HA_FIELDS) / sizeof(HA_FIELDS[0]);
