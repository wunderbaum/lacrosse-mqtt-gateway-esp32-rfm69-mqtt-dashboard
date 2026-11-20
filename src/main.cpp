#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <driver/gpio.h>
#include <vector>
#include <ctype.h>
#include "LaCrosseDecoder.h"
#include "favicon.h"

#ifndef RADIO_PIN_SCK
#define RADIO_PIN_SCK 18
#define RADIO_PIN_MISO 19
#define RADIO_PIN_MOSI 23
#define RADIO_PIN_NSS 5
#define RADIO_PIN_RESET 27
#define RADIO_PIN_DIO0 26
#endif

namespace {
constexpr uint32_t RF_FREQUENCY_KHZ=868300, RF_BITRATE=17241, RF_DEVIATION_HZ=90000;
constexpr uint8_t RF_FRAME_SIZE=5;
constexpr size_t HISTORY_LIMIT=48;
constexpr uint32_t BATTERY_CHANGE_WINDOW_MS=30000;
constexpr size_t BATTERY_CHANGE_CANDIDATE_LIMIT=8;
const char *AP_SSID="LaCrosse-Gateway";
const char *AP_PASSWORD="lacrosse868";

WebServer web(80);
WiFiClient net;
PubSubClient mqtt(net);
Preferences prefs;
struct Settings {
  String ssid,wifiPassword,mqttHost,mqttUser,mqttPassword,otaPassword,baseTopic="lacrosse";
  uint16_t mqttPort=1883;
  uint16_t rxBandwidthKhz=250;
  uint8_t pinSck=RADIO_PIN_SCK,pinMiso=RADIO_PIN_MISO,pinMosi=RADIO_PIN_MOSI;
  uint8_t pinNss=RADIO_PIN_NSS,pinReset=RADIO_PIN_RESET,pinDio0=RADIO_PIN_DIO0;
} cfg;
struct Sensor { uint16_t id=0; uint8_t radioId=0; String name; bool enabled=true,hasReading=false,hasHumidity=false,lowBattery=false,newBattery=false; float temperature=0; uint8_t humidity=0; int rssi=0; uint32_t updated=0; };
struct Sample { uint16_t id; uint8_t humidity; bool hasHumidity; float temperature; uint32_t at; };
struct ReplacementCandidate { LaCrosseReading reading; int rssi=0; };
struct BatteryChange { bool active=false,listening=false,candidateLimitReached=false; uint16_t sensorId=0; uint32_t expiresAt=0; std::vector<ReplacementCandidate> candidates; };
std::vector<Sensor> sensors;
std::vector<Sample> samples;
BatteryChange batteryChange;
uint32_t lastMqttTry=0;
bool usingAp=false;
bool otaUploadAuthorized=false,otaUploadActive=false,otaUploadSucceeded=false;
String otaUploadError;
bool rfm69Connected=false;

uint8_t readReg(uint8_t a){digitalWrite(cfg.pinNss,LOW);SPI.transfer(a&0x7f);uint8_t v=SPI.transfer(0);digitalWrite(cfg.pinNss,HIGH);return v;}
void writeReg(uint8_t a,uint8_t v){digitalWrite(cfg.pinNss,LOW);SPI.transfer(a|0x80);SPI.transfer(v);digitalWrite(cfg.pinNss,HIGH);}
void radioMode(uint8_t m){writeReg(0x01,(readReg(0x01)&0xe3)|(m&0x1c));}
void setFrequency(uint32_t khz){uint32_t frf=(uint64_t(khz)*1000ULL*524288ULL)/32000000ULL;writeReg(0x07,frf>>16);writeReg(0x08,frf>>8);writeReg(0x09,frf);}
bool isValidRxBandwidth(int khz){return khz==125||khz==200||khz==250;}
uint8_t rxBandwidthRegister(uint16_t khz){switch(khz){case 125:return 0x42;case 200:return 0x49;case 250:return 0x41;default:return 0x41;}}
bool radioBegin(){
  pinMode(cfg.pinNss,OUTPUT);digitalWrite(cfg.pinNss,HIGH);pinMode(cfg.pinReset,OUTPUT);
  pinMode(cfg.pinDio0,INPUT);
  digitalWrite(cfg.pinReset,HIGH);delay(1);digitalWrite(cfg.pinReset,LOW);delay(10);
  SPI.begin(cfg.pinSck,cfg.pinMiso,cfg.pinMosi,cfg.pinNss);
  uint8_t version=readReg(0x10);Serial.printf("RFM69 version register: 0x%02X\n",version);
  if(version!=0x24){Serial.println("RFM69CW not detected; expected version 0x24. Verify wiring and power.");return false;}
  radioMode(0x04);delay(2);
  writeReg(0x02,0x00); // FSK, packet mode
  uint16_t br=(32000000UL+RF_BITRATE/2)/RF_BITRATE;writeReg(0x03,br>>8);writeReg(0x04,br);
  uint16_t fd=(uint64_t(RF_DEVIATION_HZ)*524288ULL)/32000000ULL;writeReg(0x05,fd>>8);writeReg(0x06,fd);
  setFrequency(RF_FREQUENCY_KHZ);
  writeReg(0x18,0x88);writeReg(0x19,rxBandwidthRegister(cfg.rxBandwidthKhz));writeReg(0x29,220);
  writeReg(0x2e,0x88);writeReg(0x2f,0x2d);writeReg(0x30,0xd4);
  writeReg(0x37,0x00); // fixed-length packet, hardware CRC disabled
  writeReg(0x38,RF_FRAME_SIZE);
  writeReg(0x3c,0x8f);writeReg(0x3d,0x12);writeReg(0x6f,0x30);
  writeReg(0x28,0x10);radioMode(0x10);
  Serial.printf("RFM69 receive profile: %.3f MHz, %lu bit/s, RX bandwidth %u kHz, fixed %u-byte frame\n",RF_FREQUENCY_KHZ/1000.0,(unsigned long)RF_BITRATE,cfg.rxBandwidthKhz,RF_FRAME_SIZE);
  return true;
}
String slug(String s){s.trim();s.toLowerCase();String out;bool dash=false;for(size_t i=0;i<s.length();++i){char c=s[i];if(isalnum((unsigned char)c)){out+=c;dash=false;}else if(!dash&&out.length()){out+='-';dash=true;}}while(out.endsWith("-"))out.remove(out.length()-1);return out.length()?out:"sensor";}
Sensor *findSensor(uint16_t id){for(auto &s:sensors)if(s.id==id)return &s;return nullptr;}
Sensor *findSensorByRadioId(uint8_t id){for(auto &s:sensors)if(s.radioId==id)return &s;return nullptr;}
bool allocateSensorId(uint16_t &id){for(uint32_t candidate=0;candidate<=UINT16_MAX;++candidate)if(!findSensor(static_cast<uint16_t>(candidate))){id=static_cast<uint16_t>(candidate);return true;}return false;}
void resetBatteryChange(){batteryChange.active=false;batteryChange.listening=false;batteryChange.candidateLimitReached=false;batteryChange.sensorId=0;batteryChange.expiresAt=0;batteryChange.candidates.clear();}
void updateBatteryChangeWindow(){if(batteryChange.active&&batteryChange.listening&&static_cast<int32_t>(millis()-batteryChange.expiresAt)>=0){batteryChange.listening=false;Serial.printf("Battery-change listening window ended for sensor %u; %u candidate(s) found\n",static_cast<unsigned>(batteryChange.sensorId),static_cast<unsigned>(batteryChange.candidates.size()));}}
ReplacementCandidate *findReplacementCandidate(uint8_t radioId){for(auto &candidate:batteryChange.candidates)if(candidate.reading.id==radioId)return &candidate;return nullptr;}
void observeReplacementCandidate(const LaCrosseReading&r,int rssi){ReplacementCandidate*c=findReplacementCandidate(r.id);if(!c){if(batteryChange.candidates.size()>=BATTERY_CHANGE_CANDIDATE_LIMIT){if(!batteryChange.candidateLimitReached)Serial.println("Battery-change candidate limit reached; additional IDs are not tracked.");batteryChange.candidateLimitReached=true;return;}batteryChange.candidates.push_back({});c=&batteryChange.candidates.back();}c->reading=r;c->rssi=rssi;}
String stateTopic(const Sensor&s){return cfg.baseTopic+"/sensor/"+slug(s.name)+"/state";}
String availabilityTopic(){return cfg.baseTopic+"/status";}
void saveSettings(){prefs.begin("gateway",false);prefs.putString("ssid",cfg.ssid);prefs.putString("wifi",cfg.wifiPassword);prefs.putString("host",cfg.mqttHost);prefs.putString("user",cfg.mqttUser);prefs.putString("pass",cfg.mqttPassword);prefs.putString("ota",cfg.otaPassword);prefs.putString("topic",cfg.baseTopic);prefs.putUShort("port",cfg.mqttPort);prefs.putUShort("rxBw",cfg.rxBandwidthKhz);prefs.putUChar("pinSck",cfg.pinSck);prefs.putUChar("pinMiso",cfg.pinMiso);prefs.putUChar("pinMosi",cfg.pinMosi);prefs.putUChar("pinNss",cfg.pinNss);prefs.putUChar("pinReset",cfg.pinReset);prefs.putUChar("pinDio0",cfg.pinDio0);prefs.end();}
void loadSettings(){prefs.begin("gateway",true);cfg.ssid=prefs.getString("ssid","");cfg.wifiPassword=prefs.getString("wifi","");cfg.mqttHost=prefs.getString("host","");cfg.mqttUser=prefs.getString("user","");cfg.mqttPassword=prefs.getString("pass","");cfg.otaPassword=prefs.getString("ota","");cfg.baseTopic=prefs.getString("topic","lacrosse");cfg.mqttPort=prefs.getUShort("port",1883);cfg.rxBandwidthKhz=prefs.getUShort("rxBw",250);cfg.pinSck=prefs.getUChar("pinSck",RADIO_PIN_SCK);cfg.pinMiso=prefs.getUChar("pinMiso",RADIO_PIN_MISO);cfg.pinMosi=prefs.getUChar("pinMosi",RADIO_PIN_MOSI);cfg.pinNss=prefs.getUChar("pinNss",RADIO_PIN_NSS);cfg.pinReset=prefs.getUChar("pinReset",RADIO_PIN_RESET);cfg.pinDio0=prefs.getUChar("pinDio0",RADIO_PIN_DIO0);prefs.end();if(!isValidRxBandwidth(cfg.rxBandwidthKhz)){Serial.printf("Invalid saved RX bandwidth %u kHz; using 250 kHz\n",cfg.rxBandwidthKhz);cfg.rxBandwidthKhz=250;}}
void saveSensors(){JsonDocument d;JsonArray a=d.to<JsonArray>();for(const auto&s:sensors){JsonObject o=a.add<JsonObject>();o["id"]=s.id;o["radioId"]=s.radioId;o["name"]=s.name;o["enabled"]=s.enabled;}String text;serializeJson(d,text);prefs.begin("sensors",false);prefs.putString("list",text);prefs.end();}
void loadSensors(){prefs.begin("sensors",true);String text=prefs.getString("list","[]");prefs.end();JsonDocument d;if(deserializeJson(d,text))return;for(JsonObject o:d.as<JsonArray>()){Sensor s;s.id=o["id"]|0;s.radioId=o["radioId"]|static_cast<uint8_t>(s.id);s.name=String((const char*)(o["name"]|""));s.enabled=o["enabled"]|true;sensors.push_back(s);}}
String discoveryTopic(uint16_t id,const char *entity){return String("homeassistant/")+(strcmp(entity,"battery")==0?"binary_sensor":"sensor")+"/lacrosse_"+String(id)+"_"+entity+"/config";}
void publishDiscovery(const Sensor&s,const char *entity){
  if(!mqtt.connected())return;
  String topic=discoveryTopic(s.id,entity);
  if(strcmp(entity,"humidity")==0&&!s.hasHumidity){mqtt.publish(topic.c_str(),"",true);return;}
  if(!s.enabled){mqtt.publish(topic.c_str(),"",true);return;}
  JsonDocument d;String friendly=s.name+" "+String(entity);d["name"]=friendly;d["unique_id"]="lacrosse_"+String(s.id)+"_"+entity;d["state_topic"]=stateTopic(s);d["availability_topic"]=availabilityTopic();d["payload_available"]="online";d["payload_not_available"]="offline";d["json_attributes_topic"]=stateTopic(s);
  if(strcmp(entity,"temperature")==0){d["value_template"]="{{ value_json.temperature }}";d["unit_of_measurement"]="°C";d["device_class"]="temperature";d["state_class"]="measurement";}
  else if(strcmp(entity,"humidity")==0){d["value_template"]="{{ value_json.humidity }}";d["unit_of_measurement"]="%";d["device_class"]="humidity";d["state_class"]="measurement";}
  else {d["value_template"]="{{ 'ON' if value_json.battery_low else 'OFF' }}";d["payload_on"]="ON";d["payload_off"]="OFF";d["device_class"]="battery";}
  JsonObject dev=d["device"].to<JsonObject>();dev["identifiers"][0]="lacrosse_"+String(s.id);dev["name"]=s.name;dev["manufacturer"]="Technoline / LaCrosse-compatible";dev["model"]=s.hasHumidity?"868 MHz temperature/humidity sensor":"868 MHz temperature sensor";
  String payload;serializeJson(d,payload);if(!mqtt.publish(topic.c_str(),payload.c_str(),true))Serial.printf("Discovery publish failed: %s\n",topic.c_str());
}
void announceSensor(const Sensor&s){publishDiscovery(s,"temperature");publishDiscovery(s,"humidity");publishDiscovery(s,"battery");}
void publishReading(const Sensor&s){if(!mqtt.connected()||!s.enabled||!s.hasReading)return;JsonDocument d;d["id"]=s.radioId;d["logical_id"]=s.id;d["temperature"]=s.temperature;d["has_humidity"]=s.hasHumidity;if(s.hasHumidity)d["humidity"]=s.humidity;d["battery_low"]=s.lowBattery;d["new_battery"]=s.newBattery;d["rssi"]=s.rssi;d["updated"]=s.updated;String payload;serializeJson(d,payload);String topic=stateTopic(s);mqtt.publish(topic.c_str(),payload.c_str(),true);}
void applyReading(Sensor&s,const LaCrosseReading&r,int rssi){bool announce=!s.hasReading||s.hasHumidity!=r.hasHumidity;s.temperature=r.temperature;s.humidity=r.humidity;s.hasHumidity=r.hasHumidity;s.lowBattery=r.lowBattery;s.newBattery=r.newBattery;s.rssi=rssi;s.updated=millis();s.hasReading=true;if(announce&&mqtt.connected())announceSensor(s);samples.push_back({s.id,s.humidity,s.hasHumidity,s.temperature,s.updated});if(samples.size()>HISTORY_LIMIT)samples.erase(samples.begin());publishReading(s);}
void updateSensor(const LaCrosseReading&r){
  int rssi=-(int(readReg(0x24))/2);
  if(Sensor*s=findSensorByRadioId(r.id)){applyReading(*s,r,rssi);return;}
  updateBatteryChangeWindow();
  if(batteryChange.active){
    if(ReplacementCandidate*c=findReplacementCandidate(r.id)){c->reading=r;c->rssi=rssi;return;}
    if(batteryChange.listening){observeReplacementCandidate(r,rssi);return;}
  }
  uint16_t id;
  if(!allocateSensorId(id)){Serial.println("Sensor identity space exhausted; ignoring RF frame.");return;}
  Sensor fresh;fresh.id=id;fresh.radioId=r.id;fresh.name="Sensor "+String(r.id);sensors.push_back(fresh);Sensor*s=&sensors.back();saveSensors();Serial.printf("Discovered sensor RF ID %u (logical ID %u)\n",r.id,static_cast<unsigned>(s->id));applyReading(*s,r,rssi);
}
void radioPoll(){if(!rfm69Connected||!(readReg(0x28)&0x04))return;uint8_t bytes[RF_FRAME_SIZE];digitalWrite(cfg.pinNss,LOW);SPI.transfer(0x00);for(auto &b:bytes)b=SPI.transfer(0);digitalWrite(cfg.pinNss,HIGH);writeReg(0x28,0x10);writeReg(0x3d,0x04);LaCrosseReading r;if(decodeLaCrosseFrame(bytes,r)){if(r.hasHumidity)Serial.printf("Frame id=%u temp=%.1f humidity=%u\n",r.id,r.temperature,r.humidity);else Serial.printf("Frame id=%u temp=%.1f no humidity sensor\n",r.id,r.temperature);updateSensor(r);}else{Serial.print("Rejected RF bytes:");for(uint8_t b:bytes)Serial.printf(" %02X",b);Serial.println();}}
void onMqttConnected(){mqtt.publish(availabilityTopic().c_str(),"online",true);for(const auto&s:sensors){announceSensor(s);publishReading(s);}Serial.println("MQTT connected; discovery announced.");}
void mqttReconnect(){if(cfg.mqttHost.isEmpty()||mqtt.connected()||millis()-lastMqttTry<5000)return;lastMqttTry=millis();mqtt.setServer(cfg.mqttHost.c_str(),cfg.mqttPort);mqtt.setBufferSize(1024);String client="lacrosse-"+String((uint32_t)ESP.getEfuseMac(),HEX);String will=availabilityTopic();bool ok=cfg.mqttUser.isEmpty()?mqtt.connect(client.c_str(),will.c_str(),0,true,"offline"):mqtt.connect(client.c_str(),cfg.mqttUser.c_str(),cfg.mqttPassword.c_str(),will.c_str(),0,true,"offline");if(ok)onMqttConnected();else Serial.printf("MQTT connect failed, state=%d\n",mqtt.state());}

const char PAGE[] PROGMEM=R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<link rel="icon" href="/favicon.ico" type="image/x-icon">
<title>LaCrosse Gateway</title>
<style>
body{font:16px system-ui;max-width:900px;margin:2rem auto;padding:0 1rem;background:#111827;color:#eee}
section{background:#1f2937;padding:1rem;border-radius:12px;margin:1rem 0}
table{width:100%;border-collapse:collapse}
td,th{text-align:left;padding:.7rem;border-bottom:1px solid #374151}
input[type=text],input[type=password],input[type=number]{padding:.55rem;border-radius:6px;border:1px solid #4b5563;background:#111827;color:white}
button{padding:.65rem 1rem;border:0;border-radius:7px;background:#34d399;font-weight:bold;cursor:pointer}
.muted{color:#9ca3af}.nav{display:flex;gap:.5rem}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:.8rem}
.card{min-width:0;padding:1rem;background:#374151;border-radius:10px}
.card-heading,.reading,.chart-title{display:flex;align-items:baseline;justify-content:space-between;gap:.6rem}
.reading{justify-content:flex-start;margin:.45rem 0}
.reading-temp{font-size:1.55rem;font-weight:650}
.reading-humidity{color:#60a5fa;font-size:1.15rem;font-weight:600}
.chart-panel{margin-top:.65rem}
.chart-title{font-size:.85rem;color:#d1d5db}
.chart{display:block;width:100%;height:auto}
.chart-grid{stroke:#4b5563;stroke-width:1;stroke-dasharray:3 4}
.chart-axis{fill:#9ca3af;font-size:11px}
.chart-empty{margin:.65rem 0 0;color:#9ca3af;font-size:.9rem}
.battery-change{margin-top:.8rem}
.battery-change button.secondary{background:#60a5fa}
.replacement-panel{margin-top:.8rem;padding:.7rem;background:#1f2937;border-radius:8px}
.replacement-candidate{display:flex;align-items:center;justify-content:space-between;gap:.5rem;margin:.5rem 0}
button:disabled{opacity:.55;cursor:not-allowed}
@media(max-width:600px){.cards{grid-template-columns:1fr}}
</style>
</head>
<body>
<h1>LaCrosse Gateway</h1>
<p id="status" class="muted">Connecting</p>
<nav class="nav">
<button onclick="showSection('dashboard')">Dashboard</button>
<button onclick="showSection('setup')">Sensor setup</button>
<button onclick="loadSettings()">Network / MQTT</button>
<button onclick="showSection('ota')">Firmware update</button>
</nav>
<section id="dashboard"><h2>Live readings</h2><div id="cards" class="cards"></div></section>
<section id="setup" hidden>
<h2>Sensors</h2>
<p class="muted">Detected IDs appear automatically. Rename sensors or switch MQTT publishing on/off.</p>
<table><thead><tr><th>RF ID</th><th>Name</th><th>MQTT</th><th>Last seen</th></tr></thead><tbody id="rows"></tbody></table>
<p><button onclick="saveSensors()">Save sensor settings</button></p>
</section>
<section id="settings" hidden>
<h2>Network and MQTT</h2>
<form id="settingsForm">
<p>Wi-Fi SSID<br><input name="ssid"></p>
<p>Wi-Fi password<br><input name="wifiPassword" type="password" placeholder="Blank keeps saved password"></p>
<p>MQTT host<br><input name="mqttHost"></p>
<p>MQTT port<br><input name="mqttPort" type="number" value="1883"></p>
<p>MQTT username<br><input name="mqttUser"></p>
<p>MQTT password<br><input name="mqttPassword" type="password" placeholder="Blank keeps saved password"></p>
<p>OTA update password (minimum 8 characters)<br><input name="otaPassword" type="password" minlength="8" autocomplete="new-password" placeholder="Blank keeps saved password"></p>
<p>Base topic<br><input name="baseTopic" value="lacrosse"></p>
<h3>RFM69 GPIO pins</h3>
<p class="muted">Choose distinct GPIOs supported by your ESP32 board. Changes apply after reboot.</p>
<h3>RFM69 receive bandwidth</h3>
<p>Receive filter bandwidth<br><select name="rxBandwidthKhz" required><option value="250">250 kHz (recommended)</option><option value="200">200 kHz</option><option value="125">125 kHz (narrow)</option></select></p>
<p class="muted">Wider bandwidth can help receive weak or slightly off-frequency signals, but also admits more interference. Start at 250 kHz; if reception is noisy, try 200 kHz. Changes apply after reboot.</p>
<p>SCK<br><input name="pinSck" type="number" min="0" required></p>
<p>MISO<br><input name="pinMiso" type="number" min="0" required></p>
<p>MOSI<br><input name="pinMosi" type="number" min="0" required></p>
<p>NSS / CS<br><input name="pinNss" type="number" min="0" required></p>
<p>RESET<br><input name="pinReset" type="number" min="0" required></p>
<p>DIO0<br><input name="pinDio0" type="number" min="0" required></p>
<button>Save and reboot</button>
</form>
</section>
<section id="ota" hidden>
<h2>Firmware update</h2>
<p class="muted">Set an OTA update password in Network / MQTT before uploading firmware. Choose the firmware <code>.bin</code> produced by PlatformIO.</p>
<form id="otaForm">
<p>Firmware file<br><input id="firmwareFile" type="file" accept=".bin,application/octet-stream" required></p>
<p>OTA update password<br><input id="otaUploadPassword" type="password" autocomplete="current-password" required></p>
<button>Upload firmware and reboot</button>
</form>
</section>
<script>
const byId=id=>document.getElementById(id);
let data=[];
function esc(x){return String(x).replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',"'":'&#39;'}[c]))}
function showSection(id){for(const section of ['dashboard','setup','settings','ota'])byId(section).hidden=section!==id}
function formatSpan(ms){
  const minutes=Math.round(ms/60000);
  if(minutes<60)return `${minutes}m`;
  const hours=Math.round(minutes/60);
  if(hours<24)return `${hours}h`;
  return `${Math.round(hours/24)}d`;
}
function chartFor(sensor,key,label,unit,color){
  const points=(sensor.history||[]).filter(point=>Number.isFinite(point[key]));
  if(!points.length)return '';
  const values=points.map(point=>point[key]);
  let low,high,ticks;
  if(key==='humidity'){
    low=0;high=100;ticks=[100,50,0];
  }else{
    const min=Math.min(...values),max=Math.max(...values);
    const padding=max===min?1:Math.max((max-min)*.15,.2);
    low=Math.floor((min-padding)*2)/2;
    high=Math.ceil((max+padding)*2)/2;
    if(high<=low)high=low+1;
    ticks=[high,(high+low)/2,low];
  }
  const left=38,right=350,top=12,bottom=78;
  const first=points[0].at>>>0,last=points[points.length-1].at>>>0;
  const span=(last-first+4294967296)%4294967296;
  const coordinates=points.map((point,index)=>{
    const elapsed=((point.at>>>0)-first+4294967296)%4294967296;
    const x=span?left+elapsed/span*(right-left):right;
    const y=bottom-(point[key]-low)/(high-low)*(bottom-top);
    return {x,y,index};
  });
  const path=coordinates.map((point,index)=>`${index?'L':'M'}${point.x.toFixed(1)},${point.y.toFixed(1)}`).join(' ');
  const grid=ticks.map(value=>{
    const y=bottom-(value-low)/(high-low)*(bottom-top);
    const text=key==='humidity'?value.toFixed(0):value.toFixed(1);
    return `<line class="chart-grid" x1="${left}" y1="${y.toFixed(1)}" x2="${right}" y2="${y.toFixed(1)}"></line><text class="chart-axis" x="${left-6}" y="${(y+4).toFixed(1)}" text-anchor="end">${text}</text>`;
  }).join('');
  const latest=coordinates[coordinates.length-1];
  return `<div class="chart-panel"><div class="chart-title"><span>${label}</span><span>${values[values.length-1].toFixed(1)} ${unit}</span></div><svg class="chart" viewBox="0 0 360 104" role="img" aria-label="${label} history"><title>${label} history</title>${grid}<path d="${path}" fill="none" stroke="${color}" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"></path><circle cx="${latest.x.toFixed(1)}" cy="${latest.y.toFixed(1)}" r="3.5" fill="${color}"></circle><text class="chart-axis" x="${left}" y="99">${span?'-'+formatSpan(span):'now'}</text><text class="chart-axis" x="${right}" y="99" text-anchor="end">now</text></svg></div>`;
}
function chartsFor(sensor){
  if(!sensor.hasReading)return '<p class="chart-empty">Waiting for the first reading</p>';
  const temperature=chartFor(sensor,'temperature','Temperature','°C','#34d399');
  const humidity=sensor.hasHumidity?chartFor(sensor,'humidity','Humidity','%','#60a5fa'):'';
  return temperature||humidity?temperature+humidity:'<p class="chart-empty">No history yet</p>';
}
function batteryChangeControls(sensor,change){
  if(change?.active&&change.sensorId===sensor.id){
    const candidates=change.candidates.map(candidate=>`<div class="replacement-candidate"><span>New RF ID ${candidate.radioId} · ${Number(candidate.temperature).toFixed(1)} °C${candidate.hasHumidity?' · '+candidate.humidity+'%':''} · ${candidate.rssi} dBm</span><button type="button" class="secondary" ${change.listening?'disabled':''} onclick="confirmBatteryChange(${sensor.id},${candidate.radioId})">Assign</button></div>`).join('');
    const status=change.listening
      ? `Replace the battery now. Listening for new IDs: ${change.secondsRemaining}s left. You can confirm after the window closes.`
      : change.candidates.length?'Window closed. Confirm the replacement ID.':'No new ID appeared in 30 seconds.';
    const limit=change.candidateLimitReached?'<p class="chart-empty">Too many new IDs appeared; only the first 8 candidates are listed.</p>':'';
    const closeLabel=change.listening||change.candidates.length?'Cancel':'Close';
    const empty=change.listening?'<p class="chart-empty">Waiting for a new sensor ID.</p>':'';
    return `<div class="replacement-panel"><p>${status}</p>${candidates||empty}${limit}<button type="button" onclick="cancelBatteryChange()">${closeLabel}</button></div>`;
  }
  if(change?.active)return '<div class="battery-change"><button type="button" class="secondary" disabled>Another battery change is in progress</button></div>';
  return `<div class="battery-change"><button type="button" class="secondary" onclick="startBatteryChange(${sensor.id})">Battery change</button></div>`;
}
function updateSensorRows(sensors){
  const rows=byId('rows');
  for(const sensor of sensors){
    let row=rows.querySelector(`tr[data-id="${sensor.id}"]`);
    if(!row){
      row=document.createElement('tr');
      row.dataset.id=sensor.id;
      const idCell=row.insertCell();
      idCell.textContent=sensor.radioId;
      const nameCell=row.insertCell();
      const nameInput=document.createElement('input');
      nameInput.type='text';
      nameInput.maxLength=40;
      nameInput.value=sensor.name;
      nameCell.appendChild(nameInput);
      const enabledCell=row.insertCell();
      const enabledInput=document.createElement('input');
      enabledInput.type='checkbox';
      enabledInput.checked=sensor.enabled;
      enabledCell.appendChild(enabledInput);
      row.insertCell();
      rows.appendChild(row);
    }
    row.cells[0].textContent=sensor.radioId;
    row.cells[3].textContent=sensor.hasReading?sensor.age+'s ago':'Not received';
  }
}
async function refresh(){
  try{
    const response=await fetch('/api/state');
    if(!response.ok)throw new Error(`State request failed: ${response.status}`);
    const d=await response.json();
    data=d.sensors;
    byId('status').textContent=(d.wifi?'Wi-Fi connected':'Setup AP')+' · '+(d.rfm69Connected?'RFM69CW connected':'RFM69CW not detected');
    byId('cards').innerHTML=data.map(s=>`<article class="card"><div class="card-heading"><b>${esc(s.name)}</b><small>RF ID ${s.radioId}</small></div><div class="reading"><span class="reading-temp">${s.hasReading?Number(s.temperature).toFixed(1)+' °C':'Waiting'}</span><span class="reading-humidity">${s.hasReading?(s.hasHumidity?s.humidity+'%':'No humidity sensor'):'--'}</span></div><small>${s.lowBattery?'Low battery · ':''}${s.hasReading?'RSSI '+s.rssi+' dBm · ':''}${s.age}s ago</small>${chartsFor(s)}${batteryChangeControls(s,d.batteryChange)}</article>`).join('')||'No sensors received yet';
    updateSensorRows(data);
  }catch(e){byId('status').textContent='Gateway unavailable';console.error(e)}
}
async function postJson(url,payload){
  const response=await fetch(url,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});
  if(!response.ok)throw new Error(await response.text()||`Request failed: ${response.status}`);
}
async function startBatteryChange(sensorId){
  try{
    await postJson('/api/battery-change',{sensorId});
    await refresh();
  }catch(error){
    console.error('Could not start battery-change window',error);
    alert(`Could not start battery-change window: ${error.message}`);
  }
}
async function confirmBatteryChange(sensorId,radioId){
  try{
    await postJson('/api/battery-change/confirm',{sensorId,radioId});
    await refresh();
    alert(`Sensor reassigned to RF ID ${radioId}. Its name, MQTT topic, and Home Assistant entities are unchanged.`);
  }catch(error){
    console.error('Could not assign replacement sensor ID',error);
    alert(`Could not assign replacement sensor ID: ${error.message}`);
  }
}
async function cancelBatteryChange(){
  try{
    await postJson('/api/battery-change/cancel',{});
    await refresh();
  }catch(error){
    console.error('Could not close battery-change window',error);
    alert(`Could not close battery-change window: ${error.message}`);
  }
}
async function saveSensors(){
  for(const r of byId('rows').querySelectorAll('tr')){
    const name=r.querySelector('input[type=text]').value.trim();
    if(!name){alert('Sensor name cannot be empty');return}
    const response=await fetch('/api/sensor',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id:Number(r.dataset.id),name,enabled:r.querySelector('[type=checkbox]').checked})});
    if(!response.ok){alert('Save failed');return}
  }
  refresh();
  alert('Saved');
}
async function loadSettings(){
  try{
    const response=await fetch('/api/settings');
    if(!response.ok)throw new Error(`Settings request failed: ${response.status}`);
    const settingsData=await response.json();
    const form=byId('settingsForm');
    for(const [key,value] of Object.entries(settingsData)){
      if(form.elements[key])form.elements[key].value=value??'';
    }
    showSection('settings');
  }catch(error){
    console.error('Could not load network/MQTT settings',error);
    alert('Could not load network/MQTT settings. Check the gateway connection and try again.');
  }
}
byId('settingsForm').addEventListener('submit',async event=>{
  event.preventDefault();
  try{
    const form=byId('settingsForm');
    const payload=Object.fromEntries(new FormData(form));
    for(const key of ['mqttPort','rxBandwidthKhz','pinSck','pinMiso','pinMosi','pinNss','pinReset','pinDio0'])payload[key]=Number(payload[key]);
    const response=await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});
    if(!response.ok)throw new Error(await response.text()||`Settings save failed: ${response.status}`);
    alert('Saved; restarting');
  }catch(error){
    console.error('Could not save network/MQTT settings',error);
    alert(`Could not save settings: ${error.message}`);
  }
});
byId('otaForm').addEventListener('submit',async event=>{
  event.preventDefault();
  const file=byId('firmwareFile').files[0];
  if(!file){alert('Choose a firmware .bin file first');return}
  const password=byId('otaUploadPassword').value;
  const credentials=new TextEncoder().encode(`admin:${password}`);
  let binaryCredentials='';
  for(const byte of credentials)binaryCredentials+=String.fromCharCode(byte);
  const body=new FormData();
  body.append('firmware',file);
  try{
    const response=await fetch('/update',{method:'POST',headers:{Authorization:`Basic ${btoa(binaryCredentials)}`},body});
    const message=await response.text();
    if(!response.ok)throw new Error(message||`Firmware upload failed: ${response.status}`);
    alert(message||'Firmware uploaded; gateway is restarting.');
  }catch(error){
    console.error('Firmware update failed',error);
    alert(`Firmware update failed: ${error.message}`);
  }
});
setInterval(refresh,3000);
refresh();
</script>
</body>
</html>
)HTML";
void sendJson(JsonDocument&d){String out;serializeJson(d,out);web.send(200,"application/json",out);}
void handleOtaUpload(){
  HTTPUpload& upload=web.upload();
  switch(upload.status){
    case UPLOAD_FILE_START:{
      otaUploadAuthorized=!cfg.otaPassword.isEmpty()&&web.authenticate("admin",cfg.otaPassword.c_str());
      otaUploadActive=false;
      otaUploadSucceeded=false;
      otaUploadError="";
      if(cfg.otaPassword.isEmpty()){otaUploadError="OTA password is not configured";return;}
      if(!otaUploadAuthorized){otaUploadError="Invalid OTA password";return;}
      String filename=upload.filename;
      filename.toLowerCase();
      if(!filename.endsWith(".bin")){otaUploadError="Select a firmware .bin file";return;}
      if(!Update.begin(UPDATE_SIZE_UNKNOWN)){otaUploadError=String("Could not start firmware update: ")+Update.errorString();return;}
      otaUploadActive=true;
      break;
    }
    case UPLOAD_FILE_WRITE:
      if(!otaUploadActive)return;
      if(Update.write(upload.buf,upload.currentSize)!=upload.currentSize){
        otaUploadError=String("Firmware write failed: ")+Update.errorString();
        Update.abort();
        otaUploadActive=false;
      }
      break;
    case UPLOAD_FILE_END:
      if(!otaUploadActive)return;
      if(Update.end(true)){
        otaUploadSucceeded=true;
        Serial.printf("Firmware update uploaded: %u bytes\n",upload.totalSize);
      }else{
        otaUploadError=String("Firmware validation failed: ")+Update.errorString();
      }
      otaUploadActive=false;
      break;
    case UPLOAD_FILE_ABORTED:
      if(otaUploadActive)Update.abort();
      otaUploadError="Firmware upload aborted";
      otaUploadActive=false;
      break;
    default:
      break;
  }
}
void setupWeb(){
 web.on("/",HTTP_GET,[]{web.send_P(200,"text/html; charset=utf-8",PAGE);});
 web.on("/favicon.ico",HTTP_GET,[]{web.send_P(200,"image/x-icon",reinterpret_cast<PGM_P>(FAVICON_ICO),FAVICON_ICO_SIZE);});
 web.on("/api/state",HTTP_GET,[]{
   updateBatteryChangeWindow();
   JsonDocument d;
   d["wifi"]=WiFi.status()==WL_CONNECTED;d["rfm69Connected"]=rfm69Connected;
   JsonArray a=d["sensors"].to<JsonArray>();
   for(const auto&s:sensors){
     JsonObject o=a.add<JsonObject>();
     o["id"]=s.id;o["radioId"]=s.radioId;o["name"]=s.name;o["enabled"]=s.enabled;o["hasReading"]=s.hasReading;o["hasHumidity"]=s.hasHumidity;o["temperature"]=s.temperature;
     if(s.hasHumidity)o["humidity"]=s.humidity;
     o["lowBattery"]=s.lowBattery;o["rssi"]=s.rssi;o["age"]=s.hasReading?(millis()-s.updated)/1000:0;
     JsonArray h=o["history"].to<JsonArray>();
     for(const auto&p:samples)if(p.id==s.id){JsonObject q=h.add<JsonObject>();q["temperature"]=p.temperature;if(p.hasHumidity)q["humidity"]=p.humidity;q["at"]=p.at;}
   }
   JsonObject change=d["batteryChange"].to<JsonObject>();
   change["active"]=batteryChange.active;change["listening"]=batteryChange.listening;
   if(batteryChange.active){
     change["sensorId"]=batteryChange.sensorId;
     change["secondsRemaining"]=batteryChange.listening?(batteryChange.expiresAt-millis()+999)/1000:0;
     change["candidateLimitReached"]=batteryChange.candidateLimitReached;
     JsonArray candidates=change["candidates"].to<JsonArray>();
     for(const auto&candidate:batteryChange.candidates){
       JsonObject o=candidates.add<JsonObject>();
       o["radioId"]=candidate.reading.id;o["temperature"]=candidate.reading.temperature;o["hasHumidity"]=candidate.reading.hasHumidity;
       if(candidate.reading.hasHumidity)o["humidity"]=candidate.reading.humidity;
       o["rssi"]=candidate.rssi;
     }
   }
   sendJson(d);
 });
 web.on("/api/settings",HTTP_GET,[]{
   JsonDocument d;
   d["ssid"]=cfg.ssid;d["mqttHost"]=cfg.mqttHost;d["mqttPort"]=cfg.mqttPort;d["mqttUser"]=cfg.mqttUser;d["baseTopic"]=cfg.baseTopic;
   d["rxBandwidthKhz"]=cfg.rxBandwidthKhz;d["pinSck"]=cfg.pinSck;d["pinMiso"]=cfg.pinMiso;d["pinMosi"]=cfg.pinMosi;d["pinNss"]=cfg.pinNss;d["pinReset"]=cfg.pinReset;d["pinDio0"]=cfg.pinDio0;
   sendJson(d);
 });
 web.on("/api/settings",HTTP_POST,[]{
   JsonDocument d;
   if(deserializeJson(d,web.arg("plain"))){web.send(400,"text/plain","Invalid JSON");return;}
   const char *pinKeys[]={"pinSck","pinMiso","pinMosi","pinNss","pinReset","pinDio0"};
   uint8_t pins[6];
   if(!d["rxBandwidthKhz"].is<int>()||!isValidRxBandwidth(d["rxBandwidthKhz"].as<int>())){web.send(400,"text/plain","RX bandwidth must be 125, 200, or 250 kHz");return;}
   for(size_t i=0;i<6;++i){
     if(!d[pinKeys[i]].is<int>()){web.send(400,"text/plain","All GPIO pin values must be integers");return;}
     int pin=d[pinKeys[i]].as<int>();
     bool outputPin=i==0||i==2||i==3||i==4;
     if(!GPIO_IS_VALID_GPIO(pin)||(outputPin&&!GPIO_IS_VALID_OUTPUT_GPIO(pin))){web.send(400,"text/plain","GPIO is not available for that RFM69 signal on this ESP32 board");return;}
     for(size_t j=0;j<i;++j)if(pin==pins[j]){web.send(400,"text/plain","RFM69 GPIO pins must be distinct");return;}
     pins[i]=static_cast<uint8_t>(pin);
   }
   cfg.ssid=d["ssid"]|"";String wp=d["wifiPassword"]|"";if(wp.length())cfg.wifiPassword=wp;
   cfg.mqttHost=d["mqttHost"]|"";cfg.mqttPort=d["mqttPort"]|1883;cfg.mqttUser=d["mqttUser"]|"";String mp=d["mqttPassword"]|"";if(mp.length())cfg.mqttPassword=mp;
   String ota=d["otaPassword"]|"";if(ota.length()&&ota.length()<8){web.send(400,"text/plain","OTA password must be at least 8 characters");return;}if(ota.length())cfg.otaPassword=ota;
   cfg.baseTopic=d["baseTopic"]|"lacrosse";cfg.baseTopic.trim();if(!cfg.baseTopic.length())cfg.baseTopic="lacrosse";
   cfg.rxBandwidthKhz=d["rxBandwidthKhz"].as<uint16_t>();
   cfg.pinSck=pins[0];cfg.pinMiso=pins[1];cfg.pinMosi=pins[2];cfg.pinNss=pins[3];cfg.pinReset=pins[4];cfg.pinDio0=pins[5];
   saveSettings();web.send(200,"text/plain","Saved; restarting");delay(500);ESP.restart();
 });
 web.on("/api/battery-change",HTTP_POST,[]{
   JsonDocument d;
   if(deserializeJson(d,web.arg("plain"))){web.send(400,"text/plain","Invalid JSON");return;}
   if(!d["sensorId"].is<int>()){web.send(400,"text/plain","Sensor ID must be an integer");return;}
   int requested=d["sensorId"].as<int>();
   if(requested<0||requested>65535){web.send(400,"text/plain","Sensor ID is out of range");return;}
   if(!findSensor(static_cast<uint16_t>(requested))){web.send(404,"text/plain","Unknown sensor");return;}
   updateBatteryChangeWindow();
   if(batteryChange.active){web.send(409,"text/plain","Another battery-change operation is already active");return;}
   resetBatteryChange();
   batteryChange.active=true;batteryChange.listening=true;batteryChange.sensorId=static_cast<uint16_t>(requested);batteryChange.expiresAt=millis()+BATTERY_CHANGE_WINDOW_MS;
   Serial.printf("Battery-change listening window opened for sensor %u\n",static_cast<unsigned>(batteryChange.sensorId));
   web.send(200,"text/plain","Listening for a replacement RF ID for 30 seconds");
 });
 web.on("/api/battery-change/confirm",HTTP_POST,[]{
   JsonDocument d;
   if(deserializeJson(d,web.arg("plain"))){web.send(400,"text/plain","Invalid JSON");return;}
   if(!d["sensorId"].is<int>()||!d["radioId"].is<int>()){web.send(400,"text/plain","Sensor ID and radio ID must be integers");return;}
   int requested=d["sensorId"].as<int>(),radioId=d["radioId"].as<int>();
   if(requested<0||requested>65535||radioId<0||radioId>255){web.send(400,"text/plain","Sensor ID or radio ID is out of range");return;}
   updateBatteryChangeWindow();
   if(!batteryChange.active||batteryChange.sensorId!=requested){web.send(409,"text/plain","No matching battery-change operation is active");return;}
   ReplacementCandidate*c=findReplacementCandidate(static_cast<uint8_t>(radioId));
   if(!c){web.send(404,"text/plain","Replacement RF ID is no longer available");return;}
   Sensor*s=findSensor(static_cast<uint16_t>(requested));
   if(!s){web.send(404,"text/plain","Original sensor no longer exists");return;}
   Sensor*assigned=findSensorByRadioId(static_cast<uint8_t>(radioId));
   if(assigned&&assigned!=s){web.send(409,"text/plain","Replacement RF ID is already assigned to another sensor");return;}
   LaCrosseReading reading=c->reading;int rssi=c->rssi;
   s->radioId=static_cast<uint8_t>(radioId);saveSensors();applyReading(*s,reading,rssi);
   Serial.printf("Reassigned logical sensor %u to RF ID %u\n",static_cast<unsigned>(s->id),static_cast<unsigned>(s->radioId));
   resetBatteryChange();
   web.send(200,"text/plain","Replacement ID assigned; sensor identity and MQTT topic preserved");
 });
 web.on("/api/battery-change/cancel",HTTP_POST,[]{
   if(!batteryChange.active){web.send(409,"text/plain","No battery-change operation is active");return;}
   resetBatteryChange();
   web.send(200,"text/plain","Battery-change operation closed");
 });
 web.on("/update",HTTP_POST,[]{
   if(cfg.otaPassword.isEmpty()){web.send(403,"text/plain","Set an OTA password in Network / MQTT first");return;}
   if(!web.authenticate("admin",cfg.otaPassword.c_str())){web.send(401,"text/plain","Invalid OTA password");return;}
   if(!otaUploadSucceeded){web.send(400,"text/plain",otaUploadError.isEmpty()?"No valid firmware upload":otaUploadError);return;}
   web.send(200,"text/plain","Firmware uploaded successfully; gateway is restarting");
   delay(500);
   ESP.restart();
 },handleOtaUpload);
 web.on("/api/sensor",HTTP_POST,[]{
   JsonDocument d;
   if(deserializeJson(d,web.arg("plain"))){web.send(400,"text/plain","Invalid JSON");return;}
   if(!d["id"].is<int>()){web.send(400,"text/plain","Sensor ID must be an integer");return;}
   int requested=d["id"].as<int>();
   if(requested<0||requested>65535){web.send(400,"text/plain","Sensor ID is out of range");return;}
   Sensor*s=findSensor(static_cast<uint16_t>(requested));
   if(!s){web.send(404,"text/plain","Unknown sensor");return;}
   String name=d["name"]|"";name.trim();
   if(!name.length()){web.send(400,"text/plain","Name is required");return;}
   s->name=name;s->enabled=d["enabled"]|false;saveSensors();
   if(mqtt.connected()){announceSensor(*s);publishReading(*s);}
   web.send(200,"text/plain","Saved");
 });
 web.onNotFound([]{web.send(404,"text/plain","Not found");});web.begin();
}
}
void setup(){
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("LaCrosse MQTT Gateway starting...");
  Serial.printf("Board serial: 115200 baud\n");
  loadSettings();
  loadSensors();
  Serial.printf("Loaded %u sensor(s)\n",(unsigned)sensors.size());
  rfm69Connected=radioBegin();
  if(cfg.ssid.length()){
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid.c_str(),cfg.wifiPassword.c_str());
    Serial.printf("Connecting to Wi-Fi SSID: %s\n",cfg.ssid.c_str());
    uint32_t start=millis();
    while(WiFi.status()!=WL_CONNECTED&&millis()-start<15000){delay(250);Serial.print('.');}
    Serial.println();
  }
  if(WiFi.status()!=WL_CONNECTED){
    usingAp=true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID,AP_PASSWORD);
    Serial.printf("Wi-Fi setup AP: %s\n",AP_SSID);
    Serial.printf("Open http://%s/\n",WiFi.softAPIP().toString().c_str());
  }else{
    Serial.printf("Wi-Fi connected; IP address: %s\n",WiFi.localIP().toString().c_str());
  }
  setupWeb();
  Serial.println("Web server started on port 80");
  if(cfg.mqttHost.length())Serial.printf("MQTT server: %s:%u\n",cfg.mqttHost.c_str(),cfg.mqttPort);
  else Serial.println("MQTT server not configured");
  Serial.println("Startup complete; waiting for LaCrosse RF frames...");
}
void loop(){web.handleClient();radioPoll();mqttReconnect();if(mqtt.connected())mqtt.loop();delay(2);}
