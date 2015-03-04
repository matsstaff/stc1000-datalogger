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
* through the air lock (with a light gate module) to a 24c256.
* The sketch is targeting the Arduino Pro Mini.
*/

#include <Wire.h>
#include <EEPROM.h>
#include "math.h"

#define T1_PIN              A0
#define T2_PIN              A1
#define BUBBLE_COUNT_PIN    2 /* int.0 */
#define nMCRL_PIN           3
#define HEAT_PIN            4
#define COOL_PIN            5
//#define SWITCH_PIN          7
#define ICSPDAT_PIN         8
#define ICSPCLK_PIN         9

// 24C256 
// All pins grounded except PIN 5 -> A4, PIN 6 -> A5, PIN 8 -> Vcc
#define AT24C256_PIN5_SDA		A4
#define AT24C256_PIN6_SCL		A5
#define AT24C256_I2C_ADDRESS	0x50

const int ad_lookup[] = { 
  0, -486, -355, -270, -205, -151, -104, -61, -21, 16, 51, 85, 119, 152, 184, 217, 250, 284, 318, 354, 391, 431, 473, 519, 569, 624, 688, 763, 856, 977, 1154, 1482 };

volatile unsigned int bubble_counter = 0;
static unsigned char logging = 0;

typedef union log_entry_u {
	unsigned char raw[4];
	struct temp_str {
		unsigned type		: 1;
		unsigned bcx16		: 1;
		unsigned ad1		: 11;
		unsigned ad2		: 11;
		unsigned bc		: 8;
	} temp;
 	struct relay_str {
		unsigned res2		: 1;
		unsigned heating	: 1;
		unsigned on		: 1;
		unsigned res3		: 13;
		unsigned timestamp	: 16;
	} relay;
} logentry;

/**
 * Write data to serial EEPROM
 */
void write_24c256(unsigned int address, const unsigned char *buf, int n) {
    while (n > 0) {
        Wire.beginTransmission(AT24C256_I2C_ADDRESS);
        Wire.write((unsigned char)(address >> 8));        // hi-byte of address
        Wire.write((unsigned char)address);
        while (n) {
            Wire.write(*buf++);
            n--;
            if ((++address & 63) == 0)             // page boundary
                break;
        }
        Wire.endTransmission();
        delay(10);                             // time to write
    }
}

/**
 * Read data from serial EEPROM
 */
void read_24c256(unsigned int address, unsigned char *buf, int n) {
    Wire.beginTransmission(AT24C256_I2C_ADDRESS);
    Wire.write((unsigned char)(address >> 8)); // MSB
    Wire.write((unsigned char)address & 0xFF); // LSB
    Wire.endTransmission();
    Wire.requestFrom(AT24C256_I2C_ADDRESS, n);
    while (n) {
      if (Wire.available()){
        *buf++ = Wire.read();
        n--;
      }
    }
}

/**
 * Convert 11 bit ADC value to a temperature
 */
static int ad_to_temp(unsigned int adfilter){
  unsigned char i;
  long temp = 32;
  unsigned char a = (adfilter & 0x3f); // Lower 6 bits
  unsigned char b = ((adfilter >> 6) & 0x1f); // Upper 5 bits

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

static void print_logentry(union log_entry_u entry){
	if(entry.temp.type){
		Serial.print(entry.relay.heating ? "heat " : "cool ");
		Serial.print(entry.relay.on ? "on " : "off ");
		Serial.print("@ ");
		Serial.println(entry.relay.timestamp, DEC);
	} else {
		unsigned int bc = entry.temp.bc;
		if(entry.temp.bcx16){
			bc <<= 4;
		}
		Serial.print(ad_to_temp(entry.temp.ad1), DEC);
		Serial.print(";");
		Serial.print(ad_to_temp(entry.temp.ad2), DEC);
		Serial.print(";");
		Serial.print(bc, DEC);
		Serial.println(";");
	}
}

/* Enable/disable buzzer on STC-1000 */
static void buzz(boolean on){
  pinMode(ICSPDAT_PIN, on ? OUTPUT : INPUT);
  digitalWrite(ICSPDAT_PIN, on ? HIGH : LOW);
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
    case 'e':
      Serial.println("Enable logging and halt");
      EEPROM.write(0,1);
      while(1);
      break;
    case 'p':
	{
		int i;
		logentry mylogentry;
		for(i=0; i<32768; i+=4){
			read_24c256(i, mylogentry.raw, 4);
			print_logentry(mylogentry);
		}
	}
      break;
    }
  }
}

/* Interrupt service routine for counting bubbles */
void bubble_count_isr() {
  bubble_counter++;
}

void setup() {
  // Open serial communications and wait for port to open:
  Serial.begin(115200);
  while (!Serial) {
    ; // wait for serial port to connect. Needed for Leonardo only
  }

  Wire.begin(); 

  pinMode(BUBBLE_COUNT_PIN, INPUT);
  attachInterrupt(0, bubble_count_isr, RISING);

  pinMode(nMCRL_PIN, INPUT);
//  pinMode(SWITCH_PIN, INPUT);
//  digitalWrite(SWITCH_PIN, HIGH);
  pinMode(ICSPDAT_PIN, INPUT);
  pinMode(ICSPCLK_PIN, INPUT);

  pinMode(HEAT_PIN, INPUT);
  pinMode(COOL_PIN, INPUT);

  logging = EEPROM.read(0);
  EEPROM.write(0,0);
}

void loop() {
  static unsigned long last = millis();
  static unsigned long last_temp = last;
  static unsigned int adc1=0, adc2=0;
  static unsigned char heat=LOW, cool=LOW;
  static unsigned int address = 0;
  unsigned char di;
  unsigned long curr_millis;

  curr_millis = millis();

  if(logging){  
	  di = digitalRead(HEAT_PIN);
	  if(di!=heat){
		logentry mylogentry;
		mylogentry.temp.type = 1;
		mylogentry.relay.heating = 1;
		mylogentry.relay.on = di;
		mylogentry.relay.timestamp = curr_millis - last;
		heat = di;
		write_24c256(address, mylogentry.raw, 4);
		address += 4;
	  }

	  di = digitalRead(COOL_PIN);
	  if(di!=cool){
		logentry mylogentry;
		mylogentry.temp.type = 1;
		mylogentry.relay.heating = 0;
		mylogentry.relay.on = di;
		mylogentry.relay.timestamp = curr_millis - last;
		cool = di;
		write_24c256(address, mylogentry.raw, 4);
		address += 4;
	  }

	  if(curr_millis - last_temp >= 1875){
		last_temp += 1875;
		analogRead(T1_PIN);
		adc1 += analogRead(T1_PIN);
		analogRead(T2_PIN);
		adc2 += analogRead(T2_PIN);
	  }

	  if(curr_millis - last >= 60000){
		logentry mylogentry;
		unsigned int bc;
		last += 60000;
		mylogentry.temp.type = 0;
		mylogentry.temp.ad1 = (adc1 >> 3);
		mylogentry.temp.ad2 = (adc2 >> 3);
		adc1 = 0;
		adc2 = 0;
		noInterrupts();
		bc = bubble_counter;
		bubble_counter = 0;
		interrupts();
		if(bc > 255){
			mylogentry.temp.bcx16 = 1;
			bc += 8;
			bc >>= 4;
			bc = (bc > 255) ? 255; bc;
		} else {
			mylogentry.temp.bcx16 = 0;
		}
		mylogentry.temp.bc = bc;
		write_24c256(address, mylogentry.raw, 4);
		address += 4;
	  }
	if(address >= 32768){
		logging = 0;
	}
  }
  handle_rx();

}

