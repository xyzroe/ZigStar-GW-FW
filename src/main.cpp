#include <WiFi.h>

#include <WiFiClient.h>
#include <WebServer.h>
#include <SoftwareSerial.h>
#include <ArduinoJson.h>
#include "LITTLEFS.h"
#include "config.h"
#include "web.h"
#include "log.h"
#include "etc.h"
#include <Update.h>
#include "version.h"

#include <driver/uart.h>
#include <lwip/ip_addr.h>

#include <ETH.h>
#ifdef ETH_CLK_MODE
#undef ETH_CLK_MODE
#endif

#ifdef BONJOUR_SUPPORT
#include <ESPmDNS.h>
#endif

#include "mqtt.h"

// application config
unsigned long timeLog;
ConfigSettingsStruct ConfigSettings;
InfosStruct Infos;
bool configOK = false;
String modeWiFi = "STA";

// serial end ethernet buffer size
#define BUFFER_SIZE 256

#ifdef BONJOUR_SUPPORT
// multicast DNS responder
MDNSResponder mdns;
#endif

void saveEmergencyWifi(bool state)
{
  const char *path = "/config/system.json";
  DynamicJsonDocument doc(1024);

  File configFile = LITTLEFS.open(path, FILE_READ);
  deserializeJson(doc, configFile);
  configFile.close();

  doc["emergencyWifi"] = int(state);

  configFile = LITTLEFS.open(path, FILE_WRITE);
  serializeJson(doc, configFile);
  configFile.close();
}

void saveBoard(int rev)
{
  const char *path = "/config/system.json";
  DynamicJsonDocument doc(1024);

  File configFile = LITTLEFS.open(path, FILE_READ);
  deserializeJson(doc, configFile);
  configFile.close();

  doc["board"] = int(rev);

  configFile = LITTLEFS.open(path, FILE_WRITE);
  serializeJson(doc, configFile);
  configFile.close();
}

void WiFiEvent(WiFiEvent_t event)
{
  switch (event)
  {
  case SYSTEM_EVENT_ETH_START:
    DEBUG_PRINTLN(F("ETH Started"));

    break;
  case SYSTEM_EVENT_ETH_CONNECTED:
    DEBUG_PRINTLN(F("ETH Connected"));
    ConfigSettings.connectedEther = true;
    ConfigSettings.disconnectEthTime = 0;
    if (ConfigSettings.emergencyWifi && !ConfigSettings.enableWiFi)
    {
      saveEmergencyWifi(0);
      WiFi.disconnect();
      WiFi.mode(WIFI_OFF);
      ConfigSettings.emergencyWifi = 0;
      DEBUG_PRINTLN(F("saveEmergencyWifi 0"));
    }
    break;
  case SYSTEM_EVENT_ETH_GOT_IP:
    DEBUG_PRINTLN(F("ETH MAC: "));
    DEBUG_PRINT(ETH.macAddress());
    DEBUG_PRINT(F(", IPv4: "));
    DEBUG_PRINT(ETH.localIP());
    if (ETH.fullDuplex())
    {
      DEBUG_PRINT(F(", FULL_DUPLEX"));
    }
    DEBUG_PRINT(F(", "));
    DEBUG_PRINT(ETH.linkSpeed());
    DEBUG_PRINTLN(F("Mbps"));
    ConfigSettings.connectedEther = true;
    break;
  case SYSTEM_EVENT_ETH_DISCONNECTED:
    DEBUG_PRINTLN(F("ETH Disconnected"));
    ConfigSettings.connectedEther = false;
    ConfigSettings.disconnectEthTime = millis();
    break;
  case SYSTEM_EVENT_ETH_STOP:
    DEBUG_PRINTLN(F("ETH Stopped"));
    ConfigSettings.connectedEther = false;
    ConfigSettings.disconnectEthTime = millis();
    break;
  default:
    break;
  }
}

WiFiServer server(TCP_LISTEN_PORT, MAX_SOCKET_CLIENTS);

IPAddress parse_ip_address(const char *str)
{
  IPAddress result;
  int index = 0;

  result[0] = 0;
  while (*str)
  {
    if (isdigit((unsigned char)*str))
    {
      result[index] *= 10;
      result[index] += *str - '0';
    }
    else
    {
      index++;
      if (index < 4)
      {
        result[index] = 0;
      }
    }
    str++;
  }

  return result;
}

bool loadSystemVar()
{
  const char *path = "/config/system.json";

  File configFile = LITTLEFS.open(path, FILE_READ);
  if (!configFile)
  {
    DEBUG_PRINTLN(F("failed open. try to write defaults"));

    String CPUtemp;
    getBlankCPUtemp(CPUtemp);
    int correct = CPUtemp.toInt() - 30;
    String tempOffset =  String(correct);

    String StringConfig = "{\"board\":1,\"emergencyWifi\":0,\"tempOffset\":" + tempOffset + "}";
    DEBUG_PRINTLN(StringConfig);
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, StringConfig);

    File configFile = LITTLEFS.open(path, FILE_WRITE);
    if (!configFile)
    {
      DEBUG_PRINTLN(F("failed write"));
      return false;
    }
    else
    {
      serializeJson(doc, configFile);
    }
    return false;
  }

  DynamicJsonDocument doc(1024);
  deserializeJson(doc, configFile);

  ConfigSettings.board = (int)doc["board"];
  ConfigSettings.emergencyWifi = (int)doc["emergencyWifi"];
  ConfigSettings.tempOffset = (int)doc["tempOffset"];
  if (!ConfigSettings.tempOffset){
    DEBUG_PRINTLN(F("no tempOffset in system.json"));
    configFile.close();

    String CPUtemp;
    getBlankCPUtemp(CPUtemp);
    int correct = CPUtemp.toInt() - 30;
    String tempOffset =  String(correct);
    doc["tempOffset"] = int(tempOffset.toInt());

    configFile = LITTLEFS.open(path, FILE_WRITE);
    serializeJson(doc, configFile);
    configFile.close();
    DEBUG_PRINTLN(F("saved tempOffset in system.json"));
    ConfigSettings.tempOffset = int(tempOffset.toInt());
  }
  configFile.close();
  return true;
}

bool loadConfigWifi()
{
  const char *path = "/config/configWifi.json";

  File configFile = LITTLEFS.open(path, FILE_READ);
  if (!configFile)
  {
    String StringConfig = "{\"enableWiFi\":0,\"ssid\":\"\",\"pass\":\"\",\"dhcpWiFi\":1,\"ip\":\"\",\"mask\":\"\",\"gw\":\"\"}";

    writeDefultConfig(path, StringConfig);
  }

  configFile = LITTLEFS.open(path, FILE_READ);
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, configFile);

  if (error)
  {
    DEBUG_PRINTLN(F("deserializeJson() failed: "));
    DEBUG_PRINTLN(error.f_str());

    configFile.close();
    LITTLEFS.remove(path);
    return false;
  }

  ConfigSettings.dhcpWiFi = (int)doc["dhcpWiFi"];
  strlcpy(ConfigSettings.ssid, doc["ssid"] | "", sizeof(ConfigSettings.ssid));
  strlcpy(ConfigSettings.password, doc["pass"] | "", sizeof(ConfigSettings.password));
  strlcpy(ConfigSettings.ipAddressWiFi, doc["ip"] | "", sizeof(ConfigSettings.ipAddressWiFi));
  strlcpy(ConfigSettings.ipMaskWiFi, doc["mask"] | "", sizeof(ConfigSettings.ipMaskWiFi));
  strlcpy(ConfigSettings.ipGWWiFi, doc["gw"] | "", sizeof(ConfigSettings.ipGWWiFi));
  ConfigSettings.enableWiFi = (int)doc["enableWiFi"];

  configFile.close();
  return true;
}

bool loadConfigEther()
{
  const char *path = "/config/configEther.json";

  File configFile = LITTLEFS.open(path, FILE_READ);
  if (!configFile)
  {
    String StringConfig = "{\"dhcp\":1,\"ip\":\"\",\"mask\":\"\",\"gw\":\"\"}";

    writeDefultConfig(path, StringConfig);
  }

  configFile = LITTLEFS.open(path, FILE_READ);
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, configFile);

  if (error)
  {
    DEBUG_PRINTLN(F("deserializeJson() failed: "));
    DEBUG_PRINTLN(error.f_str());

    configFile.close();
    LITTLEFS.remove(path);
    return false;
  }

  ConfigSettings.dhcp = (int)doc["dhcp"];
  strlcpy(ConfigSettings.ipAddress, doc["ip"] | "", sizeof(ConfigSettings.ipAddress));
  strlcpy(ConfigSettings.ipMask, doc["mask"] | "", sizeof(ConfigSettings.ipMask));
  strlcpy(ConfigSettings.ipGW, doc["gw"] | "", sizeof(ConfigSettings.ipGW));
  configFile.close();
  return true;
}

bool loadConfigGeneral()
{
  const char *path = "/config/configGeneral.json";

  File configFile = LITTLEFS.open(path, FILE_READ);
  if (!configFile)
  {
    String deviceID = "ZigStarGW";
    //getDeviceID(deviceID);
    String StringConfig = "{\"hostname\":\"" + deviceID + "\",\"disableWeb\":0,\"refreshLogs\":1000}";

    writeDefultConfig(path, StringConfig);
  }

  configFile = LITTLEFS.open(path, FILE_READ);
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, configFile);

  if (error)
  {
    DEBUG_PRINTLN(F("deserializeJson() failed: "));
    DEBUG_PRINTLN(error.f_str());

    configFile.close();
    LITTLEFS.remove(path);
    return false;
  }

  ConfigSettings.disableWeb = (int)doc["disableWeb"];
  if ((double)doc["refreshLogs"] < 1000)
  {
    ConfigSettings.refreshLogs = 1000;
  }
  else
  {
    ConfigSettings.refreshLogs = (double)doc["refreshLogs"];
  }
  strlcpy(ConfigSettings.hostname, doc["hostname"] | "", sizeof(ConfigSettings.hostname));
  configFile.close();
  return true;
}

bool loadConfigSerial()
{
  const char *path = "/config/configSerial.json";

  File configFile = LITTLEFS.open(path, FILE_READ);
  if (!configFile)
  {
    String StringConfig = "{\"baud\":115200,\"port\":6638}";

    writeDefultConfig(path, StringConfig);
  }

  configFile = LITTLEFS.open(path, FILE_READ);
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, configFile);

  if (error)
  {
    DEBUG_PRINTLN(F("deserializeJson() failed: "));
    DEBUG_PRINTLN(error.f_str());

    configFile.close();
    LITTLEFS.remove(path);
    return false;
  }

  ConfigSettings.serialSpeed = (int)doc["baud"];
  ConfigSettings.socketPort = (int)doc["port"];
  if (ConfigSettings.socketPort == 0)
  {
    ConfigSettings.socketPort = TCP_LISTEN_PORT;
  }
  configFile.close();
  return true;
}

bool loadConfigMqtt()
{
  const char *path = "/config/configMqtt.json";

  File configFile = LITTLEFS.open(path, FILE_READ);
  if (!configFile)
  {
    String deviceID;
    getDeviceID(deviceID);
    String StringConfig = "{\"enable\":0,\"server\":\"\",\"port\":1883,\"user\":\"mqttuser\",\"pass\":\"\",\"topic\":\"" + deviceID + "\",\"interval\":60,\"discovery\":0}";

    writeDefultConfig(path, StringConfig);
  }

  configFile = LITTLEFS.open(path, FILE_READ);
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, configFile);

  if (error)
  {
    DEBUG_PRINTLN(F("deserializeJson() failed: "));
    DEBUG_PRINTLN(error.f_str());

    configFile.close();
    LITTLEFS.remove(path);
    return false;
  }

  ConfigSettings.mqttEnable = (int)doc["enable"];
  strlcpy(ConfigSettings.mqttServer, doc["server"] | "", sizeof(ConfigSettings.mqttServer));
  ConfigSettings.mqttServerIP = parse_ip_address(ConfigSettings.mqttServer);
  ConfigSettings.mqttPort = (int)doc["port"];
  strlcpy(ConfigSettings.mqttUser, doc["user"] | "", sizeof(ConfigSettings.mqttUser));
  strlcpy(ConfigSettings.mqttPass, doc["pass"] | "", sizeof(ConfigSettings.mqttPass));
  strlcpy(ConfigSettings.mqttTopic, doc["topic"] | "", sizeof(ConfigSettings.mqttTopic));
  ConfigSettings.mqttInterval = (int)doc["interval"];
  ConfigSettings.mqttDiscovery = (int)doc["discovery"];

  configFile.close();
  return true;
}

void setupWifiAP()
{
  WiFi.mode(WIFI_AP);
  WiFi.disconnect();

  String AP_NameString;
  getDeviceID(AP_NameString);

  char AP_NameChar[AP_NameString.length() + 1];
  memset(AP_NameChar, 0, AP_NameString.length() + 1);

  for (int i = 0; i < AP_NameString.length(); i++)
    AP_NameChar[i] = AP_NameString.charAt(i);
/*
  String WIFIPASSSTR = "ZigStar1";
  char WIFIPASS[WIFIPASSSTR.length() + 1];
  memset(WIFIPASS, 0, WIFIPASSSTR.length() + 1);
  for (int i = 0; i < WIFIPASSSTR.length(); i++)
    WIFIPASS[i] = WIFIPASSSTR.charAt(i);
*/
  WiFi.softAP(AP_NameChar); //, WIFIPASS);
  WiFi.setSleep(false);
}

bool setupSTAWifi()
{

  WiFi.mode(WIFI_STA);
  DEBUG_PRINTLN(F("WiFi.mode(WIFI_STA)"));
  WiFi.disconnect();
  DEBUG_PRINTLN(F("disconnect"));
  delay(100);

  WiFi.begin(ConfigSettings.ssid, ConfigSettings.password);
  WiFi.setSleep(false);
  DEBUG_PRINTLN(F("WiFi.begin"));

  IPAddress ip_address = parse_ip_address(ConfigSettings.ipAddressWiFi);
  IPAddress gateway_address = parse_ip_address(ConfigSettings.ipGWWiFi);
  IPAddress netmask = parse_ip_address(ConfigSettings.ipMaskWiFi);

  if (!ConfigSettings.dhcpWiFi)
  {
    WiFi.config(ip_address, gateway_address, netmask);
    DEBUG_PRINTLN(F("WiFi.config"));
  }
  else
  {
    DEBUG_PRINTLN(F("Try DHCP"));
  }

  int countDelay = 50;
  while (WiFi.status() != WL_CONNECTED)
  {
    //DEBUG_PRINT(F("."));
    DEBUG_PRINT(WiFi.status());
    countDelay--;
    if (countDelay == 0)
    {
      return false;
    }
    delay(250);
  }
  DEBUG_PRINTLN(F(" "));
  DEBUG_PRINTLN(WiFi.localIP());
  DEBUG_PRINTLN(WiFi.subnetMask());
  DEBUG_PRINTLN(WiFi.gatewayIP());
  return true;
}

void enableWifi()
{
  WiFi.setHostname(ConfigSettings.hostname);
  if ((strlen(ConfigSettings.ssid) != 0) && (strlen(ConfigSettings.password) != 0))
  {
    DEBUG_PRINTLN(F("Ok SSID & PASS"));
    if (!setupSTAWifi())
    {
      setupWifiAP();
      modeWiFi = "AP";
      ConfigSettings.radioModeWiFi = true;
      DEBUG_PRINTLN(F("AP"));
    }
    else
    {
      DEBUG_PRINTLN(F("setupSTAWifi"));
      ConfigSettings.radioModeWiFi = false;
    }
  }
  else
  {
    DEBUG_PRINTLN(F("Error SSID & PASS"));
    setupWifiAP();
    modeWiFi = "AP";
    DEBUG_PRINTLN(F("AP"));
    ConfigSettings.radioModeWiFi = true;
  }
}

void setupEthernetAndZigbeeSerial()
{
  switch (ConfigSettings.board)
  {

  case 1:
    if (ETH.begin(ETH_ADDR_1, ETH_POWER_PIN_1, ETH_MDC_PIN_1, ETH_MDIO_PIN_1, ETH_TYPE_1, ETH_CLK_MODE_1))
    {
      String boardName = "WT32-ETH01";
      boardName.toCharArray(ConfigSettings.boardName, sizeof(ConfigSettings.boardName));
      DEBUG_PRINT(F("Board - "));
      DEBUG_PRINTLN(boardName);
      ConfigSettings.rstZigbeePin = RESTART_ZIGBEE_1;
      ConfigSettings.flashZigbeePin = FLASH_ZIGBEE_1;

      DEBUG_PRINT(F("Zigbee serial setup @ "));
      DEBUG_PRINTLN(ConfigSettings.serialSpeed);
      Serial2.begin(ConfigSettings.serialSpeed, SERIAL_8N1, ZRXD_1, ZTXD_1);
    }
    else
    {
      saveBoard(2);
      ESP.restart();
    }
    break;

  case 2:
    if (ETH.begin(ETH_ADDR_2, ETH_POWER_PIN_2, ETH_MDC_PIN_2, ETH_MDIO_PIN_2, ETH_TYPE_2, ETH_CLK_MODE_2))
    {
      String boardName = "TTGO T-Internet-POE";
      boardName.toCharArray(ConfigSettings.boardName, sizeof(ConfigSettings.boardName));
      DEBUG_PRINT(F("Board - "));
      DEBUG_PRINTLN(boardName);
      ConfigSettings.rstZigbeePin = RESTART_ZIGBEE_2;
      ConfigSettings.flashZigbeePin = FLASH_ZIGBEE_2;

      DEBUG_PRINT(F("Zigbee serial setup @ "));
      DEBUG_PRINTLN(ConfigSettings.serialSpeed);
      Serial2.begin(ConfigSettings.serialSpeed, SERIAL_8N1, ZRXD_2, ZTXD_2);
    }
    else
    {
      saveBoard(3);
      ESP.restart();
    }
    break;

  case 3:
    if (ETH.begin(ETH_ADDR_3, ETH_POWER_PIN_3, ETH_MDC_PIN_3, ETH_MDIO_PIN_3, ETH_TYPE_3, ETH_CLK_MODE_3))
    {
      String boardName = "unofficial China-GW";
      boardName.toCharArray(ConfigSettings.boardName, sizeof(ConfigSettings.boardName));
      DEBUG_PRINT(F("Board - "));
      DEBUG_PRINTLN(boardName);
      ConfigSettings.rstZigbeePin = RESTART_ZIGBEE_3;
      ConfigSettings.flashZigbeePin = FLASH_ZIGBEE_3;

      DEBUG_PRINT(F("Zigbee serial setup @ "));
      DEBUG_PRINTLN(ConfigSettings.serialSpeed);
      Serial2.begin(ConfigSettings.serialSpeed, SERIAL_8N1, ZRXD_3, ZTXD_3);
    }
    else
    {
      saveBoard(0);
      ESP.restart();
    }
    break;

  default:
    String boardName = "Unknown";
    if (!ETH.begin(ETH_ADDR_1, ETH_POWER_PIN_1, ETH_MDC_PIN_1, ETH_MDIO_PIN_1, ETH_TYPE_1, ETH_CLK_MODE_1))
    {
      ConfigSettings.emergencyWifi = 1;
      DEBUG_PRINTLN(F("Please set board type in system.json"));
      saveBoard(0);
    }
    else
    {
      saveBoard(1);
      boardName = "WT32-ETH01";
    }
    boardName.toCharArray(ConfigSettings.boardName, sizeof(ConfigSettings.boardName));
    DEBUG_PRINT(F("Board - "));
    DEBUG_PRINTLN(boardName);
    ConfigSettings.rstZigbeePin = RESTART_ZIGBEE_1;
    ConfigSettings.flashZigbeePin = FLASH_ZIGBEE_1;

    DEBUG_PRINT(F("Zigbee serial setup @ "));
    DEBUG_PRINTLN(ConfigSettings.serialSpeed);
    Serial2.begin(ConfigSettings.serialSpeed, SERIAL_8N1, ZRXD_1, ZTXD_1);
    break;
  }
}

void setup(void)
{

  Serial.begin(115200);
  DEBUG_PRINTLN(F("Start"));

  WiFi.onEvent(WiFiEvent);

  if (!LITTLEFS.begin(FORMAT_LITTLEFS_IF_FAILED, "/lfs2", 10))
  {
    DEBUG_PRINTLN(F("Error with LITTLEFS"));
    return;
  }

  DEBUG_PRINTLN(F("LITTLEFS OK"));
  if (!loadSystemVar())
  {
    DEBUG_PRINTLN(F("Error load system vars"));
    const char *path = "/config";

    if (LITTLEFS.mkdir(path))
    {
      DEBUG_PRINTLN(F("Config dir created"));
      delay(500);
      ESP.restart();
    }
    else
    {
      DEBUG_PRINTLN(F("mkdir failed"));
    }
  }
  else
  {
    DEBUG_PRINTLN(F("System vars load OK"));
  }

  if (!loadConfigSerial())
  {
    DEBUG_PRINTLN(F("Error load config serial"));
    ESP.restart();
  }
  else
  {
    DEBUG_PRINTLN(F("Config serial load OK"));
  }

  setupEthernetAndZigbeeSerial();

  if ((!loadConfigWifi()) || (!loadConfigEther()) || (!loadConfigGeneral()) || (!loadConfigMqtt()))
  {
    DEBUG_PRINTLN(F("Error load config files"));
    ESP.restart();
  }
  else
  {
    configOK = true;
    DEBUG_PRINTLN(F("Config files load OK"));
  }

  /*
  String boardName;
  switch (ConfigSettings.board)
  {
  case 0:
    boardName = "Unknown";
    break;
  case 1:
    boardName = "WT32-ETH01";
    break;
  case 2:
    boardName = "TTGO T-Internet-POE";
    break;
  }
  boardName.toCharArray(ConfigSettings.boardName, sizeof(ConfigSettings.boardName));
*/

  pinMode(ConfigSettings.rstZigbeePin, OUTPUT);
  pinMode(ConfigSettings.flashZigbeePin, OUTPUT);
  digitalWrite(ConfigSettings.rstZigbeePin, 1);
  digitalWrite(ConfigSettings.flashZigbeePin, 1);

  ConfigSettings.disconnectEthTime = millis();
  ETH.setHostname(ConfigSettings.hostname);

  if (!ConfigSettings.dhcp)
  {
    DEBUG_PRINTLN(F("ETH STATIC"));
    ETH.config(parse_ip_address(ConfigSettings.ipAddress), parse_ip_address(ConfigSettings.ipGW), parse_ip_address(ConfigSettings.ipMask));
  }
  else
  {
    DEBUG_PRINTLN(F("ETH DHCP"));
  }

  initWebServer();

#ifdef BONJOUR_SUPPORT
  if (!MDNS.begin(ConfigSettings.hostname))
  {
    DEBUG_PRINTLN(F("Error setting up MDNS responder!"));
    while (1)
    {
      delay(1000);
    }
  }
  DEBUG_PRINTLN(F("mDNS responder started"));
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("zig_star_gw", "tcp", ConfigSettings.socketPort);
  MDNS.addServiceTxt("zig_star_gw", "tcp", "version", "1.0");
  MDNS.addServiceTxt("zig_star_gw", "tcp", "radio_type", "znp");
  MDNS.addServiceTxt("zig_star_gw", "tcp", "baud_rate", String(ConfigSettings.serialSpeed));
  MDNS.addServiceTxt("zig_star_gw", "tcp", "data_flow_control", "software");

#endif

  if (ConfigSettings.enableWiFi || ConfigSettings.emergencyWifi)
  {
    enableWifi();
  }
  server.begin(ConfigSettings.socketPort);

  if (ConfigSettings.mqttEnable)
  {
    mqttConnectSetup();
  }

  ConfigSettings.connectedClients = 0;
}

WiFiClient client[10];
//double loopCount;

void socketClientConnected(int client)
{
  if (ConfigSettings.connectedSocket[client] != true)
  {
    DEBUG_PRINT("Connected client ");
    DEBUG_PRINTLN(client);
    if (ConfigSettings.connectedClients == 0)
    {
      ConfigSettings.socketTime = millis();
      DEBUG_PRINT("Socket time ");
      DEBUG_PRINTLN(ConfigSettings.socketTime);
      mqttPublishIo("socket", "ON");
    }
    ConfigSettings.connectedSocket[client] = true;
    ConfigSettings.connectedClients++;
  }
}

void socketClientDisconnected(int client)
{
  if (ConfigSettings.connectedSocket[client] != false)
  {
    DEBUG_PRINT("Disconnected client ");
    DEBUG_PRINTLN(client);
    ConfigSettings.connectedSocket[client] = false;
    ConfigSettings.connectedClients--;
    if (ConfigSettings.connectedClients == 0)
    {
      ConfigSettings.socketTime = millis();
      DEBUG_PRINT("Socket time ");
      DEBUG_PRINTLN(ConfigSettings.socketTime);
      mqttPublishIo("socket", "OFF");
    }
  }
}

void printRecvSocket(size_t bytes_read, uint8_t net_buf[BUFFER_SIZE])
{
  char output_sprintf[2];
  if (bytes_read > 0)
  {
    String tmpTime;
    String buff = "";
    timeLog = millis();
    tmpTime = String(timeLog, DEC);
    logPush('[');
    for (int j = 0; j < tmpTime.length(); j++)
    {
      logPush(tmpTime[j]);
    }
    logPush(']');
    logPush('-');
    logPush('>');

    for (int i = 0; i < bytes_read; i++)
    {
      sprintf(output_sprintf, "%02x", net_buf[i]);
      logPush(' ');
      logPush(output_sprintf[0]);
      logPush(output_sprintf[1]);
    }
    logPush('\n');
  }
}

void printSendSocket(size_t bytes_read, uint8_t serial_buf[BUFFER_SIZE])
{
  char output_sprintf[2];
  for (int i = 0; i < bytes_read; i++)
  {
    sprintf(output_sprintf, "%02x", serial_buf[i]);
    if (serial_buf[i] == 0x01)
    {

      String tmpTime;
      String buff = "";
      timeLog = millis();
      tmpTime = String(timeLog, DEC);
      logPush('[');
      for (int j = 0; j < tmpTime.length(); j++)
      {
        logPush(tmpTime[j]);
      }
      logPush(']');
      logPush('<');
      logPush('-');
    }
    logPush(' ');

    logPush(output_sprintf[0]);
    logPush(output_sprintf[1]);
    if (serial_buf[i] == 0x03)
    {
      logPush('\n');
    }
  }
}

void loop(void)
{
  size_t bytes_read;
  uint8_t net_buf[BUFFER_SIZE];
  uint8_t serial_buf[BUFFER_SIZE];

  if (!ConfigSettings.disableWeb)
  {
    webServerHandleClient();
  }
  else
  {
    if (ConfigSettings.connectedClients == 0)
    {
      webServerHandleClient();
    }
  }

  if (ConfigSettings.connectedEther == false && ConfigSettings.disconnectEthTime != 0 && ConfigSettings.enableWiFi != 1 && ConfigSettings.emergencyWifi != 1)
  {
    if ((millis() - ConfigSettings.disconnectEthTime) >= (ETH_ERROR_TIME * 1000))
    {
      DEBUG_PRINTLN(F("saveEmergencyWifi(1)"));
      saveEmergencyWifi(1);
      DEBUG_PRINTLN(F("ESP.restart"));
      ESP.restart();
    }
  }

  // Check if there is no clients
  if (ConfigSettings.connectedClients == 0)
  {
    // eat any bytes in the buffer as there is noone to see them
    while (Serial2.available())
    {
      Serial2.read();
    }
  }

  // look for clients
  for (int i = 0; i < MAX_SOCKET_CLIENTS; i++)
  {
    if (!client[i])
    {
      client[i] = server.available();
    }
  }


  // work with client, read from the network and write to UART
  for (int i = 0; i < MAX_SOCKET_CLIENTS; i++)
  {
    if (client[i].connected())
    {
      socketClientConnected(i);
      int count = client[i].available();
      if (count > 0)
      {
        bytes_read = client[i].read(net_buf, min(count, BUFFER_SIZE));
        Serial2.write(net_buf, bytes_read);
        Serial2.flush();
        printRecvSocket(bytes_read, net_buf); //print rx to web console
      }
    }
    else
    {
      client[i].stop();
      socketClientDisconnected(i);
    }
  }


  // now check UART for any bytes to send to the network
  bytes_read = 0;
  bool buffOK = false;

  while (Serial2.available() && bytes_read < BUFFER_SIZE)
  {
    buffOK = true;
    serial_buf[bytes_read] = Serial2.read();
    bytes_read++;
  }

  // now send all bytes from UART to the network
  if (bytes_read > 0)
  {
    for (int i = 0; i < MAX_SOCKET_CLIENTS; i++)
    {
      if (client[i].connected())
      {
        client[i].write((const uint8_t *)serial_buf, bytes_read);
        client[i].flush();
      }
    }
  }

  //print tx to web console
  if (buffOK)
  {
    printSendSocket(bytes_read, serial_buf);
    buffOK = false;
  }

  if (ConfigSettings.mqttEnable)
  {
    mqttLoop();
  }
}