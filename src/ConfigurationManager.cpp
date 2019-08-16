/**********************************************************************
ESP32 COMMAND STATION

COPYRIGHT (c) 2017-2019 Mike Dunston

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  You should have received a copy of the GNU General Public License
  along with this program.  If not, see http://www.gnu.org/licenses
**********************************************************************/

#include "ESP32CommandStation.h"

#include <dirent.h>
#include <driver/sdmmc_defs.h>
#include <driver/sdmmc_host.h>
#include <driver/sdmmc_types.h>
#include <driver/sdspi_host.h>
#include <esp_spiffs.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <sys/stat.h>
#include <sys/types.h>

using nlohmann::json;

unique_ptr<ConfigurationManager> configStore;

static constexpr const char * ESP32_CS_CONFIG_JSON = "esp32cs-config.json";

// Handle for the SD card (if mounted)
sdmmc_card_t *sdcard = nullptr;

// All ESP32 Command Station configuration files live under this directory on
// the configured filesystem starting with v1.3.0.
static const char* const ESP32CS_CONFIG_DIR = CS_CONFIG_FILESYSTEM "/ESP32CS";

// Prior to v1.3.0 this was the configuration location, it is retained here only
// to support migration of data from previous releases.
static const char* const OLD_CONFIG_DIR = CS_CONFIG_FILESYSTEM "/DCCppESP32";

// Global handle for WiFi Manager
unique_ptr<Esp32WiFiManager> wifiManager;

// holder of the parsed command station configuration.
json commandStationConfig;

void recursiveWalkTree(const string &path, bool remove=false)
{
  DIR *dir = opendir(path.c_str());
  if (dir)
  {
    dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL)
    {
      string fullPath = path + "/" + ent->d_name;
      if (ent->d_type == DT_REG)
      {
        struct stat statbuf;
        stat(fullPath.c_str(), &statbuf);
        LOG(VERBOSE, "[Config] %s (%d bytes)", fullPath.c_str()
          , (int)statbuf.st_size);
        if (remove)
        {
          unlink(fullPath.c_str());
        }
      }
      else if (ent->d_type == DT_DIR)
      {
        recursiveWalkTree(fullPath, remove);
      }
    }
    closedir(dir);
    if (remove)
    {
      rmdir(path.c_str());
    }
  }
  else
  {
    LOG_ERROR("[Config] Failed to open directory: %s", path.c_str());
  }
}

ConfigurationManager::ConfigurationManager()
{
  bool initialize_default_config{true};
  esp_vfs_spiffs_conf_t conf =
  {
    .base_path = "/spiffs",
    .partition_label = NULL,
    .max_files = 5,
    .format_if_mount_failed = true
  };
  // Attempt to mount the partition
  esp_err_t res = esp_vfs_spiffs_register(&conf);
  // check that the partition mounted
  if (res != ESP_OK)
  {
    LOG(FATAL
      , "[Config] Failed to mount SPIFFS partition, err %s (%d), giving up!"
      , esp_err_to_name(res), res);
  }
  size_t total = 0, used = 0;
  if (esp_spiffs_info(NULL, &total, &used) == ESP_OK)
  {
    LOG(INFO, "[Config] SPIFFS usage: %.2f/%.2f KiB", (float)(used / 1024.0f)
      , (float)(total / 1024.0f));
  }
#if CONFIG_USE_SD
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  sdspi_slot_config_t slot_config = SDSPI_SLOT_CONFIG_DEFAULT();
  esp_vfs_fat_sdmmc_mount_config_t mount_config =
  {
    .format_if_mount_failed = true,
    .max_files = 5,
    .allocation_unit_size = 16 * 1024
  };
  ESP_ERROR_CHECK(
    esp_vfs_fat_sdmmc_mount("/sdcard",
                            &host,
                            &slot_config,
                            &mount_config,
                            &sdcard));
  FATFS *fsinfo;
  DWORD clusters;
  if (f_getfree("0:", &clusters, &fsinfo) == FR_OK)
  {
    LOG(INFO, "[Config] SD usage: %.2f/%.2f MB",
        (float)(((uint64_t)fsinfo->csize * (fsinfo->n_fatent - 2 - fsinfo->free_clst)) * fsinfo->ssize) / 1048576L,
        (float)(((uint64_t)fsinfo->csize * (fsinfo->n_fatent - 2)) * fsinfo->ssize) / 1048576L);
  }
  else
  {
    LOG(INFO, "[Config] SD capacity %.2f MB",
        (float)(((uint64_t)sdcard->csd.capacity) * sdcard->csd.sector_size) / 1048576);
  }
#endif
  if (config_cs_force_factory_reset() == CONSTANT_TRUE)
  {
    LOG(WARNING,
        "WARNING: The Factory Reset flag has been set to true, all persistent "
        "data will be cleared.");
    uint8_t countdown = 10;
    while (countdown--)
    {
      LOG(WARNING, "Factory reset will be initiated in %d seconds..."
        , countdown);
      usleep(SEC_TO_USEC(1));
    }
    LOG(WARNING, "Factory reset initiated!");
  }

  LOG(VERBOSE, "[Config] Persistent storage contents:");
  recursiveWalkTree(CS_CONFIG_FILESYSTEM
                  , config_cs_force_factory_reset() == CONSTANT_TRUE);
  mkdir(ESP32CS_CONFIG_DIR, ACCESSPERMS);

  if (exists(ESP32_CS_CONFIG_JSON))
  {
    LOG(INFO, "[Config] Found existing CS config file, attempting to load...");
    commandStationConfig = json::parse(load(ESP32_CS_CONFIG_JSON));
    if (validateLCCConfig() && validateWiFiConfig())
    {
      LOG(INFO
        , "[Config] Existing configuration successfully loaded and validated.");
      initialize_default_config = false;
    }
    else
    {
      LOG_ERROR("[Config] Existing configuration failed one (or more) "
                "validation(s)!");
    }
  }
  
  if (initialize_default_config)
  {
    LOG(INFO, "[Config] Generating default configuration...");
    commandStationConfig =
    {
      { JSON_LCC_NODE,
        {
          { JSON_NODE_ID_NODE, UINT64_C(LCC_NODE_ID) },
          { JSON_LCC_CAN_NODE,
            {
              { JSON_LCC_CAN_RX_NODE, LCC_CAN_RX_PIN },
              { JSON_LCC_CAN_TX_NODE, LCC_CAN_TX_PIN },
            }
          }
        }
      },
      { JSON_WIFI_NODE, 
        {
#if WIFI_ENABLE_SOFT_AP_ONLY
          { JSON_WIFI_MODE_NODE, JSON_VALUE_WIFI_MODE_SOFTAP_ONLY },
          { JSON_WIFI_SOFTAP_NODE,
            {
              { JSON_WIFI_SSID_NODE, wifiSSID_ },
/*
              { JSON_WIFI_PASSWORD_NODE, wifiPassword_ },
*/
            },
          },
#elif WIFI_ENABLE_SOFT_AP
          { JSON_WIFI_MODE_NODE, JSON_VALUE_WIFI_MODE_SOFTAP_STATION },
          { JSON_WIFI_SOFTAP_NODE,
            {
              { JSON_WIFI_SSID_NODE, wifiSSID_ },
            },
          },
#else
          { JSON_WIFI_MODE_NODE, JSON_VALUE_WIFI_MODE_STATION_ONLY },
#endif
#ifdef WIFI_STATIC_IP_DNS
          { JSON_WIFI_DNS_NODE, WIFI_STATIC_IP_DNS },
#endif
#if !WIFI_ENABLE_SOFT_AP_ONLY
          { JSON_WIFI_STATION_NODE, 
            {
#if defined(WIFI_STATIC_IP_ADDRESS) && defined(WIFI_STATIC_IP_GATEWAY) && defined(WIFI_STATIC_IP_SUBNET)
              { JSON_WIFI_MODE_NODE, JSON_VALUE_STATION_IP_MODE_STATIC },
              { JSON_WIFI_STATION_IP_NODE, WIFI_STATIC_IP_ADDRESS },
              { JSON_WIFI_STATION_GATEWAY_NODE, WIFI_STATIC_IP_GATEWAY },
              { JSON_WIFI_STATION_NETMASK_NODE, WIFI_STATIC_IP_SUBNET },
#else
              { JSON_WIFI_MODE_NODE, JSON_VALUE_STATION_IP_MODE_DHCP },
#endif
              { JSON_WIFI_SSID_NODE, wifiSSID_ },
              { JSON_WIFI_PASSWORD_NODE, wifiPassword_ },
            }
          },
#endif
        },
      }
    };
    store(ESP32_CS_CONFIG_JSON, commandStationConfig.dump());
  }
  LOG(VERBOSE, "[Config] %s", commandStationConfig.dump().c_str());
}

ConfigurationManager::~ConfigurationManager() {
  // Unmount the SPIFFS partition
  if (esp_spiffs_mounted(NULL))
  {
    LOG(INFO, "[Config] Unmounting SPIFFS...");
    ESP_ERROR_CHECK(esp_vfs_spiffs_unregister(NULL));
  }
  // Unmount the SD card if it was mounted
  if (sdcard)
  {
    LOG(INFO, "[Config] Unmounting SD...");
    esp_vfs_fat_sdmmc_unmount();
  }
}

void ConfigurationManager::clear()
{
  LOG(INFO, "[Config] Clearing persistent config...");
  string configRoot = ESP32CS_CONFIG_DIR;
  recursiveWalkTree(configRoot, true);
  mkdir(configRoot.c_str(), ACCESSPERMS);
}

bool ConfigurationManager::exists(const string &name)
{
  string oldConfigFilePath = getFilePath(name, true);
  string configFilePath = getFilePath(name);
  if (!access(oldConfigFilePath.c_str(), F_OK) &&
      access(configFilePath.c_str(), F_OK))
  {
    LOG(INFO, "[Config] Migrating configuration file %s to %s."
      , oldConfigFilePath.c_str()
      , configFilePath.c_str());
    rename(oldConfigFilePath.c_str(), configFilePath.c_str());
  }
  LOG(VERBOSE, "[Config] Checking for %s", configFilePath.c_str());
  return access(configFilePath.c_str(), F_OK) == 0;
}

void ConfigurationManager::remove(const string &name)
{
  string configFilePath = getFilePath(name);
  LOG(VERBOSE, "[Config] Removing %s", configFilePath.c_str());
  unlink(configFilePath.c_str());
}

string ConfigurationManager::load(const string &name)
{
  string configFilePath = getFilePath(name);
  if (!exists(name))
  {
    LOG(VERBOSE, "[Config] Failed to load: %s", configFilePath.c_str());
    return "{}";
  }
  LOG(VERBOSE, "[Config] Loading %s", configFilePath.c_str());
  return read_file_to_string(configFilePath);
}

void ConfigurationManager::store(const char *name, const string &content)
{
  string configFilePath = getFilePath(name);
  LOG(VERBOSE, "[Config] Storing %s, %d bytes", configFilePath.c_str()
    , content.length());
  write_string_to_file(configFilePath, content);
}

NodeID ConfigurationManager::getNodeId()
{
  auto lccConfig = commandStationConfig[JSON_LCC_NODE];
  return (NodeID)lccConfig[JSON_NODE_ID_NODE].get<uint64_t>();
}

void ConfigurationManager::configureLCC(OpenMRN *openmrn
                                      , const esp32cs::Esp32ConfigDef &cfg)
{
  // Create the CDI.xml dynamically if it doesn't already exist.
  openmrn->create_config_descriptor_xml(cfg, LCC_NODE_CDI_FILE);

  // Create the default internal configuration file if it doesn't already exist.
  configFd_ =
    openmrn->stack()->create_config_file_if_needed(cfg.seg().internal_config()
                                                 , ESP32CS_CDI_VERSION
                                                 , openlcb::CONFIG_FILE_SIZE);

#if CONFIG_USE_SD
  // ESP32 FFat library uses a 512b cache in memory by default for the SD VFS
  // adding a periodic fsync call for the LCC configuration file ensures that
  // config changes are saved since the LCC config file is less than 512b.
  configAutoSync_.emplace(openmrn->stack()->service(), configFd_
                        , SEC_TO_USEC(config_lcc_sd_sync_interval_sec()));
#endif // CONFIG_USE_SD

  auto lccConfig = commandStationConfig[JSON_LCC_NODE];
  if (lccConfig.contains(JSON_LCC_CAN_NODE))
  {
    auto canConfig = lccConfig[JSON_LCC_CAN_NODE];
    gpio_num_t canRXPin =
      (gpio_num_t)canConfig[JSON_LCC_CAN_RX_NODE].get<uint8_t>();
    gpio_num_t canTXPin =
      (gpio_num_t)canConfig[JSON_LCC_CAN_TX_NODE].get<uint8_t>();
    if (canRXPin < GPIO_NUM_MAX && canTXPin < GPIO_NUM_MAX)
    {
      LOG(INFO, "[Config] Enabling LCC CAN interface (rx: %d, tx: %d)"
        , canRXPin, canTXPin);
      openmrn->add_can_port(
        new Esp32HardwareCan("esp32can", canRXPin, canTXPin, false));
    }
  }
  parseWiFiConfig();

  wifiManager.reset(new Esp32WiFiManager(wifiSSID_.c_str(),
                                         wifiPassword_.c_str(),
                                         openmrn->stack(), cfg.seg().wifi(),
                                         HOSTNAME_PREFIX,
                                         wifiMode_,
                                         stationStaticIP_.get(),
                                         stationDNSServer_,
                                         WIFI_SOFT_AP_CHANNEL));
}

string ConfigurationManager::getFilePath(const string &name, bool oldPath)
{
  if (oldPath)
  {
    return StringPrintf("%s/%s", OLD_CONFIG_DIR, name.c_str());
  }
  return StringPrintf("%s/%s", ESP32CS_CONFIG_DIR, name.c_str());
}

bool ConfigurationManager::validateWiFiConfig()
{
  if (!commandStationConfig.contains(JSON_WIFI_NODE))
  {
    LOG_ERROR("[Config] WiFi configuration not found.");
    return false;
  }
  auto wifiNode = commandStationConfig[JSON_WIFI_NODE];
  LOG(VERBOSE, "[Config] WiFi config: %s", wifiNode.dump().c_str());

  // Verify that the wifi operating mode is one of the three supported
  // modes.
  string wifiMode = wifiNode[JSON_WIFI_MODE_NODE];
  if (wifiMode.compare(JSON_VALUE_WIFI_MODE_SOFTAP_ONLY) &&
      wifiMode.compare(JSON_VALUE_WIFI_MODE_SOFTAP_STATION) &&
      wifiMode.compare(JSON_VALUE_WIFI_MODE_STATION_ONLY))
  {
    LOG_ERROR("[Config] Unknown WiFi operating mode: %s!", wifiMode.c_str());
    return false;
  }

  // If we are not operating in AP only mode we should verify we have
  // an SSID.
  if (wifiMode.compare(JSON_VALUE_WIFI_MODE_SOFTAP_ONLY) &&
    (!wifiNode[JSON_WIFI_STATION_NODE].contains(JSON_WIFI_SSID_NODE) ||
     !wifiNode[JSON_WIFI_STATION_NODE].contains(JSON_WIFI_PASSWORD_NODE)))
  {
    LOG_ERROR("[Config] SSID/Password was not specified for Station mode!");
    return false;
  }

  // If we are operating in SoftAP only we require a default SSID name
  if (!wifiMode.compare(JSON_VALUE_WIFI_MODE_SOFTAP_ONLY) &&
      !wifiNode[JSON_WIFI_SOFTAP_NODE].contains(JSON_WIFI_SSID_NODE))
  {
    LOG_ERROR("[Config] SSID was not specified for SoftAP mode!");
    return false;
  }
  return true;
}

bool ConfigurationManager::validateLCCConfig()
{
  // verify LCC configuration
  if (commandStationConfig.contains(JSON_LCC_NODE))
  {
    auto lccNode = commandStationConfig[JSON_LCC_NODE];
    LOG(VERBOSE, "[Config] LCC config: %s", lccNode.dump().c_str());
    if (!lccNode.contains(JSON_NODE_ID_NODE) ||
        lccNode[JSON_NODE_ID_NODE].get<uint64_t>() == 0)
    {
      LOG_ERROR("[Config] Missing LCC node ID!");
      return false;
    }

    if (!lccNode.contains(JSON_LCC_CAN_NODE) ||
        !lccNode[JSON_LCC_CAN_NODE].contains(JSON_LCC_CAN_RX_NODE) ||
        !lccNode[JSON_LCC_CAN_NODE].contains(JSON_LCC_CAN_TX_NODE))
    {
      LOG_ERROR("[Config] LCC CAN configuration invalid.");
      return false;
    }
  }
  else
  {
    LOG_ERROR("[Config] Missing LCC configuration!");
    return false;
  }
  return true;
}

void ConfigurationManager::parseWiFiConfig()
{
  auto wifiConfig = commandStationConfig[JSON_WIFI_NODE];
  string wifiMode = wifiConfig[JSON_WIFI_MODE_NODE];
  if (!wifiMode.compare(JSON_VALUE_WIFI_MODE_SOFTAP_ONLY))
  {
    wifiMode_ =  WIFI_MODE_AP;
  }
  else if (!wifiMode.compare(JSON_VALUE_WIFI_MODE_SOFTAP_STATION))
  {
    wifiMode_ =  WIFI_MODE_APSTA;
  }
  else if (!wifiMode.compare(JSON_VALUE_WIFI_MODE_STATION_ONLY))
  {
    wifiMode_ =  WIFI_MODE_STA;
  }
  if (wifiMode_ != WIFI_MODE_AP)
  {
    auto stationConfig = wifiConfig[JSON_WIFI_STATION_NODE];
    wifiSSID_ = stationConfig[JSON_WIFI_SSID_NODE];
    wifiPassword_ = stationConfig[JSON_WIFI_PASSWORD_NODE];
    string stationMode = stationConfig[JSON_WIFI_MODE_NODE];
    if (!stationMode.compare(JSON_VALUE_STATION_IP_MODE_STATIC))
    {
      stationStaticIP_.reset(new tcpip_adapter_ip_info_t());
      string value = stationConfig[JSON_WIFI_STATION_IP_NODE];
      stationStaticIP_->ip.addr = ipaddr_addr(value.c_str());
      value = stationConfig[JSON_WIFI_STATION_GATEWAY_NODE];
      stationStaticIP_->gw.addr = ipaddr_addr(value.c_str());
      value = stationConfig[JSON_WIFI_STATION_NETMASK_NODE];
      stationStaticIP_->netmask.addr = ipaddr_addr(value.c_str());
    }
  }
  else
  {
    wifiSSID_ = wifiConfig[JSON_WIFI_SOFTAP_NODE][JSON_WIFI_SSID_NODE];  
  }
  if (wifiConfig.contains(JSON_WIFI_DNS_NODE))
  {
    string value = wifiConfig[JSON_WIFI_DNS_NODE];
    stationDNSServer_.u_addr.ip4.addr = ipaddr_addr(value.c_str());
  }
}

void ConfigurationManager::configureEnabledModules(SimpleCanStack *stack)
{
  if (config_cs_hc12_enabled() == CONSTANT_TRUE)
  {
    hc12_.emplace(stack->service(), (uart_port_t)config_cs_hc12_uart_num());
  }
  ota_.emplace(stack->service());
  infoScreen_.emplace(stack);
  statusLED_.emplace(stack->service());
  
  // Task Monitor, periodically dumps runtime state to STDOUT.
  taskMon_.emplace(stack->service());
#if ENABLE_OUTPUTS
  OutputManager::init();
#endif

#if ENABLE_SENSORS
  SensorManager::init();
  S88BusManager::init();
  RemoteSensorManager::init();
#endif

#if LOCONET_ENABLED
  initializeLocoNet();
#endif
}

string ConfigurationManager::getCSConfig()
{
  return commandStationConfig.dump();
}

string ConfigurationManager::getCSFeatures()
{
  json features = 
  {
    { JSON_S88_SENSOR_BASE_NODE, S88_FIRST_SENSOR }
  , { JSON_S88_NODE, S88_ENABLED && ENABLE_SENSORS ? JSON_VALUE_TRUE
                                                   : JSON_VALUE_FALSE }
  , { JSON_OUTPUTS_NODE, ENABLE_OUTPUTS ? JSON_VALUE_TRUE : JSON_VALUE_FALSE }
  , { JSON_SENSORS_NODE, ENABLE_SENSORS ? JSON_VALUE_TRUE : JSON_VALUE_FALSE }
  , { JSON_HC12_NODE
    , config_cs_hc12_enabled() == CONSTANT_TRUE ? JSON_VALUE_TRUE
                                                : JSON_VALUE_FALSE }
  };
  return features.dump();
}