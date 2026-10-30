#include <Arduino.h>
#include <WiFi.h>
#include "ModbusIP_ESP8266.h"
#include <Wire.h>
#include <VL6180X.h>
#include <QTRSensors.h>
#include "HX711.h"
#include <Adafruit_NeoPixel.h>

// ==========================================
// CONFIGURACIÓN DE RED WIFI Y MODBUS
// ==========================================
const char* ssid = "Modbus";
const char* password = "TU_CLAVE_WIFI";  // no subir la clave real

IPAddress ip(172, 16, 146, 220);
IPAddress subnet(255, 255, 255, 0);
IPAddress gateway(172, 16, 146, 93);

ModbusIP mb;

// ==================== PINES Y CONFIGURACIÓN ====================
#define MOT_DER_ENA 16
#define MOT_DER_IN1 19
#define MOT_DER_IN2 32

#define MOT_IZQ_ENB 23
#define MOT_IZQ_IN3 5
#define MOT_IZQ_IN4 18

#define PIN_SENSOR 17
const int pinAnalogico = 39;

const int PIN_ENCODER_DER = 34;
const int PIN_ENCODER_IZQ = 35;

const float PULSOS_POR_REV = 20.0;
const float RADIO_CM = 2;

const int LED_BUILTIN = 2;
QTRSensors qtr;
const uint8_t SensorCount = 8;
uint16_t sensorValues[SensorCount];

VL6180X sensorTOF;

const int freq = 500;
const int resolution = 8;

// ================= CONFIGURACIÓN HX711 Y NEOPIXEL =================
const int DOUT = 36;
const int CLK = 15;
const float FACTOR_CALIBRACION = 447.15;
HX711 balanza;

#define PIN_NEOPIXEL 1
#define NUMPIXELS 8
Adafruit_NeoPixel anillo(NUMPIXELS, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

// ================= VARIABLES GLOBALES PROTEGIDAS =================
volatile uint16_t distanciaTOF = 9999;
volatile float peso_gramos = 0.0;

// Mutex para proteger las variables compartidas entre núcleos
SemaphoreHandle_t xMutex = NULL;

int contador = 0;
int ultimoEstadoLeido = LOW;
unsigned long tiempoUltimoConteo = 0;
const long retardoRebote = 150;

volatile long contadorPulsosDer = 0;
volatile long contadorPulsosIzq = 0;
float velocidadFiltradaDer = 0.0;
float velocidadFiltradaIzq = 0.0;
const float factorSuavizado = 0.4;

int valorLectura = 0;
int porcentaje = 0;
unsigned long tiempoAnteriorPeriodico = 0;
const long intervaloPeriodico = 10;

const int setpointLinea = 3500;
const int toleranciaLinea = 50;

float errorAcumuladoLin = 0.0, errorAnteriorLin = 0.0;
int pwmDer = 0;
int pwmIzq = 0;

TaskHandle_t TareaCore0;

// ==================== INTERRUPCIONES Y FUNCIONES ====================
void IRAM_ATTR contarPulsoDer() { contadorPulsosDer++; }
void IRAM_ATTR contarPulsoIzq() { contadorPulsosIzq++; }

void actualizarNeopixel(float peso) {
 anillo.clear();
 int ledsEncendidos = 0;
 uint32_t colorActual = 0;

 if (peso < 10) {
     ledsEncendidos = 0;
 } else if (peso >= 10 && peso < 150) {
     ledsEncendidos = map(peso, 10, 150, NUMPIXELS, 0);
     ledsEncendidos = constrain(ledsEncendidos, 0, NUMPIXELS);
     colorActual = anillo.Color(0, 255, 0);
 } else if (peso >= 150 && peso < 250) {
     ledsEncendidos = map(peso, 150, 250, NUMPIXELS, 0);
     ledsEncendidos = constrain(ledsEncendidos, 0, NUMPIXELS);
     colorActual = anillo.Color(255, 255, 0);
 } else {
     ledsEncendidos = NUMPIXELS;
     colorActual = anillo.Color(255, 0, 0);
 }

 for (int i = 0; i < ledsEncendidos; i++) {
     anillo.setPixelColor(i, colorActual);
 }
 anillo.show();
}

// ==================== TAREA ASÍNCRONA (CORE 0) ====================
void TareaComunicaciones(void * parameter) {
 Wire.begin();
 sensorTOF.init();
 sensorTOF.configureDefault();
 sensorTOF.setTimeout(500);

 balanza.begin(DOUT, CLK);
 balanza.set_scale(FACTOR_CALIBRACION);
 balanza.tare(30);

 for(;;) {
   // 1. Procesar Modbus (Red)
   mb.task();

   // 2. Lectura local de Balanza
   float pesoLocal = 0.0;
   if (balanza.is_ready()) {
     pesoLocal = balanza.get_units(1);
     if (pesoLocal < 0) pesoLocal = 0;
     actualizarNeopixel(pesoLocal);
   }

   // 3. Lectura local de ToF
   uint16_t distLocal = sensorTOF.readRangeSingleMillimeters();
   if (sensorTOF.timeoutOccurred()) {
     distLocal = 9999;
   }

   // 4. Guardar datos de forma segura usando el Mutex
   if (xMutex != NULL) {
     if (xSemaphoreTake(xMutex, (TickType_t)5) == pdTRUE) {
       peso_gramos = pesoLocal;
       distanciaTOF = distLocal;
       xSemaphoreGive(xMutex);
     }
   }

   vTaskDelay(pdMS_TO_TICKS(100));
 }
}

// ==================== SETUP (CORRE EN CORE 1) ====================
void setup() {
 anillo.begin();
 anillo.clear();
 anillo.show();

 xMutex = xSemaphoreCreateMutex();

 qtr.setTypeRC();
 qtr.setSensorPins((const uint8_t[]){33, 4, 12, 13, 14, 25, 26, 27}, SensorCount);

 pinMode(LED_BUILTIN, OUTPUT);
 digitalWrite(LED_BUILTIN, HIGH);
 for (uint16_t i = 0; i < 400; i++) {
   qtr.calibrate();
 }
 digitalWrite(LED_BUILTIN, LOW);

 WiFi.mode(WIFI_STA);
 WiFi.config(ip, gateway, subnet);
 WiFi.begin(ssid, password);
 while (WiFi.status() != WL_CONNECTED) { delay(50); }

 mb.server();
 mb.addIreg(0, 0); mb.addIreg(1, 0); mb.addIreg(2, 0); mb.addIreg(3, 0); mb.addIreg(4, 0); mb.addIreg(5, 0);
 mb.addHreg(0, 0); mb.addHreg(1, 50); mb.addHreg(2, 5); mb.addHreg(3, 20); mb.addHreg(4, 210);

 pinMode(PIN_SENSOR, INPUT);
 ultimoEstadoLeido = digitalRead(PIN_SENSOR);

 pinMode(PIN_ENCODER_DER, INPUT);
 attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_DER), contarPulsoDer, RISING);
 pinMode(PIN_ENCODER_IZQ, INPUT);
 attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_IZQ), contarPulsoIzq, RISING);

 pinMode(pinAnalogico, INPUT);

 pinMode(MOT_DER_IN1, OUTPUT); pinMode(MOT_DER_IN2, OUTPUT);
 pinMode(MOT_IZQ_IN3, OUTPUT); pinMode(MOT_IZQ_IN4, OUTPUT);

 digitalWrite(MOT_DER_IN1, HIGH); digitalWrite(MOT_DER_IN2, LOW);
 digitalWrite(MOT_IZQ_IN3, LOW); digitalWrite(MOT_IZQ_IN4, HIGH);

 ledcAttach(MOT_DER_ENA, freq, resolution);
 ledcAttach(MOT_IZQ_ENB, freq, resolution);
 ledcWrite(MOT_DER_ENA, 0); ledcWrite(MOT_IZQ_ENB, 0);

 xTaskCreatePinnedToCore(
   TareaComunicaciones,
   "CommsTask",
   10000,
   NULL,
   1,
   &TareaCore0,
   0
 );
}

// ==================== LOOP PRINCIPAL (CORRE EN CORE 1) ====================
void loop() {
 unsigned long tiempoActual = millis();

 int valorParadaModbus = mb.Hreg(0);
 float kp_lin = mb.Hreg(1) / 1000.0;
 float ki_lin = mb.Hreg(2) / 1000.0;
 float kd_lin = mb.Hreg(3) / 1000.0;
 int basePwm = mb.Hreg(4);

 int lectura = digitalRead(PIN_SENSOR);
 if (lectura == HIGH && ultimoEstadoLeido == LOW) {
   if (tiempoActual - tiempoUltimoConteo > retardoRebote) {
     contador++;
     tiempoUltimoConteo = tiempoActual;
     if (contador >= 11) { contador = 0; }
   }
 }
 ultimoEstadoLeido = lectura;

 if (tiempoActual - tiempoAnteriorPeriodico >= intervaloPeriodico) {
   float deltaT = (tiempoActual - tiempoAnteriorPeriodico) / 1000.0;
   tiempoAnteriorPeriodico = tiempoActual;

   noInterrupts();
   long pulsosDer = contadorPulsosDer;
   long pulsosIzq = contadorPulsosIzq;
   contadorPulsosDer = 0;
   contadorPulsosIzq = 0;
   interrupts();

   float velAngDer = ((pulsosDer / PULSOS_POR_REV) * 2.0 * PI) / deltaT;
   float velInstDer = velAngDer * RADIO_CM;
   velocidadFiltradaDer = (factorSuavizado * velInstDer) + ((1.0 - factorSuavizado) * velocidadFiltradaDer);

   float velAngIzq = ((pulsosIzq / PULSOS_POR_REV) * 2.0 * PI) / deltaT;
   float velInstIzq = velAngIzq * RADIO_CM;
   velocidadFiltradaIzq = (factorSuavizado * velInstIzq) + ((1.0 - factorSuavizado) * velocidadFiltradaIzq);

   float velocidadPromedio = (velocidadFiltradaDer + velocidadFiltradaIzq) / 2.0;

   uint16_t posicionLinea = qtr.readLineBlack(sensorValues);

   float errorLinea = setpointLinea - posicionLinea;
   if (abs(errorLinea) <= toleranciaLinea) { errorLinea = 0; }

   float derivativoLin = (errorLinea - errorAnteriorLin) / deltaT;
   errorAcumuladoLin += errorLinea * deltaT;
   errorAnteriorLin = errorLinea;

   float esfuerzoLinea = (kp_lin * errorLinea) + (ki_lin * errorAcumuladoLin) + (kd_lin * derivativoLin);

   pwmDer = basePwm - (int)esfuerzoLinea;
   pwmIzq = basePwm + (int)esfuerzoLinea;

   pwmDer = constrain(pwmDer, 0, 255);
   pwmIzq = constrain(pwmIzq, 0, 255);

   float pesoCopia = 0.0;
   uint16_t tofCopia = 9999;

   if (xMutex != NULL) {
     if (xSemaphoreTake(xMutex, (TickType_t)2) == pdTRUE) {
       pesoCopia = peso_gramos;
       tofCopia = distanciaTOF;
       xSemaphoreGive(xMutex);
     }
   }

   bool paradaPorContador  = (contador == valorParadaModbus);
   bool paradaPorPeso      = (pesoCopia >= 250.0);
   bool paradaPorDistancia = (tofCopia >= 70 && tofCopia <= 110);

   if (paradaPorContador || paradaPorPeso || paradaPorDistancia) {
     ledcWrite(MOT_DER_ENA, 0);
     ledcWrite(MOT_IZQ_ENB, 0);
     errorAcumuladoLin = 0.0;
     errorAnteriorLin = errorLinea;
   } else {
     if (basePwm == 0) {
       ledcWrite(MOT_DER_ENA, 0);
       ledcWrite(MOT_IZQ_ENB, 0);
     } else {
       ledcWrite(MOT_DER_ENA, pwmDer);
       ledcWrite(MOT_IZQ_ENB, pwmIzq);
     }
   }
   valorLectura = analogRead(pinAnalogico);
   porcentaje = map(valorLectura, 1530, 2500, 0, 100);

   mb.Ireg(0, porcentaje);
   mb.Ireg(1, (uint16_t)(velocidadPromedio * 100));
   mb.Ireg(2, contador);
   mb.Ireg(3, tofCopia);
   mb.Ireg(4, posicionLinea);
   mb.Ireg(5, (uint16_t)pesoCopia);
 }
}
