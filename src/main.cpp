#include "Arduino.h"
#include "RTClib.h"
#include "NewPing.h"
#include "SoftwareSerial.h"
#include "Wire.h"

/**
 * This is a small IoT project, to automatically messure the water height of
 * the Freudensee located in Hauzenberg.
 * This is the code of the transmitter station, which provides the messure of
 * the water heigth by using the HC-SR04 ultra sonic sensor.
 * After the water heigth is messured the data will be sent every 2 hours to a
 * server over the GPRS network using the SIM800L module.
 */

// First critical point (20 cm under foodbridge)
#define CRIT_DIST_1 3

// Second critical point (10 cm under foodbridge
#define CRIT_DIST_2 2

// Third critical point (water entering the hut)
#define CRIT_DIST_3 1

 // Size of allowed numbers
#define SIZE_OF_ALLOWED_NUMBERS 5

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

// Server URL
#define SERVER_URL "ServerURL"

// Server password
#define SERVER_PW "ServerPw"

// Interval in ms for trying to get valid values after getting invalid ones
#define INTERVAL 1200000

// Create software serial object to communicate with SIM800L
SoftwareSerial mySerial(TX_PIN, RX_PIN);

// Create a NewPing object to communicate with the ultra sonic sensor.
NewPing sonar(TRIGGER_PIN, ECHO_PIN, MAX_DIST);

// Create a RTClib object to communicate with rtc module.
RTC_DS3231 rtc;

// String array of the numbers to get notifed
String allowedNumbers[] = {};

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

// Time in ms since the last correct messurement
long previousMillis = 0;

/**
 * Method which providing the hand off of commands from the Arduino to the
 * SIM800L module and vise versa.
 */
void updateSerial() {
  delay(500);
  while (Serial.available()) {

    //Forward what Serial received to Software Serial Port
    mySerial.write(Serial.read());
  }
  while(mySerial.available()) {

    //Forward what Software Serial received to Serial Port
    Serial.write(mySerial.read());
  }
}

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
      return "Meldestufe 1 erreicht!!!\nWasserstand:  cm";
    case 2:
      return "Meldestufe 2 erreicht!!!\nWasserstand: cm";
    case 3:
      return "Wir saufen ab!!! Meldestufe 3 erreicht!!!\nWasserstand: cm";
    case 4:
      return "Meldestufe 1 aufgehoben!!!\nWasserstand: cm";
    case 5:
      return "Meldestufe 2 aufgehoben!!!\nWasserstand: cm";
    case 6:
      return "Meldestufe 3 aufgehoben!!!\nWasserstand: cm";
    case 7:
      return "Fehler mit dem Ultraschallsensor bitte Überprüfen!";
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
  delay(500);

  // Configuring TEXT mode
  mySerial.println("AT+CMGF=1");
  updateSerial();

  // Command to write a sms
  mySerial.print("AT+CMGS=\"");
  mySerial.print(number);
  mySerial.println("\"");
  updateSerial();
  mySerial.print(createMessage(messageCode));
  updateSerial();

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
  }
}

/**
 * Method which provides the initializing the connection to the GPRS-Network
 * (mobile network).
 */
void initGPRS() {

  // Configure the module for GPRS connection
  mySerial.println("AT+SAPBR=3,1,\"Contype\",\"GPRS\"");
  updateSerial();
  delay(500);

  // Access data for the APN (needed for GPRS connection)
  mySerial.println("AT+CSTT=\"internet.t-mobile\",\"t-mobile\",\"tm\"");
  updateSerial();
  delay(500);

  // Command for connecting to the GPRS network
  mySerial.println("AT+SAPBR=1,1");
  updateSerial();
  delay(3000);

  /*
   * Command to check if we already got a ip (if this isn't executed some weird
   * failures occurs)
   */
  mySerial.println("AT+SAPBR=2,1");
  updateSerial();
  delay(2000);
}

/**
 * Method which provides the initializing of HTTP and SSL.
 */
void initHTTP() {
  mySerial.println("AT+HTTPINIT");
  updateSerial();
  delay(500);
  mySerial.println("AT+HTTPSSL=1");
  updateSerial();
  delay(500);

  // Set user ID to 1 (Needed HTTP param)
  mySerial.println("AT+HTTPPARA=\"CID\",1");
  updateSerial();
  delay(500);
}

/**
 * Method which terminates SSL, HTTP and the mobile network.
 */
void terminateConnection() {
  mySerial.println("AT+HTTPTERM");
  updateSerial();
  delay(500);

  // Command to disconnect from the GPRS network
  mySerial.println("AT+SAPBR=0,1");
  updateSerial();
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
  mySerial.println("\"");
  updateSerial();
  delay(500);

  // Establish the HTTP connection
  mySerial.println("AT+HTTPACTION=0");
  updateSerial();
  delay(5000);
  updateSerial();
  Serial.print("Gemessener Stand:");
  Serial.println(sonar.ping_cm());
  terminateConnection();
}

/**
 * Check the water height if a critical point is reached, if so send a sms
 * warning and data to the server. Also it inform via sms if the water is back
 * again under a certain critical point.
 */
void checkWaterHeight() {
  Serial.println(messuredHeigth);
  if (messuredHeigth <= CRIT_DIST_3 && !warning3Sent) {
    Serial.println(createMessage(3));
    warnAll(3);
    delay(10000);
    sendDataToServer();
    warning3Sent = true;
  } else if (messuredHeigth <= CRIT_DIST_2 && !warning2Sent) {
    Serial.println(createMessage(2));
    warnAll(2);
    delay(10000);
    sendDataToServer();
    warning2Sent = true;
  } else if (messuredHeigth <= CRIT_DIST_1 && !warning1Sent) {
    Serial.println(createMessage(1));
    warnAll(1);
    delay(10000);
    sendDataToServer();
    warning1Sent = true;
  } else if (messuredHeigth > CRIT_DIST_3 && warning3Sent) {
    Serial.println(createMessage(6));
    warnAll(6);
    delay(10000);
    sendDataToServer();
    warning3Sent = false;
  }else if (messuredHeigth > CRIT_DIST_2 && warning2Sent) {
    Serial.println(createMessage(5));
    warnAll(5);
    delay(10000);
    sendDataToServer();
    warning2Sent = false;
  }else if (messuredHeigth > CRIT_DIST_1 && warning1Sent) {
    Serial.println(createMessage(4));
    warnAll(4);
    delay(10000);
    sendDataToServer();
    warning1Sent = false;
  }
}


/**
 * Setup which needs to be done before the loop can start.
 */
void setup() {

  // Begin serial communication with Arduino and Arduino IDE (Serial Monitor)
  Serial.begin(9600);

  //Begin serial communication with Arduino and SIM800L
  mySerial.begin(9600);

  // Configure the SIM800L to show the number of a caller
  //mySerial.println("AT+CLIP=1");
  updateSerial();
  delay(10000);

  // If rtc module isn't working stop Arduino and send a sms to inform the admin
  if (! rtc.begin()) {
    sendingSMS(allowedNumbers[0], 8);
    updateSerial();
    while (1);
  }
}

/**
 * Main function of the program.
 */
void loop() {
  messuredHeigth = sonar.ping_cm();
  long currentMillis = millis();
  Serial.println(messuredHeigth);

  // Check if sensor getting no wrong values
  if (messuredHeigth > MIN_DIST && messuredHeigth < MAX_DIST) {
    messureFail = false;
    previousMillis = currentMillis;
    checkWaterHeight();

    // Send data every 10 minutes.
    DateTime now = rtc.now();
    if ((now.minute() % 10 == 0) && (!dataSent)) {
      sendDataToServer();
      dataSent = true;
    } else if (now.minute() % 10 != 0) {
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
