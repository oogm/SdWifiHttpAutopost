#include <Arduino_JSON.h>
#include <SdFat.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>

// LED is connected to GPIO2 on this board
#define INIT_LED			{pinMode(2, OUTPUT);}
#define LED_ON				{digitalWrite(2, LOW);}
#define LED_OFF				{digitalWrite(2, HIGH);}

// SD Parameters
#define SPI_BLOCKOUT_PERIOD	2000UL
#define SD_CS		4
#define MISO_PIN		12
#define MOSI_PIN		13
#define SCLK_PIN		14
#define CS_SENSE	5

// Setup status codes
#define SETUP_BOOT 1
#define SETUP_INIT_SD 2
#define SETUP_LOAD_CONFIG 3
#define SETUP_CONNECT_WIFI 4
#define SETUP_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 5
#define ERROR_ENDPOINT 6

SdFat sdfat;
JSONVar configJson;
int status = SETUP_BOOT;
int remainingPosts = 0;
String endpointProtocol;
String endpointHost;
BearSSL::Session session;
volatile bool postedSinceBusTakenByOtherHost = true; // Wait for other bus before posting
volatile long _spiBlockoutTime = 0;
bool _weTookBus = false;

// ------------------------
void setup() {
	Serial.begin(115200);
	INIT_LED;
  Serial.println("");
	Serial.println("Boot complete");
	
  // Setup SD and  until bus becomes free for the first time to get config
  Serial.println("Loading config");
  sdSetup();
  LED_ON;
  while(!canWeTakeBus()) {
    Serial.print(".");
    delay(100);
  }
  takeBusControl();
  LED_OFF;

  // The following steps have no recovery / retry's don't make sense.
  // We'll just exit and communicate the status/error via LED.

  // Initialize SD
  status = SETUP_INIT_SD;
  if(!sdfat.begin(SD_CS, SPI_FULL_SPEED)) {
    Serial.println("Initial access to SD failed");
    relinquishBusControl();
    return;
  }

  // Load config
  status = SETUP_LOAD_CONFIG;
  File32 configFile;
  if (!configFile.open("espconfig.json", FILE_READ)) {
    Serial.println("Open espconfig.json file failed");
    relinquishBusControl();
    return;
  }
  String configContent = configFile.readString();
  configFile.close();
  relinquishBusControl();

  // Parse config & validate fields
  configJson = JSON.parse(configContent);
  if (JSON.typeof(configJson) == "undefined") {
    Serial.println("Parsing espconfig.json failed!");
    return;
  }
  if (!configJson.hasOwnProperty("wifiSsid")) {
    Serial.println("espconfig.json is missing property wifiSsid.");
    return;
  }
  if (!configJson.hasOwnProperty("wifiPassword")) {
    Serial.println("espconfig.json is missing property wifiPassword.");
    return;
  }
  if (!configJson.hasOwnProperty("filePath")) {
    Serial.println("espconfig.json is missing property filePath.");
    return;
  }
  if (!configJson.hasOwnProperty("fileTailBytes")) {
    Serial.println("espconfig.json is missing property fileTailBytes.");
    return;
  }
  if (!configJson.hasOwnProperty("endpointUrl")) {
    Serial.println("espconfig.json is missing property endpointUrl.");
    return;
  }
  if (!configJson.hasOwnProperty("endpointAuthorization")) {
    Serial.println("espconfig.json is missing property endpointAuthorization.");
    return;
  }
  if (!configJson.hasOwnProperty("postIntervalSeconds")) {
    Serial.println("espconfig.json is missing property postIntervalSeconds.");
    return;
  }
  if (!configJson.hasOwnProperty("postNumberBeforeShutdown")) {
    Serial.println("espconfig.json is missing property postNumberBeforeShutdown.");
    return;
  }
  remainingPosts = configJson["postNumberBeforeShutdown"];
  // Extract endpoint host
  String endpointUrl = configJson["endpointUrl"];
  int startIndex = endpointUrl.indexOf("://") + 3;
  if (startIndex == -1) { return; }
  int endIndex = endpointUrl.indexOf("/", startIndex);
  if (endIndex == -1) {return;}
  endpointProtocol = endpointUrl.substring(0, startIndex - 3);
  endpointHost = endpointUrl.substring(startIndex, endIndex);

  // Connect to Wifi
  status = SETUP_CONNECT_WIFI;
  Serial.print("Connecting to Wifi");
  WiFi.hostname("SdWifiEsp");
  WiFi.setAutoConnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.setPhyMode(WIFI_PHY_MODE_11N);
  String wifiSsid = configJson["wifiSsid"];
  String wifiPassword = configJson["wifiPassword"];
  WiFi.begin(wifiSsid, wifiPassword);

  // Wait for connection
  unsigned int timeout = 0;
  while(WiFi.status() != WL_CONNECTED) {
    LED_ON;
		delay(100);
    LED_OFF;
    delay(100);
		Serial.print(".");
	}

  Serial.println("");
	Serial.print("Connected to "); Serial.println(configJson["wifiSsid"]);
	Serial.print ("IP address: "); Serial.println(WiFi.localIP());
	Serial.print ("RSSI: "); Serial.println(WiFi.RSSI());
	Serial.print ("Mode: "); Serial.println(WiFi.getPhyMode());
  status = SETUP_SUCCESS;
}

// ------------------------
void loop() {
  // In general: Go to sleep 2 minutes after the last (foreign) bus activity, in case we forgot to switch off the device.
  if (millis() > _spiBlockoutTime + 120 * 1000) {
    Serial.println("No activity for 2 minutes, going to sleep");
    ESP.deepSleep(0);
  }

  // Early exit if any of the steps in setup failed.
  // Communicate the error via LED, the number of blinks indicates the phase where the error occurred.
  if (status != SETUP_SUCCESS) {
    for (int i = 0; i < status; i++) {
      LED_ON; 
      delay(150); 
      LED_OFF;
      delay(150);
    }
    delay(1000);
    return;
  }

  // If we're in "post as soon as bus used" mode (postIntervalSeconds <= 0), only proceed if
  // * The bus is free
  // * We haven't posted an update since the other host last used the bus
  int postIntervalSeconds = configJson["postIntervalSeconds"];
  if (postIntervalSeconds <= 0 && (!canWeTakeBus() || postedSinceBusTakenByOtherHost)) {
    Serial.println("Either bus is busy, or we've already posted since was last used");
    LED_ON;
    delay(500);
    return;
  }
  LED_OFF;

  // Read file from SD-card
  Serial.println("Waiting for bus.");
  while(!canWeTakeBus()) {delay(100);}
  takeBusControl();
  String filePath = configJson["filePath"];
  Serial.println("Reading file from SD: " + filePath);
  File32 dataFile;
  if (!dataFile.open(filePath.c_str(), FILE_READ)) {
    Serial.println("Open file failed");
    relinquishBusControl();
    status = ERROR_FILE_NOT_FOUND;
    return;
  }
  // Seek to the tail of the file if tailBytes is active
  long fileTailBytes = configJson["fileTailBytes"];
  long filePostSize = dataFile.fileSize();
  if (fileTailBytes > 0) {
    Serial.println("Tail feature active - only posting last " + String(fileTailBytes) + " of the file.");
    dataFile.seek(dataFile.fileSize() - fileTailBytes);
    filePostSize = fileTailBytes;
  }
  
  // Post file to endpoint
  long post_start = millis();
  String endpointUrl = configJson["endpointUrl"];
  Serial.println("Starting POST to " + endpointUrl + " host: " + endpointHost + " protocol: " + endpointProtocol);
  int httpCode;
  if (endpointProtocol == "https") {
    // Note HTTPS only works well with ESP 2.7.4 (not with e.g. 3.1.2) for reasons I did not investigate.
    WiFiClientSecure clientSecure;
    clientSecure.setInsecure();
    clientSecure.setSession(&session);
    clientSecure.connect(endpointHost, 443);
    httpCode = sendPostRequest(clientSecure, endpointUrl, dataFile, filePostSize);
    clientSecure.stop();
  } else { // Assume normal http
    WiFiClient client;
    client.connect(endpointHost, 80);
    httpCode = sendPostRequest(client, endpointUrl, dataFile, filePostSize);
    client.stop();
  }
  
  if (httpCode / 100 != 2) {
     Serial.println("Got non-200 exit code " + String(httpCode));
     status = ERROR_ENDPOINT;
     return;
  }
  Serial.println("Post duration: " + String(millis() - post_start));

  // Close file and relinquishBusControl to other SD host.
  postedSinceBusTakenByOtherHost = true;
  dataFile.close();
  relinquishBusControl();

  // Decrease posts
  remainingPosts--;
  Serial.println(String(remainingPosts) + " POSTs remaining.");
  // Go to sleep either when we've done all the posts, or it's been a minute since the last bus activity.
  if (remainingPosts <= 0) {
    Serial.println("Done, going to sleep");
    ESP.deepSleep(0);
  } else if (postIntervalSeconds > 0) {
    Serial.println("Waiting seconds: " + String(postIntervalSeconds));
    delay(postIntervalSeconds*1000);
  }
}

int sendPostRequest(WiFiClient &client, String endpointUrl, File32 &dataFile, long filePostSize) {
  HTTPClient http;
  http.begin(client, endpointUrl);
  http.addHeader("Authorization", configJson["endpointAuthorization"]);
  http.addHeader("Content-Type", "application/octet-stream");
  http.addHeader("Content-Length", String(filePostSize));
  http.addHeader("Connection", "close");
  int httpCode =  http.sendRequest("POST", &dataFile, filePostSize);
  http.end();
  return httpCode;
}

// This is a direct implementation of HTTP post. It's unused since it's less readable than HTTPClient and unstable when used with HTTPS.
// While developing, I noticed that the performance and stability of HTTPClient varies a lot between versions of the esp8266 SDK,
// and used this a lot for benchmarking and partially sanity-checking. Keeping here for reference.
int sendPostRequestRaw(WiFiClient &client, String endpointPath, String endpointHost, File32 &dataFile, long filePostSize) {
  String authorization = configJson["endpointAuthorization"];
  client.print("POST " + endpointPath + " HTTP/1.1\r\n" +
               "Host: " + endpointHost + "\r\n" +
               "Content-Type: application/octet-stream\r\n" +
               "Content-Length: " + String(filePostSize) + "\r\n" +
               "Authorization: " + authorization + "\r\n" +
               "Connection: close\r\n\r\n");
  client.write(dataFile);
  Serial.println("Request sent, await response.");
  int httpCode = -2;
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    Serial.print("HTTP Response:"); Serial.println(line);
    int responseCodeIndex = line.indexOf("HTTP/1");
    if (responseCodeIndex == -1) {continue;}
    String responseCodeString = line.substring(responseCodeIndex + 8, responseCodeIndex + 8 + 4);
    httpCode = responseCodeString.toInt();
  }
  return httpCode;
}

// SD bus sharing logic copied from ardyesp/FYSETC's ESPWebDAV
void ICACHE_RAM_ATTR sdInterruptHandler() {
  if(!_weTookBus) {
    _spiBlockoutTime = millis() + SPI_BLOCKOUT_PERIOD;
    postedSinceBusTakenByOtherHost = false;
  }
			
}

void sdSetup() {
  // ----- GPIO -------
	// Detect when other master uses SPI bus
	pinMode(CS_SENSE, INPUT);
	attachInterrupt(CS_SENSE, sdInterruptHandler, FALLING);

	// wait for other master to assert SPI bus first
	delay(SPI_BLOCKOUT_PERIOD);
}

// ------------------------
void takeBusControl()	{
// ------------------------
	_weTookBus = true;
	//LED_ON;
	pinMode(MISO_PIN, SPECIAL);	
	pinMode(MOSI_PIN, SPECIAL);	
	pinMode(SCLK_PIN, SPECIAL);	
	pinMode(SD_CS, OUTPUT);
}

// ------------------------
void relinquishBusControl()	{
// ------------------------
	pinMode(MISO_PIN, INPUT);	
	pinMode(MOSI_PIN, INPUT);	
	pinMode(SCLK_PIN, INPUT);	
	pinMode(SD_CS, INPUT);
	//LED_OFF;
	_weTookBus = false;
}

bool canWeTakeBus() {
	if(millis() < _spiBlockoutTime) {
    return false;
  }
  return true;
}
