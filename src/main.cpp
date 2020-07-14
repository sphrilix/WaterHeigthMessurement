#include "Arduino.h"
#include "RTClib.h"
#include "NewPing.h"
#include "SoftwareSerial.h"
#include "Wire.h"
#include "OneWire.h"
#include "DallasTemperature.h"

/**
 * This is a small IoT project, to automatically messure the water height and
 * temperature of the Freudensee located in Hauzenberg.
 * This is the code of the transmitter station, which provides the messure of
 * the water heigth by using the HC-SR04 ultra sonic sensor, as well using the
 * DS18B20 module to messure the temperature of the lake.
 * After the water heigth and temperature are messured the data will be sent
 * every 30 minutes to a server over the GPRS network using the SIM800L module.
 * The values can be seen here: https://wawa-wasserstand.herokuapp.com/
 */

// First critical point (20 cm under foodbridge)
#define CRIT_DIST_1 322

// Second critical point (10 cm under foodbridge
#define CRIT_DIST_2 332

// Third critical point (water entering the hut)
#define CRIT_DIST_3 342

 // Size of allowed numbers
#define SIZE_OF_ALLOWED_NUMBERS 4

// Trigger of the ultra sonic module
#define TRIGGER_PIN 7

// Echo of the ultra sonic module
#define ECHO_PIN 6

// The maximum distance to be messured
#define MAX_DIST 400

// The minimum distance to be messured
#define MIN_DIST 0

// TX pin of the SIM800L module
#define TX_PIN 2

// RX pin of the SIM800L module
#define RX_PIN 3

// RST pin of the SIM800L module
#define RST_PIN 9

// Server URL
#define SERVER_URL "https://wawa-wasserstand.herokuapp.com/"

// Server password
#define SERVER_PW "gxcxWUxezdAgrhZz2EZH/"

// Interval in ms for trying to get valid values after getting invalid ones
#define INTERVAL 1200000

// One wire bus pin of the DS18B20
#define ONE_WIRE_BUS 4

// Distance from the sensor to the deepest point of the lake
#define DIST_OVER_NULL 402

// Create software serial object to communicate with SIM800L
SoftwareSerial mySerial(TX_PIN, RX_PIN);

// Create a NewPing object to communicate with the ultra sonic sensor.
NewPing sonar(TRIGGER_PIN, ECHO_PIN, MAX_DIST);

// Init the one wire bus to the pin of the DS18B20
OneWire oneWire(ONE_WIRE_BUS);

// Set the reference of the one wire bus to the libary which calculates the temperature
DallasTemperature tempSensor(&oneWire);

// Create a RTClib object to communicate with rtc module.
RTC_DS3231 rtc;

// String array of the numbers to get notifed
String allowedNumbers[] = {"+4915142437055", "+4915224760882", "+491711707191", "+491606488035"};

// Boolean if criticial point 1 is reached and the corresponding warning is sent
boolean warning1Sent = false;

// Boolean if criticial point 2 is reached and the corresponding warning is sent
boolean warning2Sent = false;

// Boolean if criticial point 3 is reached and the corresponding warning is sent
boolean warning3Sent = false;

// Boolean if data were sent to the server
boolean dataSent = false;

// Boolean if there's a messure failure
boolean messureFail = false;

// Messured heigth of the water
int messuredHeigth = 0;

// Messured heigth from the ultra sonoc sensor
int rawMessuredHeight = 0;

// Messured water temperature
int messuredWaterTemp = 0;

// Time in ms since the last correct messurement
long previousMillis = 0;

/**
 * Method which creates a message corresponding to a given code and returns it.
 * @param  code The internal message code
 * @return Returns the created message.
 */
String createMessage(int code) {
  if (code < 0 || code > 8) {
    return "Invalid ErrorCode";
  }
  switch (code) {
    case 0:
      return "Wasserstand: cm";
    case 1:
      return "Meldestufe 1 erreicht!!!";
    case 2:
      return "Meldestufe 2 erreicht!!!";
    case 3:
      return "Wir saufen ab!!! Meldestufe 3 erreicht!!!";
    case 4:
      return "Meldestufe 1 aufgehoben!!!";
    case 5:
      return "Meldestufe 2 aufgehoben!!!";
    case 6:
      return "Meldestufe 3 aufgehoben!!!";
    case 7:
      return "Sensoren liefern falsche Werte! Bitte überprüfen!";
    case 8:
      return "Fehler mit dem RTC-Modul bitte überprüfen!";
  }
}

/**
 * Method which sends a sms with a given message to a given number.
 * @param number      Given number of the recipient
 * @param messageCode Given message to be sent
 */
void sendingSMS(String number, int messageCode) {
  delay(1500);

  // Configuring TEXT mode
  mySerial.println("AT+CMGF=1");

  // Command to write a sms
  mySerial.print("AT+CMGS=\"");
  mySerial.print(number);
  mySerial.println("\"");
  mySerial.print(createMessage(messageCode));
  delay(1000);

  // HEX-Code of the char the SIM800L needs to know the end of the sms
  mySerial.write(26);
}

/**
 * Sends a sms given by the message code to all given numbers.
 * @param messageCode The given message code.
 */
void warnAll(int messageCode) {
  for (int i = 0; i < SIZE_OF_ALLOWED_NUMBERS; i++) {
    sendingSMS(allowedNumbers[i], messageCode);
    delay(1000);
  }
}

/**
 * Resets the module by setting the RST_PIN low.
 */
void resetSIM800L() {
  digitalWrite(RST_PIN, LOW);
  delay(500);
  digitalWrite(RST_PIN, HIGH);
}

/**
 * Method which provides the initializing the connection to the GPRS-Network
 * (mobile network).
 */
void initGPRS() {

  // Configure the module for GPRS connection
  mySerial.println("AT+SAPBR=3,1,\"Contype\",\"GPRS\"");
  delay(500);

  // Access data for the APN (needed for GPRS connection)
  mySerial.println("AT+CSTT=\"internet.t-mobile\",\"t-mobile\",\"tm\"");
  delay(500);

  // Command for connecting to the GPRS network
  mySerial.println("AT+SAPBR=1,1");
  delay(3000);

  /*
   * Command to check if we already got a ip (if this isn't executed some weird
   * failures occurs)
   */
  mySerial.println("AT+SAPBR=2,1");
  delay(2000);
}

/**
 * Method which provides the initializing of HTTP and SSL.
 */
void initHTTP() {
  mySerial.println("AT+HTTPINIT");
  delay(500);
  mySerial.println("AT+HTTPSSL=1");
  delay(500);

  // Set user ID to 1 (Needed HTTP param)
  mySerial.println("AT+HTTPPARA=\"CID\",1");
  delay(500);
}

/**
 * Method which terminates SSL, HTTP and the mobile network.
 */
void terminateConnection() {
  mySerial.println("AT+HTTPTERM");
  delay(500);

  // Command to disconnect from the GPRS network
  mySerial.println("AT+SAPBR=0,1");
  delay(500);
}

/**
 * Method which provides the sending of the water heigth to the server.
 */
void sendDataToServer() {
  initGPRS();
  initHTTP();

  // Command to write URL with sensor data to the module
  mySerial.print("AT+HTTPPARA=\"URL\",\"");
  mySerial.print(SERVER_URL);
  mySerial.print(SERVER_PW);
  mySerial.print(messuredHeigth);
  mySerial.print("/");
  mySerial.print(messuredWaterTemp);
  mySerial.print("/");
  mySerial.println("\"");
  delay(500);

  // Establish the HTTP connection
  mySerial.println("AT+HTTPACTION=0");
  delay(30000);
  Serial.print("Gemessener Stand:");
  Serial.println(messuredHeigth);
  terminateConnection();
  resetSIM800L();
}

/**
 * Check the water height if a critical point is reached, if so send a sms
 * warning and data to the server. Also it inform via sms if the water is back
 * again under a certain critical point.
 */
void checkWaterHeight() {
  Serial.println(messuredHeigth);
  if (messuredHeigth >= CRIT_DIST_3 && !warning3Sent) {
    Serial.println(createMessage(3));
    warnAll(3);
    delay(10000);
    sendDataToServer();
    warning3Sent = true;
  } else if (messuredHeigth >= CRIT_DIST_2 && !warning2Sent) {
    Serial.println(createMessage(2));
    warnAll(2);
    delay(10000);
    sendDataToServer();
    warning2Sent = true;
  } else if (messuredHeigth >= CRIT_DIST_1 && !warning1Sent) {
    Serial.println(createMessage(1));
    warnAll(1);
    delay(10000);
    sendDataToServer();
    warning1Sent = true;
  } else if (messuredHeigth < CRIT_DIST_3 && warning3Sent) {
    Serial.println(createMessage(6));
    warnAll(6);
    delay(10000);
    sendDataToServer();
    warning3Sent = false;
  }else if (messuredHeigth < CRIT_DIST_2 && warning2Sent) {
    Serial.println(createMessage(5));
    warnAll(5);
    delay(10000);
    sendDataToServer();
    warning2Sent = false;
  }else if (messuredHeigth < CRIT_DIST_1 && warning1Sent) {
    Serial.println(createMessage(4));
    warnAll(4);
    delay(10000);
    sendDataToServer();
    warning1Sent = false;
  }
}

/**
 * Calculates the real water heigth, distance from the sensor being above the
 * deepest point of the lake minus the messured distance from the sensor to the
 * water level.
 * @return Returns the real water height.
 */
int calcWaterHeigth() {
  return DIST_OVER_NULL - rawMessuredHeight;
}

/**
 * Setup which needs to be done before the loop can start.
 */
void setup() {
  pinMode(RST_PIN, OUTPUT);
  digitalWrite(RST_PIN, HIGH);
  delay(20000);

  // Begin serial communication with Arduino and Arduino IDE (Serial Monitor)
  Serial.begin(9600);

  //Begin serial communication with Arduino and SIM800L
  mySerial.begin(9600);

  // If rtc module isn't working stop Arduino and send a sms to inform the admin
  if (! rtc.begin()) {
    sendingSMS(allowedNumbers[0], 8);
    while (1);
  }

  // Start the DS18B20
  tempSensor.begin();
}

/**
 * Main function of the program.
 */
void loop() {
  rawMessuredHeight = 0;
  for (int i = 0; i < 100; i++) {
    rawMessuredHeight = rawMessuredHeight + sonar.ping_cm();
    delay(100);
  }
  rawMessuredHeight = rawMessuredHeight / 100;

  //rawMessuredHeight = sonar.ping_cm();
  messuredHeigth = calcWaterHeigth();
  tempSensor.requestTemperatures();
  float rawWaterTemp = tempSensor.getTempCByIndex(0);

  // Float can't be sent so we need to cast it to a int
  messuredWaterTemp = int (rawWaterTemp*10);
  long currentMillis = millis();
  Serial.println(messuredHeigth);
  Serial.println(rawWaterTemp);

  // Check if sensor getting no wrong values
  if (rawMessuredHeight > MIN_DIST && rawMessuredHeight < MAX_DIST
          && rawWaterTemp != DEVICE_DISCONNECTED_C) {
    messureFail = false;
    previousMillis = currentMillis;
    checkWaterHeight();

    // Send data every 2 hours.
    DateTime now = rtc.now();
    if ((now.minute() % 2 == 0) && (!dataSent)) {
      sendDataToServer();
      dataSent = true;
    } else if (now.minute() % 2 != 0) {
      dataSent = false;
    }

  /*
   * Sensor delivering wrong values for INTERVAL ms, so inform admin and stop
   * program
   */
  } else if (currentMillis - previousMillis >= INTERVAL && messureFail) {
    sendingSMS(allowedNumbers[0], 7);
    while (1);
  } else {
    messureFail = true;
  }
  delay(500);
}
