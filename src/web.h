void initWebServer();
void webServerHandleClient();
void handleGeneral();
void handleRoot();
void handleWifi();
void handleEther();
void handleSerial();
void handleSaveSerial();
void handleNotFound();
void handleSaveWifi();
void handleSaveEther();
void handleLogs();
void handleReboot();
void handleUpdate();
void handleFSbrowser();
void handleReadfile();
void handleSavefile();
void handleLogBuffer();
void handleScanNetwork();
void handleClearConsole();
void handleGetVersion();
void handleZigbeeReset();
void handleZigbeeBSL();
void handleSaveGeneral();
void handleHelp();
void handleESPUpdate();
void printLogTime();
void printLogMsg(String msg);
void handleSaveSucces(String msg);