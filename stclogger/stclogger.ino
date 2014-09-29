/*
* STC1000+ logger, datalogger for the STC-1000 thermostat.
*
* Copyright 2014 Mats Staffansson
*
* This file is part of STC1000+.
*
* STC1000+ is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* STC1000+ is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with STC1000+. If not, see <http://www.gnu.org/licenses/>.
*
* This sketch will allow logging of temperature(s), on time for
* heating and cooling and optionally the amount of bubbles passing
* through the air lock (with a light gate module) to an SD/uSD card
* using a SD/uSD module. The sketch is targeting the Arduino Pro Mini.
*/

#include <SD.h>
#include "math.h"

#define T1_PIN              A0
#define T2_PIN              A1
#define BUBBLE_COUNT_PIN    2 /* int.0 */
#define nMCRL_PIN           3
#define HEAT_PIN            4
#define COOL_PIN            5
#define SWITCH_PIN          7
#define ICSPDAT_PIN         8
#define ICSPCLK_PIN         9
#define SD_SPI_CS_PIN      10
#define SD_SPI_MOSI_PIN    11
#define SD_SPI_MISO_PIN    12
#define SD_SPI_CLK_PIN     13

const int ad_lookup[] = { 
  0, -486, -355, -270, -205, -151, -104, -61, -21, 16, 51, 85, 119, 152, 184, 217, 250, 284, 318, 354, 391, 431, 473, 519, 569, 624, 688, 763, 856, 977, 1154, 1482 };

volatile unsigned long bubble_counter = 0;
int t1, t2;
boolean debug = 0;
boolean logging = 0;
unsigned int logfileno = 0;
char logfilename[13];

static int ad_to_temp(unsigned int adfilter){
  unsigned char i;
  long temp = 32;
  unsigned char a = ((adfilter >> 5) & 0x3f); // Lower 6 bits
  unsigned char b = ((adfilter >> 11) & 0x1f); // Upper 5 bits

  // Interpolate between lookup table points
  for (i = 0; i < 64; i++) {
    if(a <= i) {
      temp += ad_lookup[b];
    } 
    else {
      temp += ad_lookup[b + 1];
    }
  }

  // Divide by 64 to get back to normal temperature
  return (temp >> 6);
}

static boolean read_temperatures(){
  static unsigned long last=millis();
  static unsigned char count=0;
  static unsigned int ad_filter1=0x7fff;
  static unsigned int ad_filter2=0x7fff;
  boolean new_temps=false;

  /* Take new reading (alternating between T1 and T2 every 60ms */
  /* and filter it with leaky integrator */
  /* This mimics the handling on STC-1000+ */
  if(millis()-last >= 60){
    last += 60;
    if(count&1){
      ad_filter2 = (ad_filter2 - (ad_filter2 >> 6)) + analogRead(T2_PIN);
    } 
    else {
      ad_filter1 = (ad_filter1 - (ad_filter1 >> 6)) + analogRead(T1_PIN);
    }
    count++;

    /* Convert A/D filters to temperatures */
    if(count>=16){
      t1 = ad_to_temp(ad_filter1);
      t2 = ad_to_temp(ad_filter2);
      count=0;
      new_temps = true;
    }
  }
  return new_temps;
}

/* Enable/disable buzzer on STC-1000 */
static void buzz(boolean on){
  pinMode(ICSPDAT_PIN, on ? OUTPUT : INPUT);
  digitalWrite(ICSPDAT_PIN, on ? HIGH : LOW);
}

/* Generate logfile name */
/* Ugly, but works*/
static char *get_logfile_name(int i){
  char j;
  
  logfilename[0] = 's';
  logfilename[1] = 't';
  logfilename[2] = 'c';
  logfilename[3] = '1';
  logfilename[4] = '0';
  logfilename[5] = '0';
  logfilename[6] = '0';
  logfilename[7] = 'p';
  logfilename[8] = '.';
  for(j=0; i>100; j++){
    i-=100;
  }
  logfilename[9] = '0' + j;
  for(j=0; i>10; j++){
    i-=10;
  }
  logfilename[10] = '0'+ j;

  logfilename[11] = '0'+i;
  logfilename[12] = '\0';
  return logfilename;
}

/* Accept commands from serial */
static void handle_rx(){
  if(Serial.available()){
    char command = Serial.read();
    switch(command){
    case 'a':
      buzz(HIGH);
      Serial.println("Buzzer on");
      break;
    case 'b':
      buzz(LOW);
      Serial.println("Buzzer off");
      break;
    case 'd':
      debug = !debug;
      Serial.print("Debugging ");
      Serial.println(debug ? "on" : "off");
      break;
    case 'l':
      logging = !logging;
      Serial.print("Logging ");
      Serial.println(logging ? "on" : "off");
      /* Fallthrough */
    case 'f':
      Serial.print("Filename: ");
      Serial.println(logfilename);
      break;
    case 'r':
      logfileno = 0;
      get_logfile_name(logfileno);
      break;
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      if(logfileno<100){
        logfileno=(logfileno*10)+(command-'0');
        get_logfile_name(logfileno);
      }
      break;
    case 'p':
      {
        File dataFile = SD.open(logfilename);
        if (dataFile) {
          while (dataFile.available()) {
            Serial.write(dataFile.read());
          }
          dataFile.close();
        }
      }
      break;
    }
  }
}

/* Check for pushbutton being released (low-high transition) */
static boolean button_press(){
  static unsigned long last = millis();
  static boolean btn=1;
  
  if(millis() - last >= 25){
    last+=25;
    if(digitalRead(SWITCH_PIN)!=btn){
      btn=!btn;
      return btn;
    }
  }
  return 0;  
}

/* Print one line of logdata to serial, SD or whatever... */
static void log_data(Print *p){
  unsigned long bc;
  
  /*Disable interrupts while reading bubble count variable */
  noInterrupts();
  bc = bubble_counter;
  interrupts();

  p->print(t1, DEC); 
  p->print(";"); 
  p->print(t2, DEC); 
  p->print(";");
  p->print(digitalRead(HEAT_PIN), DEC);    
  p->print(";");
  p->print(digitalRead(COOL_PIN), DEC);
  p->print(";");
  p->print(digitalRead(SWITCH_PIN), DEC);
  p->print(";");
  p->print(bc, DEC);
  p->println();
}

/* Interrupt service routine for counting bubbles */
void bubble_count_isr() {
  bubble_counter++;
}

void setup()
{
  // Open serial communications and wait for port to open:
  Serial.begin(115200);
  while (!Serial) {
    ; // wait for serial port to connect. Needed for Leonardo only
  }

  Serial.println("Initializing SD card...");

  pinMode(SD_SPI_CS_PIN, OUTPUT);
  digitalWrite(SD_SPI_CS_PIN, HIGH);
  while(!SD.begin(SD_SPI_CS_PIN)) {
    Serial.println("SD card failed, or not present");
    buzz(1);
    delay(100);
    buzz(0);
    delay(900);
  }
  Serial.println("SD card initialized.");
  
  while(SD.exists(get_logfile_name(logfileno))){
    logfileno++;
    if(logfileno>999){
      Serial.println("Too many logfiles...");
      while(1){
        buzz(1);
        delay(300);
        buzz(0);
        delay(700);
      }
    }
  }
  Serial.print("Using log file: ");
  Serial.println(logfilename);

  pinMode(BUBBLE_COUNT_PIN, INPUT);
  attachInterrupt(0, bubble_count_isr, RISING);

  pinMode(nMCRL_PIN, INPUT);
  pinMode(SWITCH_PIN, INPUT);
  digitalWrite(SWITCH_PIN, HIGH);
  pinMode(ICSPDAT_PIN, INPUT);
  pinMode(ICSPCLK_PIN, INPUT);

  pinMode(HEAT_PIN, INPUT);
  pinMode(COOL_PIN, INPUT);
}

void loop()
{
  static unsigned long last = millis();
  static unsigned int rows=0;

  if(read_temperatures() && millis() - last > 60000){
    last += 60000;
    if(debug){
      log_data(&Serial);
    }
    if(logging){
      File dataFile = SD.open(logfilename, FILE_WRITE);
      if(dataFile) {
        log_data(&dataFile);
        dataFile.close();
        /* New logfile every 24h */
        if(++rows >= 3600){
          get_logfile_name(++logfileno);
          rows=0;
        }
      }
    }
  }

  handle_rx();

  if(button_press()){
    buzz(1);
    delay(100);
    buzz(0);
    logging=!logging;
    if(logging){
      delay(100);
      buzz(1);
      delay(100);
      buzz(0);
    }
  }

}

