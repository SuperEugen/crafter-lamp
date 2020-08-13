//***************************************************************************************************
//  crafter-lamp:               A LED working light with additional and separet controlled 
//                              light strip and spot light. Can be controlled via MQTT. 
//                              All three lights are dimmable via rotary knob. Double click 
//                              on the knob switches controll to the next lamp. A display shows 
//                              time and date and values of all three lamps. 
//                              The crafter lamp has a build-in Qi charger and 
//                              an additional USB fast charger port.
// 
//                              By Ingo Hoffmann.
//***************************************************************************************************
//
//  Hardware components:
//  Board:                      NodeMCU 1.0
//
//  Components:
//    OLED 128x64               connects via I2C
//    rotary enc with button    A and B connected via 10k ohm pull up resistors
//    3 LED 12V light strips    controlled via 3 MOSFETs
//    Power:                    powered from a 12V power supply. 
//                              5V is generated from a step down converter
//
//  Libraries used:
//    Wire                      Arduino I2C library
//    Adafruit_GFX              Adafruit grafik lib
//    Adafruit_SSD1306          Adafruit display lib
//    ESP8266Wifi               Espressif wifi lib
//    PubSubClient              connect to MQTT
//    Encoder                   rotary encoder lib; fixed Encoder.h file as described on GitHub
//                              https://github.com/PaulStoffregen/Encoder/issues/40
//    OneButton                 detect double clicks, long clicks etc
//    ezTime                    get time from an NTP server
//
//  Dev history:
//    24.03.2019, IH            initial tests
//    25.03.2019, IH            using NTP, impoved information display
//    26.03.2019, IH            using rotary knob and button
//    27.03.2019, IH            fade-in and -out, all off, all default, display improved
//    28.03.2019, IH            robust against failed wifi connect
//    29.03.2019, IH            publishing to mqtt
//    18.06.2019, IH            improved connecting
//    12.08.2020, IH            recompiled with updated libraries and fixed Encoder.h problem
//
//***************************************************************************************************

#define VERSION                 "1.1"   // 12.08.20

// libraries
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeMonoBoldOblique18pt7b.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <Encoder.h>
#include <OneButton.h>
#include <ezTime.h>

#include "secrets.h"

const String crafterLampVersion = "Crafter Lamp V1.1";

// PIN definitions
//      LED_BUILTIN         D0
//      PIN_WIRE_SCL        D1
//      PIN_WIRE_SDA        D2
#define LAMP_A_PIN          D3
#define ROTARY_SWITCH_PIN   D4    // also connected to ESP-12E LED
#define ROTARY_B_PIN        D5
#define ROTARY_A_PIN        D6
#define LAMP_B_PIN          D7
#define LAMP_C_PIN          D8
#define SCREEN_WIDTH        128
#define SCREEN_HEIGHT       64

// Declaration for an SSD1306 display connected to I2C (SDA, SCL pins)
#define OLED_RESET          -1    // Reset pin # (or -1 if sharing Arduino reset pin)

// mqtt topics, LampTopic + lampNumber + [BrightnessTopic | StateTopic]
const char* mqttLampTopic = "crafter-lamp";
const char* mqttBrightnessTopic = "brightness";
const char* mqttCommandTopic = "command";

// MQTT client ID
char* mqttClientID = "S-I-CrafterLamp";

// global variables
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
Encoder rotaryKnob(ROTARY_A_PIN, ROTARY_B_PIN);
OneButton rotaryButton(ROTARY_SWITCH_PIN, true);
Adafruit_SSD1306 oledDisplay(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Timezone myTZ;

// lamp values are stored here
typedef struct{
  int pin;
  bool isOn;
  bool isOnDefault;
  int value;
} lamp_t;

const int numberOfLamps = 3;
const int fadeSpeed = 7;            // milliseconds for each step, smaller numbers fade faster
const uint8_t displayAfterglow = 4; // seconds before switching back to default display
const int minLampValue = 2;         // to avoid switching lamp back on with a value of zero

lamp_t myLamps[numberOfLamps];
int selectedLamp = 0;               // A = 0, B = 1, C = 2
long rotaryOldPosition = 128;       // start with 50%
bool displayAlwaysOn = true;        // show clock even when all lights are out
bool lampsInUse = false;            // true if at least one lamp is on

bool displayConnected;
bool wifiConnected;
bool mqttConnected;

void setup() {
  int wifiRetries = 0;
  
  Serial.begin(74880);

  // configure pins
  pinMode(LAMP_A_PIN, OUTPUT);
  pinMode(LAMP_B_PIN, OUTPUT);
  pinMode(LAMP_C_PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);     // NodeMCU LED
  digitalWrite(LED_BUILTIN, HIGH);  // turn build-in LED off, inverted
  
  // initialise lamp array
  for(int i=0; i<numberOfLamps; i++) {
    myLamps[i].isOn = false;
    myLamps[i].isOnDefault = false;
    myLamps[i].value = 128;         // 50% brightness
  }
  myLamps[0].pin = LAMP_A_PIN;
  myLamps[1].pin = LAMP_B_PIN;
  myLamps[2].pin = LAMP_C_PIN;

  // initialise rotary button
  rotaryButton.attachClick(rotaryClick);
  rotaryButton.attachDoubleClick(rotaryDoubleClick);
  rotaryButton.attachLongPressStart(rotaryLongPressStart);

  // initialise rotary encoder
  rotaryKnob.write(myLamps[selectedLamp].value);

  // configure oled, i2c adress is 0x3C
  if(!oledDisplay.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("Display not connected"));
    displayConnected = false;
  } else {
    displayConnected = true;
    oledDisplay.display();            // init, shows Adafruit logo
    oledDisplay.clearDisplay();
    oledDisplay.setTextSize(1);       // Draw 1X-scale text
  }

  // connecting to WiFi network
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while ((WiFi.status() != WL_CONNECTED) && wifiRetries < 8) {
    delay(800);
    wifiRetries++;
  }
  if(WiFi.status() != WL_CONNECTED) {
    Serial.println(F("WiFi not connected"));
    wifiConnected = false;
  } else {
    wifiConnected = true;
    
    // connecting to NTP
    waitForSync();                    // Wait for ezTime to get its time synchronized
    myTZ.setLocation(F("Europe/Berlin"));
    setInterval(60 * 60 * 24);        // set NTP polling interval to daily
    setDebug(NONE);                   // NONE = set ezTime to quiet

    // connecting to MQTT
    mqttClient.setServer(mqttBroker, mqttPort);
    mqttClient.setCallback(mqttCallback);
    if (mqttClient.connect(mqttClientID, mqttUser, mqttPassword)) {
      mqttConnected = true;
      mqttSubscribeToMultipleTopics();
    } else {
      Serial.println(F("MQTT not connected"));
      mqttConnected = false;
    }
  }

  if(displayConnected) {
    // show date and time for the first time
    if(displayAlwaysOn) {
      if(wifiConnected) {
        displayDate();
        displayTime();
      }
      displayLampState();
    } else {
        oledDisplay.clearDisplay();
        oledDisplay.display();
    }
  }

}

void loop() {
  long rotaryNewPosition;

  // knob turned?
  if(myLamps[selectedLamp].isOn) {
    rotaryNewPosition = rotaryKnob.read();
    if(rotaryNewPosition != rotaryOldPosition) {
      if(rotaryNewPosition < 2) {
        rotaryKnob.write(2);
        rotaryNewPosition = 2;
      }
      if(rotaryNewPosition > 255) {
        rotaryKnob.write(255);
        rotaryNewPosition = 255;
      }
      myLamps[selectedLamp].value = rotaryNewPosition;
      rotaryOldPosition = rotaryNewPosition;
      analogWrite(myLamps[selectedLamp].pin, myLamps[selectedLamp].value);
      deleteEvent(clearDisplayAndPublishMQTT);  // delete all previously set events
      setEvent(clearDisplayAndPublishMQTT, makeTime(hour(), minute(), second() + displayAfterglow, \
        day(), month(), year()));
      if(displayConnected) {
        displayRotaryValue();
        displayLampState();
      }
    }
  }

  // new time?
  if(displayConnected) {
    if(lampsInUse || displayAlwaysOn) {
      if(minuteChanged() && wifiConnected) {
        displayDate();
        displayTime();
      }
    }
  }

  rotaryButton.tick();
  events();
  mqttClient.loop();
}

String getStringWithSpaces(uint8_t number, uint8_t width) {
  String numberAsString = String(number);
  String leadingSpaces = "";

  if(numberAsString.length() < width) {
    for(int i=numberAsString.length(); i<width; i++) {
      leadingSpaces = leadingSpaces + " ";
    }
  }
  return(leadingSpaces + numberAsString);
}

void mqttSubscribeToMultipleTopics() {
  int topicLength;  
  String wildcardTopic;

  topicLength = strlen(mqttLampTopic);
  wildcardTopic = mqttLampTopic;
  wildcardTopic.concat("/#");
  mqttClient.subscribe(wildcardTopic.c_str());
}

void mqttReconnect() {
  // Loop until we're reconnected
  while (!mqttClient.connected()) {
    // Attempt to connect
    if (mqttClient.connect(mqttClientID, mqttUser, mqttPassword)) {
      // resubscribe
      mqttSubscribeToMultipleTopics();
    } else {
      Serial.print("MQTT failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  int lamp;
  String topicPart;
  String stringValue;
  int intValue;
  bool lampsState = false;

  lamp = topic[strlen(mqttLampTopic) + 1] - 48;  // '/' adds one character, 48 is ASCII code '0'
  topicPart = String(topic).substring(strlen(mqttLampTopic) + 3); // '/1/' adds 3 characters
  payload[length] = '\0';
  stringValue = String((char*) payload);
  intValue = stringValue.toInt();
  if(lamp < 0 || lamp > (numberOfLamps - 1)) {
    Serial.println("MQTT: wrong lamp");
  } else {
    if(topicPart == mqttBrightnessTopic) {
      // do nothing, this is our own message
    } else if(topicPart == mqttCommandTopic) {
      if(intValue < 0 || intValue > 255) {
        Serial.println("MQTT: wrong value");
      } else {
        // turn lamp on or off and set value
        if(intValue > 0) {
          myLamps[lamp].value = intValue;
          analogWrite(myLamps[lamp].pin, myLamps[lamp].value);
          rotaryKnob.write(intValue);
          myLamps[lamp].isOn = true;
          lampsInUse = true;
        } else {
          analogWrite(myLamps[lamp].pin, 0);
          myLamps[lamp].isOn = false;
          for(int i=0; i<numberOfLamps; i++) {
            lampsState = lampsState || myLamps[i].isOn;
          }
          lampsInUse = lampsState;
        }
        if(displayConnected) {
          if(lampsInUse || displayAlwaysOn) {
            displayLampState();
            if(wifiConnected) {
              displayDate();
              displayTime();
            }
          } else {
            oledDisplay.clearDisplay();
            oledDisplay.display();
          }
        }
      }
    } else {
      Serial.println("MQTT: unknown topic");
    }
  }
}

void mqttPublishValue(String topic, String value) {
  String completeTopic;

  if(!mqttClient.connected()) {
    mqttReconnect();
  }
  mqttClient.loop();
  completeTopic = mqttLampTopic;
  completeTopic.concat('/');
  completeTopic.concat(selectedLamp);
  completeTopic.concat('/');
  completeTopic.concat(topic);
  mqttClient.publish(completeTopic.c_str(), value.c_str());
}

void clearDisplayAndPublishMQTT() {
  if(wifiConnected) {
    displayTime();
  } else {
    oledDisplay.fillRect(0, 8, oledDisplay.width(), 48, BLACK);
    oledDisplay.display();
  }
  if(mqttConnected) {
    mqttPublishValue(mqttBrightnessTopic, String(myLamps[selectedLamp].value));
  }
}

void rotaryClick() {
  bool lampsState = false;
  
  if(myLamps[selectedLamp].isOn) {
    switchLamp(false);
    for(int i=0; i<numberOfLamps; i++) {
      lampsState = lampsState || myLamps[i].isOn;
    }
    lampsInUse = lampsState;
  } else {
    if(! lampsInUse) {
      if(displayConnected && wifiConnected) {
        displayDate();
        displayTime();
      }
      lampsInUse = true;
    }
    switchLamp(true);
  }
  if(displayConnected) {
    if(lampsInUse) {
        displayLampState();
    } else if(displayAlwaysOn) {
      displayLampState();
    } else {
      oledDisplay.clearDisplay();
      oledDisplay.display();
    }
  }
}

void rotaryDoubleClick() {
  selectedLamp = selectedLamp + 1;
  if(selectedLamp >= numberOfLamps) {
    selectedLamp = 0;
  }
  rotaryKnob.write(myLamps[selectedLamp].value);
  if(displayConnected) {
    displayLampState();
  }
}

void rotaryLongPressStart() {
  if(lampsInUse) {
    for(int i=0; i<numberOfLamps; i++) {
      myLamps[i].isOnDefault = myLamps[i].isOn;
      if(myLamps[i].isOn) {
        selectedLamp = i;
        switchLamp(false);
      }
    }
    if(displayConnected) {
      if(displayAlwaysOn) {
        selectedLamp = 0;
        displayLampState();
      } else {
        oledDisplay.clearDisplay();
        oledDisplay.display();    
      }
    }
    lampsInUse = false;
  } else {
    for(int i=0; i<numberOfLamps; i++) {
      myLamps[i].isOn = myLamps[i].isOnDefault;
      if(myLamps[i].isOn) {
        selectedLamp = i;
        switchLamp(myLamps[i].isOn);
      }
    }
    selectedLamp = 0;
    if(displayConnected) {
      displayLampState();
      if(wifiConnected) {
        displayDate();
        displayTime();
      }
    }
    lampsInUse = true;
  }
}

void switchLamp(bool switchOn) {
  for(int i=0; i<=myLamps[selectedLamp].value; i++) {
    if(switchOn) {
      analogWrite(myLamps[selectedLamp].pin, i);
    } else {
      analogWrite(myLamps[selectedLamp].pin, myLamps[selectedLamp].value - i);
    }
    delay(fadeSpeed);
  }
  myLamps[selectedLamp].isOn = switchOn;
  if(mqttConnected) {
    if(switchOn) {
      mqttPublishValue(mqttBrightnessTopic, String(myLamps[selectedLamp].value));
    } else {
      mqttPublishValue(mqttBrightnessTopic, "0");
    }
  }
}

void displayLampState() {
  oledDisplay.fillRect(0, 56, oledDisplay.width(), 8, BLACK);
  oledDisplay.setFont();
  oledDisplay.setCursor(0, 56);               // Start at bottom

  for(int i=0; i<numberOfLamps; i++) {
    if(i == selectedLamp) {
      oledDisplay.setTextColor(BLACK, WHITE); // Draw inverse text  
    } else {
      oledDisplay.setTextColor(WHITE, BLACK); // Draw white text  
    }
    oledDisplay.print(" ");
    oledDisplay.print(char(65 + i));
    oledDisplay.print(":");
    if(myLamps[i].isOn) {
      oledDisplay.print(getStringWithSpaces(map(myLamps[i].value, 1, 255, 1, 100), 3));
      oledDisplay.print("%");
    } else {
      oledDisplay.print(" aus");
    }
  }
  oledDisplay.display();  
}

void displayDate() {
  oledDisplay.fillRect(0, 0, oledDisplay.width(), 8, BLACK);
  oledDisplay.setTextColor(WHITE, BLACK);  // Draw white text
  oledDisplay.setFont();
  oledDisplay.setCursor(0, 0);             // Start at top-left corner
  oledDisplay.print(getDayString(myTZ.weekday()) + myTZ.dateTime(", d.m.y"));  
  oledDisplay.display();
}

void displayTime() {
  oledDisplay.fillRect(0, 8, oledDisplay.width(), 48, BLACK);
  oledDisplay.setTextColor(WHITE, BLACK);  // Draw white text
  oledDisplay.setFont(&FreeMonoBoldOblique18pt7b);
  oledDisplay.setCursor(10, 44);           // Start in the middle, baseline of font is used
  oledDisplay.print(getStringWithSpaces(myTZ.hour(), 2));
  oledDisplay.print(myTZ.dateTime(":i"));
  oledDisplay.display();
}

void displayRotaryValue() {
  oledDisplay.fillRect(0, 8, oledDisplay.width(), 48, BLACK);
  oledDisplay.fillRect(10, 23, 108, 18, WHITE);
  oledDisplay.fillRect(14, 27, 100, 10, BLACK);
  oledDisplay.fillRect(14, 27, map(myLamps[selectedLamp].value, 1, 255, 1, 100), 10, WHITE);
  oledDisplay.display();
}

String getDayString(uint8_t currentDay) {
  switch(currentDay) {
    case 1: return("Sonntag");
    case 2: return("Montag");
    case 3: return("Dienstag");
    case 4: return("Mittwoch");
    case 5: return("Donnerstag");
    case 6: return("Freitag");
    case 7: return("Samstag");
  }
}
