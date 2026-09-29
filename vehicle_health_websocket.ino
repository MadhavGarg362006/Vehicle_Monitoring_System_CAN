/*
  Normal ESP32 vehicle-health simulator -> WSS relay.
  Install ArduinoWebsockets by gilmaimon from Arduino Library Manager.
*/

#include <WiFi.h>
#include <ArduinoWebsockets.h>
using namespace websockets;

const char* WIFI_SSID="YOUR_WIFI";
const char* WIFI_PASSWORD="YOUR_WIFI_PASSWORD";
const char* WS_HOST="YOUR-RENDER-SERVICE.onrender.com";
const uint16_t WS_PORT=443;
const char* WS_PATH="/ws?role=device&token=change-me";

WebsocketsClient client;
unsigned long lastSample=0;
const unsigned long SAMPLE_INTERVAL=1000;

void connectWiFi(){
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  while(WiFi.status()!=WL_CONNECTED){delay(500);Serial.print(".");}
  Serial.println();
  Serial.println("WiFi connected");
  Serial.println(WiFi.localIP());
}

bool connectWebSocket(){
  Serial.println("Connecting to WebSocket...");
  bool ok=client.connect(WS_HOST,WS_PORT,WS_PATH);
  Serial.println(ok ? "WebSocket connected" : "WebSocket connection failed");
  return ok;
}

void setup(){
  Serial.begin(115200);
  delay(1000);
  randomSeed(analogRead(34));
  connectWiFi();
  client.setInsecure();
  connectWebSocket();
}

void loop(){
  client.poll();

  if(WiFi.status()!=WL_CONNECTED)connectWiFi();

  static unsigned long lastReconnect=0;
  if(!client.available() && millis()-lastReconnect>5000){
    lastReconnect=millis();
    connectWebSocket();
  }

  if(millis()-lastSample>=SAMPLE_INTERVAL){
    lastSample=millis();

    int rpm=random(800,4001);
    float speed=(rpm-800)*0.012+random(-20,21)/10.0;
    speed=constrain(speed,0,50);

    float temperature=random(250,351)/10.0;

    float engineTemp=65.0+((rpm-800)/3200.0)*30.0+random(-20,21)/10.0;
    engineTemp=constrain(engineTemp,60.0,100.0);

    float vibration=0.15+(rpm/4000.0)*0.25+random(-10,11)/100.0;
    vibration=constrain(vibration,0.05,1.0);

    float batteryVoltage=13.8-(rpm/4000.0)*0.8+random(-10,11)/100.0;
    batteryVoltage=constrain(batteryVoltage,11.5,14.5);

    float batteryCurrent=1.5+(rpm/4000.0)*5.0+random(-10,11)/10.0;
    batteryCurrent=constrain(batteryCurrent,0.5,10.0);

    float humidity=random(400,801)/10.0;

    String json="{";
    json+="\"type\":\"sensor\",";
    json+="\"time\":"+String(millis()/1000.0,1)+",";
    json+="\"temperature\":"+String(temperature,2)+",";
    json+="\"vibration\":"+String(vibration,3)+",";
    json+="\"batteryVoltage\":"+String(batteryVoltage,2)+",";
    json+="\"engineTemp\":"+String(engineTemp,2)+",";
    json+="\"rpm\":"+String(rpm)+",";
    json+="\"batteryCurrent\":"+String(batteryCurrent,2)+",";
    json+="\"speed\":"+String(speed,2)+",";
    json+="\"humidity\":"+String(humidity,2);
    json+="}";

    Serial.println(json);
    if(client.available())client.send(json);
  }
  delay(5);
}
