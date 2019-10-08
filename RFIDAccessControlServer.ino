/**
 * RFID Access Control Server
 *
 * This project implements a single stand-alone RFID access control
 * system that can operate independently of a host computer or any
 * other device. It uses either an ID-12 RFID reader module from ID
 * Innovations or an RDM630 RFID reader module from Seeed Studio to
 * scan for 125KHz RFID tags, and when a recognised tag is identified
 * it toggles an output for a configurable duration, typically 2
 * seconds. The output can then be used to control a relay to trip an
 * electric striker plate to release a door lock.
 *
 * Because this project is intended to provide a minimal working system
 * it does not have any provision for database updates to be managed
 * externally from a host, so updates to the accepted cards must be
 * made by changing the values in the code, recompiling the program,
 * and re-uploading it to the Arduino. It does however report card
 * readings (both successful and unsuccessful) via the serial
 * connection so you can monitor the system using a connected computer.
 *
 * Some of this code was inspired by Tom Igoe's excellent RFID tutorial
 * which is detailed on his blog at:
 *   http://www.tigoe.net/pcomp/code/category/PHP/347
 * And also from the ID-12 example code on the Arduino Playground at:
 *   http://www.arduino.cc/playground/Code/ID12
 *
 * Copyright Jonathan Oxer <jon@oxer.com.au>
 * http://www.practicalarduino.com/projects/medium/rfid-access-control
 *
 * Copyright Dzordzs Barens <dzordzs@dzoka.l>
 * http://www.dzoka.lv
 */

// Set up the serial connection to the RFID reader module. In order to
// keep the Arduino TX and RX pins free for communication with a host,
// the sketch uses the SoftwareSerial library to implement serial
// communications on other pins.
#include <SoftwareSerial.h>
#include <SPI.h>
#include <Ethernet.h>

// The RFID module's TX pin needs to be connected to the Arduino. Module
// RX doesn't need to be connected to anything since we won't send
// commands to it, but SoftwareSerial requires us to define a pin for
// TX anyway so you can either connect module RX to Arduino TX or just
// leave them disconnected.

// If you have built the circuit exactly as described in Practical
// If you are using the Freetronics RFID Lock Shield, use pins D4 / D5:
#define rxPin 5
#define txPin 4

// Create a software serial object for the connection to the RFID module
SoftwareSerial rfid = SoftwareSerial( rxPin, txPin );

// Set up outputs for the strike plate and status LEDs.
// If you are using the Freetronics RFID Lock Shield, use pins D6 / D7:
#define strikePlate 6
#define ledPin 7

// Specify how long the strike plate should be held open.
#define unlockSeconds 2

// The tag database consists of two parts. The first part is an array of
// tag values with each tag taking up 5 bytes. The second is a list of
// names with one name for each tag (ie: group of 5 bytes).
//char* allowedTags[100];

byte tags[200][5];
int incomingByte = 0;    // To store incoming serial data

byte mac[] = {0x02, 0x00, 0x00, 0xFF, 0x00, 0x02};
byte ip[] = {10, 5, 1, 180};
byte server[] = {10, 5, 1, 21};
int port = 11000;
EthernetClient client;
unsigned long prevTime = 0;
unsigned long interval = 300000;    // send timestamp to server in miliseconds

/**
 * Setup
 */
void setup() {
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);
  pinMode(strikePlate, OUTPUT);
  digitalWrite(strikePlate, LOW);
  Serial.begin(9600);       // Serial port for connection to host
  rfid.begin(9600);        // Serial port for connection to RFID module
  Ethernet.begin(mac, ip);
  timeServer(0);          // request data reload
  rfid.listen();
  Serial.println("Ready...");
}

/**
 * Loop
 */
void loop() {
  byte i         = 0;
  byte val       = 0;
  byte checksum  = 0;
  byte bytesRead = 0;
  byte tempByte  = 0;
  byte tagBytes[6];    // "Unique" tags are only 5 bytes but we need an extra byte for the checksum
  char tagValue[11];
  
  if(rfid.available() > 0) {
    val = rfid.read();
    if(val == 2) {              // Check for header
      bytesRead = 0;            // start storing
      while (bytesRead < 12) {  // Read 10 digit code + 2 digit checksum
        if(rfid.available() > 0) {
          val = rfid.read();
          if(bytesRead < 10) {  // Append the first 10 bytes (0 to 9) to the raw tag value
            tagValue[bytesRead] = val;
          }
          if((val == 0x0D)||(val == 0x0A)||(val == 0x03)||(val == 0x02)) {
            break;              // header or stop byte before the 10 digit reading is complete - Stop reading
          }
          // Ascii/Hex conversion:
          if ((val >= '0') && (val <= '9')) {
            val = val - '0';
          }
          else if ((val >= 'A') && (val <= 'F')) {
            val = 10 + val - 'A';
          }
          // Every two hex-digits, add a byte to the code:
          if (bytesRead & 1 == 1) {
            // Make space for this hex-digit by shifting the previous digit 4 bits to the left
            tagBytes[bytesRead >> 1] = (val | (tempByte << 4));
            if (bytesRead >> 1 != 5) {                // If we're at the checksum byte,
              checksum ^= tagBytes[bytesRead >> 1];   // Calculate the checksum... (XOR)
            };
          } else {
            tempByte = val;                           // Store the first hex digit first
          };
          bytesRead++;                                // Ready to read next digit
        }
      }

      // Send the result to the host connected via USB
      if (bytesRead == 12) {                        // 12 digit read is complete
        tagValue[10] = '\0';                        // Null-terminate the string
        if(tagBytes[5] == checksum) {
          Serial.print(tagValue);
          int tagId = findTag(tagBytes);            // Search the tag database for this particular tag
          if(tagId > 0){                            // if tag is found
            Serial.println(" - Y");
            unlock();                               // Fire the strike plate to open the lock
            tagValue[10] = 'Y';
          } else {
            Serial.println(" - N");
            tagValue[10] = 'N';
          }
          tagValue[11] = '\0';
          informServer(tagValue);
        }
      }
      bytesRead = 0;
    }
  }
  
  unsigned long nowTime = millis();
  if(nowTime - prevTime >= interval) {
    prevTime = nowTime;
    Serial.print('T');
    Serial.println(nowTime);
    timeServer(nowTime);
  }
}

/*
* Send timestamp to server
*/
void timeServer(unsigned long time){
  client.connect(server, port);
  if (client.connected()){
    if(time == 0){
      client.println('R');    // just started - request reload data
    } else {
      client.print('T');      // timestamp
      client.println(time);
    }
    delay(500);
    if (client.read() == 'R'){  //available()){
      Serial.print("Reload data... ");
      reload();
      Serial.println("done.");
    }
    client.stop();
  }
}

/*
* Send data to server and update database
*/
void informServer(char* value){
  client.connect(server, port);
  if (client.connected()){
    client.println(value);
    client.stop();
  }
}

/*
* Receive database updates
*/
void reload() {
  int toRead = 0;
  byte* rcv = &tags[0][0];
  while(toRead < 1000) {
    if(client.available()){
      rcv[toRead] = client.read();
      toRead++;
    }
  }
}

/**
 * Fire the relay to activate the strike plate for the configured
 * number of seconds.
 */
void unlock() {
  digitalWrite(ledPin, HIGH);
  digitalWrite(strikePlate, HIGH);
  delay(unlockSeconds * 1000);
  digitalWrite(strikePlate, LOW);
  digitalWrite(ledPin, LOW);
}

/**
 * Search for a specific tag in the database
 */
int findTag(byte tagValue[5]){
  for (int thisCard = 0; thisCard < 200; thisCard++){
    if(compareTags(tagValue, tags[thisCard]) == 0)
    {
      return(thisCard + 1);
    }
  }
  return(0);
}

/*
* Compare two tags
*/
int compareTags(byte tag1[5], byte tag2[5]){
  for(int i=0;i<5;i++){
    if(tag1[i] != tag2[i]){
      return 1;              // mismatch
    }
  }
  return 0;                  // match
}


