#include <Wire.h>  // for I2C
#include "sensordef_32x32.h"
#include "lookuptable.h"
#include <SPI.h>
#include <Ethernet.h>
//#include <EthernetUdp.h>
#include "uTimerLib.h"


// SENSOR CHARACTERISTICS
struct characteristics {
  uint8_t number_row;    // number of raws
  uint8_t number_col;    // number of column
  uint8_t number_blocks; // number of blocks (top + down)
  uint16_t number_pixel;  // number of pixel
};
characteristics sensor = {32, 32, 8, 1024};


// ETHERNET-COMMUNICATION
uint8_t mac[] = {0x2C, 0xF7, 0xF1, 0x08, 0x19, 0x6C};
unsigned int localPort = 30444;      // local port to listen on
EthernetUDP Udp;
uint8_t ip_partner[4];
uint8_t device_bind;

// PROGRAMM CONTROL
uint16_t timer_duration;
uint8_t switch_ptat_vdd = 0;
uint8_t adr_offset = 0x00;
uint8_t send_data = 0;
uint8_t statusreg;
uint8_t picnum = 0;
uint8_t state = 0;
uint8_t read_block_num = 0;
uint8_t read_eloffset_next_pic = 0;
uint8_t gui_mode = 0;
uint8_t wait_pic = 0;


// EEPROM DATA
uint8_t mbit_calib, bias_calib, clk_calib, bpa_calib, pu_calib, nrofdefpix, gradscale, vddscgrad, vddscoff, epsilon, arraytype;
int8_t globaloff;
uint8_t mbit_user, bias_user, clk_user, bpa_user, pu_user;
uint16_t tablenumber, vddth1, vddth2, ptatth1, ptatth2, ptatgr, globalgain;
int16_t thgrad[32][32];
int16_t thoffset[32][32];
int16_t vddcompgrad[8][32];
int16_t vddcompoff[8][32];
uint16_t pij[32][32];
uint16_t deadpixadr[24];
uint8_t deadpixmask[12];
int32_t pixcij_int32[32][32];
uint32_t id, ptatoff;
float ptatgr_float, ptatoff_float, pixcmin, pixcmax, bw;

// SENSOR DATA
uint8_t data_top_block0[258], data_top_block1[258], data_top_block2[258], data_top_block3[258];
uint8_t data_bottom_block0[258], data_bottom_block1[258], data_bottom_block2[258], data_bottom_block3[258];
uint8_t electrical_offset_top[258], electrical_offset_bottom[258];
uint16_t eloffset[8][32];
uint16_t ptat_top_block0, ptat_top_block1, ptat_top_block2, ptat_top_block3;
uint16_t ptat_bottom_block0, ptat_bottom_block1, ptat_bottom_block2, ptat_bottom_block3;
uint16_t data_pixel[32][32];

// CALCULATED VALUES
uint16_t ptat_av_uint16;
uint16_t vdd_av_uint16;
uint16_t ambient_temperature;
int32_t vij_pixc_int32[32][32];
uint32_t temp_pix_uint32[32][32];

// OTHER
uint32_t gradscale_div;
uint32_t vddscgrad_div;
uint32_t vddscoff_div;
int vddcompgrad_n;
int vddcompoff_n;



/********************************************************************
   Function:        void setup()

   Description:     setup before main loop

   Dependencies:
 *******************************************************************/
void setup() {

  // begin serial communication
  Serial.begin(115200);
  while (!Serial) {
    ;
  }

  Serial.print("\nSETUP\n\n");
  Serial.print("search device");
  Wire.begin();
  uint8_t error = 1;
  while (error != 0) {
    Wire.end();
    delay(2000);
    Wire.begin();
    Wire.beginTransmission(SENSOR_ADDRESS);
    error = Wire.endTransmission();
    Serial.print(".");
  }

  Wire.setClock(CLOCK_EEPROM); // I2C clock frequency 400kHz (for eeprom communication)


  // read ip from eeprom
  IPAddress ip(read_EEPROM_byte(EEPROM_ADDRESS, E_IP),
               read_EEPROM_byte(EEPROM_ADDRESS, E_IP + 1),
               read_EEPROM_byte(EEPROM_ADDRESS, E_IP + 2),
               read_EEPROM_byte(EEPROM_ADDRESS, E_IP + 3));
  IPAddress subnet(read_EEPROM_byte(EEPROM_ADDRESS, E_SUBNET),
                   read_EEPROM_byte(EEPROM_ADDRESS, E_SUBNET + 1),
                   read_EEPROM_byte(EEPROM_ADDRESS, E_SUBNET + 2),
                   read_EEPROM_byte(EEPROM_ADDRESS, E_SUBNET + 3));
  IPAddress myDns(ip[0], ip[1], ip[2], 1);
  IPAddress gateway(ip[0], ip[1], ip[2], 1);


  // look for dhcp. If there's no dhcp -> use eeprom ip
  Serial.print("\nask dhcp for ip: ");
  uint32_t timea = millis();
  Ethernet.begin(mac);
  uint32_t timeb = millis();
  Serial.println(timeb-timea);
  if (Ethernet.localIP()[0] > 0) {
    //Ethernet.begin(mac, Ethernet.localIP(), myDns, gateway, subnet);
    Serial.print("ok -> ip: ");
    Serial.print(Ethernet.localIP()[0]);
    Serial.print(".");
    Serial.print(Ethernet.localIP()[1]);
    Serial.print(".");
    Serial.print(Ethernet.localIP()[2]);
    Serial.print(".");
    Serial.print(Ethernet.localIP()[3]);
  }
  else {
    Ethernet.begin(mac, ip, myDns, gateway, subnet);
    Serial.print("fail -> read default ip from eeprom: ");
    Serial.print(ip[0]);
    Serial.print(".");
    Serial.print(ip[1]);
    Serial.print(".");
    Serial.print(ip[2]);
    Serial.print(".");
    Serial.print(ip[3]);
  }

  Udp.begin(localPort);


  Serial.print("\nread eeprom");
  read_eeprom();


  // I2C clock frequency (for sensor communication)
  Wire.setClock(CLOCK_SENSOR);
  // HINT: To increase the frame rate, here the I2C clock is higher than 1MHz from datasheet. If this causes any problems, set to the datasheet value.


  Serial.print("\nwake up sensor");

  // to wake up sensor set configuration register to 0x01
  // |  7  |  6  |  5  |  4  |   3   |    2     |   1   |    0   |
  // |    RFU    |   Block   | Start | VDD_MEAS | BLIND | WAKEUP |
  // |  0  |  0  |  0  |  0  |   0   |    0     |   0   |    1   |
  write_sensor_byte(SENSOR_ADDRESS, CONFIGURATION_REGISTER, 0x01);


  Serial.print("\ninitialization");
  write_calibration_settings_to_sensor();

  Serial.print("\nstart sensor");
  // to start sensor set configuration register to 0x09
  // |  7  |  6  |  5  |  4  |   3   |    2     |   1   |    0   |
  // |    RFU    |   Block   | Start | VDD_MEAS | BLIND | WAKEUP |
  // |  0  |  0  |  0  |  0  |   1   |    0     |   0   |    1   |
  write_sensor_byte(SENSOR_ADDRESS, CONFIGURATION_REGISTER, 0x09);

  // other calculations before main loop
  gradscale_div = pow(2, gradscale);
  vddscgrad_div = pow(2, vddscgrad);
  vddscoff_div = pow(2, vddscoff);
  calculate_pixcij();



  // config Timer STM32 (need lib uTimerLib.h)
  timer_duration = calc_timer_duration(bw, clk_calib, mbit_calib);
  TimerLib.setInterval_us(readblockinterrupt, timer_duration);
  Serial.print("\nsetup done -> GUI");

  // ERROR TABLENUMBER
  if (tablenumber != TABLENUMBER) {
    Serial.print("\n\nHINT:\tConnected sensor does not match the selected look up table.");
    Serial.print("\n\tThe calculated temperatures could be wrong!");
    Serial.print("\n\tChange device in sensordef_32x32.h to sensor with tablenumber ");
    Serial.print(tablenumber);
  }
  // ERROR BUFFER LENGTH
  if (BUFFER_LENGTH < 258){
    Serial.print("\n\nHINT:\tBUFFER_LENGTH in Wire.h library is not 258 or higher.");
    Serial.print("\n\tThe calculated temperatures could be wrong!");
    Serial.print("\n\tChange BUFFER_LENGTH in wire.h to 258 or higher");
  }
  
}


/********************************************************************
   Function:      calc_timer_duration(float bw, uint8_t clk, uint8_t mbit)

   Description:   calculate the duration of the timer which reads the sensor blocks

   Dependencies:  band width (bw)
                  clock (clk)
                  adc resolution (mbit)
 *******************************************************************/
word calc_timer_duration(float bw, uint8_t clk, uint8_t mbit) {
  float Fclk_float = 12000000 / 63 * clk + 1000000;    // calc clk in Hz
  float a, b, c;
  uint16_t calculated_timer_duration;
  a = 1 / NORM_BW;
  b = 32 * (pow(2, mbit & 0b00001111) + 4) / Fclk_float;
  c = b / a;
  c = c / bw;
  c = SAFETY_FAC * c;

  calculated_timer_duration = c * 1000000; // c in s | timer_duration in µs

  return calculated_timer_duration;
}


/********************************************************************
   Function:        void readblockinterrupt()

   Description:     read one sensor block and change configuration register to next block
                    (also read electrical offset when read_eloffset_next_pic is set)

   Dependencies:    current number to read (read_block_num)
                    marker to read electrical offset after last block (read_eloffset_next_pic)
                    number of complete pictures (picnum)
 *******************************************************************/
void readblockinterrupt() {


  TimerLib.clearTimer();
  // wait for end of conversion bit (~27ms)
  read_sensor_register( STATUS_REGISTER, (uint8_t*)&statusreg, 1);
  while (bitRead(statusreg, 0) == 0) {
    read_sensor_register( STATUS_REGISTER, (uint8_t*)&statusreg, 1);
  }


  if (read_block_num == 0) {
    // read block 0 of top half and block 0 of bottom half
    read_sensor_register( TOP_HALF, (uint8_t*)&data_top_block0, 258);
    read_sensor_register( BOTTOM_HALF, (uint8_t*)&data_bottom_block0, 258);
    // change block in configuration register (to block 1)
    // |  7  |  6  |  5  |  4  |   3   |    2     |   1   |    0   |
    // |    RFU    |   Block   | Start | VDD_MEAS | BLIND | WAKEUP |
    // |  0  |  0  |  0  |  1  |   1   |    x     |   0   |    1   |
    // x is 1 if adr_offset is active
    write_sensor_byte(SENSOR_ADDRESS, CONFIGURATION_REGISTER, 0x19 + adr_offset);
  }

  if (read_block_num == 1) {
    // read block 1 of top half and block 1 of bottom half
    read_sensor_register( TOP_HALF, (uint8_t*)&data_top_block1, 258);
    read_sensor_register( BOTTOM_HALF, (uint8_t*)&data_bottom_block1, 258);
    // change block in configuration register (to block 2)
    // |  7  |  6  |  5  |  4  |   3   |    2     |   1   |    0   |
    // |    RFU    |   Block   | Start | VDD_MEAS | BLIND | WAKEUP |
    // |  0  |  0  |  1  |  0  |   1   |    x     |   0   |    1   |
    // x is 1 if adr_offset is active
    write_sensor_byte(SENSOR_ADDRESS, CONFIGURATION_REGISTER, 0x29 + adr_offset);

  }

  if (read_block_num == 2) {
    // read block 2 of top half and block 2 of bottom half
    read_sensor_register( TOP_HALF, (uint8_t*)&data_top_block2, 258);
    read_sensor_register( BOTTOM_HALF, (uint8_t*)&data_bottom_block2, 258);
    // change block in configuration register (to block 3)
    // |  7  |  6  |  5  |  4  |   3   |    2     |   1   |    0   |
    // |    RFU    |   Block   | Start | VDD_MEAS | BLIND | WAKEUP |
    // |  0  |  0  |  1  |  1  |   1   |    x     |   0   |    1   |
    // x is 1 if adr_offset is active
    write_sensor_byte(SENSOR_ADDRESS, CONFIGURATION_REGISTER, 0x39 + adr_offset);
  }

  if (read_block_num == 3) {
    // read block 3 of top half and block 3 of bottom half
    read_sensor_register( TOP_HALF, (uint8_t*)&data_top_block3, 258);
    read_sensor_register( BOTTOM_HALF, (uint8_t*)&data_bottom_block3, 258);
    // change block in configuration register (to block 0 or to el.offset)
    if (read_eloffset_next_pic == 1) {
      // change block in configuration register (to el.offset)
      // |  7  |  6  |  5  |  4  |   3   |    2     |   1   |    0   |
      // |    RFU    |   Block   | Start | VDD_MEAS | BLIND | WAKEUP |
      // |  0  |  0  |  0  |  0  |   1   |    0     |   1   |    1   |
      write_sensor_byte(SENSOR_ADDRESS, CONFIGURATION_REGISTER, 0x0B + adr_offset);
    }
  }

  if (read_block_num == 4) {
    // read block 0 of top half and block 0 of bottom half
    read_sensor_register( TOP_HALF, (uint8_t*)&electrical_offset_top, 258);
    read_sensor_register( BOTTOM_HALF, (uint8_t*)&electrical_offset_bottom, 258);

  }

  read_block_num++;

  if ( (read_eloffset_next_pic == 0 && read_block_num == 4) || (read_eloffset_next_pic == 1 && read_block_num == 5)) {
    state = 1;
    read_block_num = 0;
    picnum++;

    // read vdd at next picture
    if (switch_ptat_vdd == 0) {
      switch_ptat_vdd = 1;
      adr_offset = 0x04;
    }
    else {
      // read ptat at next picture
      switch_ptat_vdd = 0;
      adr_offset = 0x00;
    }

    if (picnum == 10) {
      read_eloffset_next_pic = 1;
      picnum = 0;
    }
    else {
      read_eloffset_next_pic = 0;
    }


    // change block in configuration register (to block 0)
    // |  7  |  6  |  5  |  4  |   3   |    2     |   1   |    0   |
    // |    RFU    |   Block   | Start | VDD_MEAS | BLIND | WAKEUP |
    // |  0  |  0  |  0  |  0  |   1   |    x     |   0   |    1   |
    // x is 1 if adr_offset is active
    write_sensor_byte(SENSOR_ADDRESS, CONFIGURATION_REGISTER, 0x09 + adr_offset);

  }



  TimerLib.setInterval_us(readblockinterrupt, timer_duration);

}





/********************************************************************
   Function:        void loop()

   Description:

   Dependencies:
 *******************************************************************/
void loop() {

  /*
     state is the state in this "state machine"
        0... reading new data and pic is not complete
        1... reading done (set in ISR readblockinterrupt)
        2... raw data blocks sorted
        3... pixel temps calculated
  */

  switch (state) {

    // ---IDLE---
    case 0:
      // do nothing
      break;

    // ---SORT---
    case 1:
      sort_data();
      state++;
      break;

    // ---CALC---
    case 2:
      // only when sending udp packets with temperatures
      if (send_data == 1) {
        calculate_pixel_temp();
        pixel_masking();
      }
      state++;
      break;

    // ---SEND---
    case 3:
      send_udp_packets();
      state = 0;
      break;

  }

}











/********************************************************************
   Function:        void pixel_masking()

   Description:     repair dead pixel by using the average of the neighbors

   Dependencies:    number of defect pixel (nrofdefpix),
                    dead pixel address (deadpixadr),
                    dead pixel mask (deadpixmask),
                    pixel temperatures (temp_pix_uint32[32][32])
 *******************************************************************/
void pixel_masking() {


  uint8_t number_neighbours[24];
  uint32_t temp_defpix[24];


  for (int i = 0; i < nrofdefpix; i++) {
    number_neighbours[i] = 0;
    temp_defpix[i] = 0;

    // top half

    if (deadpixadr[i] < 512) {

      if ( (deadpixmask[i] & 1 )  == 1) {
        number_neighbours[i]++;
        temp_defpix[i] = temp_defpix[i] + temp_pix_uint32[(deadpixadr[i] >> 5) - 1][(deadpixadr[i] % 32)];
      }


      if ( (deadpixmask[i] & 2 )  == 2 ) {
        number_neighbours[i]++;
        temp_defpix[i] = temp_defpix[i] + temp_pix_uint32[(deadpixadr[i] >> 5) - 1][(deadpixadr[i] % 32) + 1];
      }

      if ( (deadpixmask[i] & 4 )  == 4 ) {
        number_neighbours[i]++;
        temp_defpix[i] = temp_defpix[i] + temp_pix_uint32[(deadpixadr[i] >> 5)][(deadpixadr[i] % 32) + 1];
      }

      if ( (deadpixmask[i] & 8 )  == 8 ) {
        number_neighbours[i]++;
        temp_defpix[i] = temp_defpix[i] + temp_pix_uint32[(deadpixadr[i] >> 5) + 1][(deadpixadr[i] % 32) + 1];
      }

      if ( (deadpixmask[i] & 16 )  == 16 ) {
        number_neighbours[i]++;
        temp_defpix[i] = temp_defpix[i] + temp_pix_uint32[(deadpixadr[i] >> 5) + 1][(deadpixadr[i] % 32)];
      }

      if ( (deadpixmask[i] & 32 )  == 32 ) {
        number_neighbours[i]++;
        temp_defpix[i] = temp_defpix[i] + temp_pix_uint32[(deadpixadr[i] >> 5) + 1][(deadpixadr[i] % 32) - 1];
      }

      if ( (deadpixmask[i] & 64 )  == 64 ) {
        number_neighbours[i]++;
        temp_defpix[i] = temp_defpix[i] + temp_pix_uint32[(deadpixadr[i] >> 5)][(deadpixadr[i] % 32) - 1];
      }

      if ( (deadpixmask[i] & 128 )  == 128 ) {
        number_neighbours[i]++;
        temp_defpix[i] = temp_defpix[i] + temp_pix_uint32[(deadpixadr[i] >> 5) - 1][(deadpixadr[i] % 32) - 1];
      }

    }

    // bottom half
    else {

      if ( (deadpixmask[i] & 1 << 0 )  == 1 << 0) {
        number_neighbours[i]++;
        temp_defpix[i] = temp_defpix[i] + temp_pix_uint32[(deadpixadr[i] >> 5) + 1][(deadpixadr[i] % 32)];
      }

      if ( (deadpixmask[i] & 2 )  == 2 ) {
        number_neighbours[i]++;
        temp_defpix[i] = temp_defpix[i] + temp_pix_uint32[(deadpixadr[i] >> 5) + 1][(deadpixadr[i] % 32) + 1];
      }

      if ( (deadpixmask[i] & 4 )  == 4 ) {
        number_neighbours[i]++;
        temp_defpix[i] = temp_defpix[i] + temp_pix_uint32[(deadpixadr[i] >> 5)][(deadpixadr[i] % 32) + 1];
      }

      if ( (deadpixmask[i] & 8 )  == 8 ) {
        number_neighbours[i]++;
        temp_defpix[i] = temp_defpix[i] + temp_pix_uint32[(deadpixadr[i] >> 5) - 1][(deadpixadr[i] % 32) + 1];
      }

      if ( (deadpixmask[i] & 16 )  == 16 ) {
        number_neighbours[i]++;
        temp_defpix[i] = temp_defpix[i] + temp_pix_uint32[(deadpixadr[i] >> 5) - 1][(deadpixadr[i] % 32)];
      }

      if ( (deadpixmask[i] & 32 )  == 32 ) {
        number_neighbours[i]++;
        temp_defpix[i] = temp_defpix[i] + temp_pix_uint32[(deadpixadr[i] >> 5) - 1][(deadpixadr[i] % 32) - 1];
      }

      if ( (deadpixmask[i] & 64 )  == 64 ) {
        number_neighbours[i]++;
        temp_defpix[i] = temp_defpix[i] + temp_pix_uint32[(deadpixadr[i] >> 5)][(deadpixadr[i] % 32) - 1];
      }

      if ( (deadpixmask[i] & 128 )  == 128 ) {
        number_neighbours[i]++;
        temp_defpix[i] = temp_defpix[i] + temp_pix_uint32[(deadpixadr[i] >> 5) + 1][(deadpixadr[i] % 32) - 1];
      }
    }

    temp_defpix[i] = temp_defpix[i] / number_neighbours[i];
    temp_pix_uint32[deadpixadr[i] >> 5][deadpixadr[i] % 32] = temp_defpix[i];

  }


}





/********************************************************************
   Function:        calculate_pixel_temp()

   Description:     compensate thermal, electrical offset and vdd and multiply sensitivity coeff
                    look for the correct temp in lookup table

   Dependencies:
 *******************************************************************/
void calculate_pixel_temp() {
  int32_t vij_comp_int32[sensor.number_row][sensor.number_col];
  int32_t vij_comp_s_int32[sensor.number_row][sensor.number_col];
  int32_t vij_vddcomp_int32[sensor.number_row][sensor.number_col];
  int64_t vij_pixc_and_pcscaleval;
  int64_t vdd_calc_steps;
  uint16_t table_row, table_col;
  int32_t vx, vy, ydist, dta;

  // find column of lookup table
  for (int i = 0; i < NROFTAELEMENTS; i++) {
    if (ambient_temperature > XTATemps[i]) {
      table_col = i;
    }
  }
  dta = ambient_temperature - XTATemps[table_col];
  ydist = (int32_t)ADEQUIDISTANCE;

  for (int m = 0; m < sensor.number_row; m++) {
    for (int n = 0; n < sensor.number_col; n++) {

      // --- THERMAL OFFSET ---
      // compensate thermal drifts (see datasheet, chapter: 11.2 Thermal Offset)
      vij_comp_int32[m][n] = (data_pixel[m][n] - (thgrad[m][n] * ptat_av_uint16) / gradscale_div - thoffset[m][n]);


      // --- ELECTRICAL OFFSET
      // compensate electrical offset (see datasheet, chapter: 11.3 Electrical Offset)
      // top half
      if (m < sensor.number_row / 2) {
        vij_comp_s_int32[m][n] = vij_comp_int32[m][n] - eloffset[m % 4][n];
      }
      // bottom half
      else {
        vij_comp_s_int32[m][n] = vij_comp_int32[m][n] - eloffset[m % 4 + 4][n];
      }



      // --- VDD ---
      // select VddCompGrad and VddCompOff for pixel m,n:
      // top half
      if (m < sensor.number_row / 2) {
        vddcompgrad_n = vddcompgrad[m % 4][n];
        vddcompoff_n = vddcompoff[m % 4][n];
      }
      // bottom half
      else {
        vddcompgrad_n = vddcompgrad[m % 4 + 4][n];
        vddcompoff_n = vddcompoff[m % 4 + 4][n];
      }
      // compensate vdd (see datasheet, chapter: 11.4 Vdd Compensation)
      vdd_calc_steps = vddcompgrad_n * ptat_av_uint16;
      vdd_calc_steps = vdd_calc_steps / vddscgrad_div;
      vdd_calc_steps = vdd_calc_steps + vddcompoff_n;
      vdd_calc_steps = vdd_calc_steps * ( vdd_av_uint16 - vddth1 - ((vddth2 - vddth1) / (ptatth2 - ptatth1)) * (ptat_av_uint16  - ptatth1));
      vdd_calc_steps = vdd_calc_steps / vddscoff_div;
      vij_vddcomp_int32[m][n] = vij_comp_s_int32[m][n] - vdd_calc_steps;

      // --- SENSITIVITY ---
      // multiply sensitivity coeff for each pixel (see datasheet, chapter: 11.5 Object Temperature)
      vij_pixc_and_pcscaleval = (int64_t)vij_vddcomp_int32[m][n] * (int64_t)PCSCALEVAL;
      vij_pixc_int32[m][n] =  (int32_t)(vij_pixc_and_pcscaleval / (int64_t)pixcij_int32[m][n]);


      // --- LOOKUPTABLE ---
      // find correct temp for this sensor in lookup table and do a bilinear interpolation (see datasheet, chapter: 11.7 Look-up table)
      table_row = vij_pixc_int32[m][n] + TABLEOFFSET;
      table_row = table_row >> ADEXPBITS;
      // bilinear interpolation
      vx = ((((int32_t)TempTable[table_row][table_col + 1] - (int32_t)TempTable[table_row][table_col]) * (int32_t)dta) / (int32_t)TAEQUIDISTANCE) + (int32_t)TempTable[table_row][table_col];
      vy = ((((int32_t)TempTable[table_row + 1][table_col + 1] - (int32_t)TempTable[table_row + 1][table_col]) * (int32_t)dta) / (int32_t)TAEQUIDISTANCE) + (int32_t)TempTable[table_row + 1][table_col];
      temp_pix_uint32[m][n] = (uint32_t)((vy - vx) * ((int32_t)(vij_pixc_int32[m][n] + TABLEOFFSET) - (int32_t)YADValues[table_row]) / ydist + (int32_t)vx);

      // --- GLOBAL OFFSET ---
      temp_pix_uint32[m][n] = temp_pix_uint32[m][n] + globaloff;

    }
  }


}

/********************************************************************
   Function:        void sort_data()

   Description:     sort the raw data blocks in 2d array and calculate ambient temperature, ptat and vdd

   Dependencies:
 *******************************************************************/
void sort_data() {

  uint32_t sum;

  for (int n = 0; n < sensor.number_col; n++) {


    // --- PIXEL DATA TOP HALF ---
    // block 0
    data_pixel[0][n] = data_top_block0[2 * n + 2] << 8 | data_top_block0[2 * n + 3];
    data_pixel[1][n] = data_top_block0[2 * (n + 32) + 2] << 8 | data_top_block0[2 * (n + 32) + 3];
    data_pixel[2][n] = data_top_block0[2 * (n + 64) + 2] << 8 | data_top_block0[2 * (n + 64) + 3];
    data_pixel[3][n] = data_top_block0[2 * (n + 96) + 2] << 8 | data_top_block0[2 * (n + 96) + 3];

    // block 1
    data_pixel[4][n] = data_top_block1[2 * n + 2] << 8 | data_top_block1[2 * n + 3];
    data_pixel[5][n] = data_top_block1[2 * (n + 32) + 2] << 8 | data_top_block1[2 * (n + 32) + 3];
    data_pixel[6][n] = data_top_block1[2 * (n + 64) + 2] << 8 | data_top_block1[2 * (n + 64) + 3];
    data_pixel[7][n] = data_top_block1[2 * (n + 96) + 2] << 8 | data_top_block1[2 * (n + 96) + 3];

    // block 2
    data_pixel[8][n] = data_top_block2[2 * n + 2] << 8 | data_top_block2[2 * n + 3];
    data_pixel[9][n] = data_top_block2[2 * (n + 32) + 2] << 8 | data_top_block2[2 * (n + 32) + 3];
    data_pixel[10][n] = data_top_block2[2 * (n + 64) + 2] << 8 | data_top_block2[2 * (n + 64) + 3];
    data_pixel[11][n] = data_top_block2[2 * (n + 96) + 2] << 8 | data_top_block2[2 * (n + 96) + 3];

    // block 3
    data_pixel[12][n] = data_top_block3[2 * n + 2] << 8 | data_top_block3[2 * n + 3];
    data_pixel[13][n] = data_top_block3[2 * (n + 32) + 2] << 8 | data_top_block3[2 * (n + 32) + 3];
    data_pixel[14][n] = data_top_block3[2 * (n + 64) + 2] << 8 | data_top_block3[2 * (n + 64) + 3];
    data_pixel[15][n] = data_top_block3[2 * (n + 96) + 2] << 8 | data_top_block3[2 * (n + 96) + 3];

    // --- PIXEL DATA BOTTOM HALF ---
    // block 3
    data_pixel[16][n] = data_bottom_block3[192 + 2 * n + 2] << 8 | data_bottom_block3[192 + 2 * n + 3];
    data_pixel[17][n] = data_bottom_block3[128 + 2 * n + 2] << 8 | data_bottom_block3[128 + 2 * n + 3];
    data_pixel[18][n] = data_bottom_block3[64 + 2 * n + 2] << 8 | data_bottom_block3[64 + 2 * n + 3];
    data_pixel[19][n] = data_bottom_block3[0 + 2 * n + 2] << 8 | data_bottom_block3[0 + 2 * n + 3];

    // block 2
    data_pixel[20][n] = data_bottom_block2[192 + 2 * n + 2] << 8 | data_bottom_block2[192 + 2 * n + 3];
    data_pixel[21][n] = data_bottom_block2[128 + 2 * n + 2] << 8 | data_bottom_block2[128 + 2 * n + 3];
    data_pixel[22][n] = data_bottom_block2[64 + 2 * n + 2] << 8 | data_bottom_block2[64 + 2 * n + 3];
    data_pixel[23][n] = data_bottom_block2[0 + 2 * n + 2] << 8 | data_bottom_block2[0 + 2 * n + 3];

    // block 1
    data_pixel[24][n] = data_bottom_block1[192 + 2 * n + 2] << 8 | data_bottom_block1[192 + 2 * n + 3];
    data_pixel[25][n] = data_bottom_block1[128 + 2 * n + 2] << 8 | data_bottom_block1[128 + 2 * n + 3];
    data_pixel[26][n] = data_bottom_block1[64 + 2 * n + 2] << 8 | data_bottom_block1[64 + 2 * n + 3];
    data_pixel[27][n] = data_bottom_block1[0 + 2 * n + 2] << 8 | data_bottom_block1[0 + 2 * n + 3];

    // block 0
    data_pixel[28][n] = data_bottom_block0[192 + 2 * n + 2] << 8 | data_bottom_block0[192 + 2 * n + 3];
    data_pixel[29][n] = data_bottom_block0[128 + 2 * n + 2] << 8 | data_bottom_block0[128 + 2 * n + 3];
    data_pixel[30][n] = data_bottom_block0[64 + 2 * n + 2] << 8 | data_bottom_block0[64 + 2 * n + 3];
    data_pixel[31][n] = data_bottom_block0[0 + 2 * n + 2] << 8 | data_bottom_block0[0 + 2 * n + 3];


    // --- ELECTRICAL OFFSET ---
    // top half
    eloffset[0][n] = electrical_offset_top[2 * n + 2] << 8 | electrical_offset_top[2 * n + 3];
    eloffset[1][n] = electrical_offset_top[2 * (n + 32) + 2] << 8 | electrical_offset_top[2 * (n + 32) + 3];
    eloffset[2][n] = electrical_offset_top[2 * (n + 64) + 2] << 8 | electrical_offset_top[2 * (n + 64) + 3];
    eloffset[3][n] = electrical_offset_top[2 * (n + 96) + 2] << 8 | electrical_offset_top[2 * (n + 96) + 3];
    // bottom half
    eloffset[4][n] = electrical_offset_bottom[2 * (n + 96) + 2] << 8 | electrical_offset_bottom[2 * (n + 96) + 3];
    eloffset[5][n] = electrical_offset_bottom[2 * (n + 64) + 2] << 8 | electrical_offset_bottom[2 * (n + 64) + 3];
    eloffset[6][n] = electrical_offset_bottom[2 * (n + 32) + 2] << 8 | electrical_offset_bottom[2 * (n + 32) + 3];
    eloffset[7][n] = electrical_offset_bottom[2 * n + 2] << 8 | electrical_offset_bottom[2 * n + 3];


  }



  if (switch_ptat_vdd == 1) {

    // new ptat values (1st and 2nd byte at every data block)

    // top
    ptat_top_block0 = data_top_block0[0] << 8  | data_top_block0[1];
    ptat_top_block1 = data_top_block1[0] << 8  | data_top_block1[1];
    ptat_top_block2 = data_top_block2[0] << 8  | data_top_block2[1];
    ptat_top_block3 = data_top_block3[0] << 8  | data_top_block3[1];
    // bottom
    ptat_bottom_block0 = data_bottom_block0[0] << 8  | data_bottom_block0[1];
    ptat_bottom_block1 = data_bottom_block1[0] << 8  | data_bottom_block1[1];
    ptat_bottom_block2 = data_bottom_block2[0] << 8  | data_bottom_block2[1];
    ptat_bottom_block3 = data_bottom_block3[0] << 8  | data_bottom_block3[1];


    // calculate ptat average (datasheet, chapter: 11.1 Ambient Temperature )
    sum = ptat_top_block0 + ptat_top_block1 + ptat_top_block2 + ptat_top_block3 + ptat_bottom_block0 + ptat_bottom_block1 + ptat_bottom_block2 + ptat_bottom_block3;
    ptat_av_uint16 = sum / 8;



    // calculate ambient_temperature (datasheet, chapter: 11.1 Ambient Temperature )
    // only for T-Mode; in V-Mode Ta = 0
    if (send_data == 2 && (mbit_user & 0b00001111) != (mbit_calib & 0b00001111)) {
      ambient_temperature = 0;
    }
    else {
      ambient_temperature = ptat_av_uint16 * ptatgr_float + ptatoff_float;
    }

  }

  else {

    // new vdd values (1st and 2nd byte at every data block)
    uint16_t vdd_top_block0, vdd_top_block1, vdd_top_block2, vdd_top_block3, vdd_bottom_block0, vdd_bottom_block1, vdd_bottom_block2, vdd_bottom_block3;

    // top
    vdd_top_block0 = data_top_block0[0] << 8  | data_top_block0[1];
    vdd_top_block1 = data_top_block1[0] << 8  | data_top_block1[1];
    vdd_top_block2 = data_top_block2[0] << 8  | data_top_block2[1];
    vdd_top_block3 = data_top_block3[0] << 8  | data_top_block3[1];
    // bottom
    vdd_bottom_block0 = data_bottom_block0[0] << 8  | data_bottom_block0[1];
    vdd_bottom_block1 = data_bottom_block1[0] << 8  | data_bottom_block1[1];
    vdd_bottom_block2 = data_bottom_block2[0] << 8  | data_bottom_block2[1];
    vdd_bottom_block3 = data_bottom_block3[0] << 8  | data_bottom_block3[1];

    // calculate vdd average (datasheet, chapter: 11.4 Vdd Compensation )
    sum = vdd_top_block0 + vdd_top_block1 + vdd_top_block2 + vdd_top_block3 + vdd_bottom_block0 + vdd_bottom_block1 + vdd_bottom_block2 + vdd_bottom_block3;
    vdd_av_uint16 = sum / 8;

  }


}

/********************************************************************
   Function:        void calculate_pixcij()

   Description:     calculate sensitivity coefficients for each pixel

   Dependencies:    minimum sensitivity coefficient (pixcmin),
                    maximum sensitivity coefficient (pixcmax),
                    sensitivity coefficient (pij[32][32]),
                    emissivity factor (epsilon),
                    factor for fine tuning of the sensitivity (globalgain)
 *******************************************************************/
void calculate_pixcij() {

  for (int m = 0; m < 32; m++) {
    for (int n = 0; n < 32; n++) {

      // calc sensitivity coefficients (see datasheet, chapter: 11.5 Object Temperature)
      pixcij_int32[m][n] = (int32_t)pixcmax - (int32_t)pixcmin;
      pixcij_int32[m][n] = pixcij_int32[m][n] / 65535;
      pixcij_int32[m][n] = pixcij_int32[m][n] * pij[m][n];
      pixcij_int32[m][n] = pixcij_int32[m][n] + pixcmin;
      pixcij_int32[m][n] = pixcij_int32[m][n] * 1.0 * epsilon / 100;
      pixcij_int32[m][n] = pixcij_int32[m][n] * 1.0 * globalgain / 10000;

    }
  }

}








/********************************************************************
   Function:        void read_EEPROM_byte(int deviceaddress, unsigned int eeaddress )

   Description:     read eeprom register

   Dependencies:    epprom address (deviceaddress)
                    eeprom register (eeaddress)
 *******************************************************************/
byte read_EEPROM_byte(int deviceaddress, unsigned int eeaddress ) {
  byte rdata = 0xFF;

  Wire.beginTransmission(deviceaddress);
  Wire.write((int)(eeaddress >> 8));   // MSB
  Wire.write((int)(eeaddress & 0xFF)); // LSB
  Wire.endTransmission();

  Wire.requestFrom(deviceaddress, 1);

  if (Wire.available()) rdata = Wire.read();

  return rdata;
}



/********************************************************************
   Function:        void read_eeprom()

   Description:     read all values from eeprom

   Dependencies:
 *******************************************************************/
void read_eeprom() {
  byte b[3];

  bw = (read_EEPROM_byte(EEPROM_ADDRESS, E_BW2) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_BW1)) / 100;
  id = read_EEPROM_byte(EEPROM_ADDRESS, E_ID4) << 24 | read_EEPROM_byte(EEPROM_ADDRESS, E_ID3) << 16 | read_EEPROM_byte(EEPROM_ADDRESS, E_ID2) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_ID1);
  mbit_calib = read_EEPROM_byte(EEPROM_ADDRESS, E_MBIT_CALIB);
  bias_calib = read_EEPROM_byte(EEPROM_ADDRESS, E_BIAS_CALIB);
  clk_calib = read_EEPROM_byte(EEPROM_ADDRESS, E_CLK_CALIB);
  bpa_calib = read_EEPROM_byte(EEPROM_ADDRESS, E_BPA_CALIB);
  pu_calib = read_EEPROM_byte(EEPROM_ADDRESS, E_PU_CALIB);
  mbit_user = read_EEPROM_byte(EEPROM_ADDRESS, E_MBIT_USER);
  bias_user = read_EEPROM_byte(EEPROM_ADDRESS, E_BIAS_USER);
  clk_user = read_EEPROM_byte(EEPROM_ADDRESS, E_CLK_USER);
  bpa_user = read_EEPROM_byte(EEPROM_ADDRESS, E_BPA_USER);
  pu_user = read_EEPROM_byte(EEPROM_ADDRESS, E_PU_USER);
  vddth1 = read_EEPROM_byte(EEPROM_ADDRESS, E_VDDTH1_2) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_VDDTH1_1);
  vddth2 = read_EEPROM_byte(EEPROM_ADDRESS, E_VDDTH2_2) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_VDDTH2_1);
  vddscgrad = read_EEPROM_byte(EEPROM_ADDRESS, E_VDDSCGRAD);
  vddscoff = read_EEPROM_byte(EEPROM_ADDRESS, E_VDDSCOFF);
  ptatth1 = read_EEPROM_byte(EEPROM_ADDRESS, E_PTATTH1_2) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_PTATTH1_1);
  ptatth2 = read_EEPROM_byte(EEPROM_ADDRESS, E_PTATTH2_2) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_PTATTH2_1);
  nrofdefpix = read_EEPROM_byte(EEPROM_ADDRESS, E_NROFDEFPIX);
  gradscale = read_EEPROM_byte(EEPROM_ADDRESS, E_GRADSCALE);
  tablenumber = read_EEPROM_byte(EEPROM_ADDRESS, E_TABLENUMBER2) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_TABLENUMBER1);
  arraytype = read_EEPROM_byte(EEPROM_ADDRESS, E_ARRAYTYPE);
  b[0] = read_EEPROM_byte(EEPROM_ADDRESS, E_PTATGR_1);
  b[1] = read_EEPROM_byte(EEPROM_ADDRESS, E_PTATGR_2);
  b[2] = read_EEPROM_byte(EEPROM_ADDRESS, E_PTATGR_3);
  b[3] = read_EEPROM_byte(EEPROM_ADDRESS, E_PTATGR_4);
  ptatgr_float = *(float*)b;
  b[0] = read_EEPROM_byte(EEPROM_ADDRESS, E_PTATOFF_1);
  b[1] = read_EEPROM_byte(EEPROM_ADDRESS, E_PTATOFF_2);
  b[2] = read_EEPROM_byte(EEPROM_ADDRESS, E_PTATOFF_3);
  b[3] = read_EEPROM_byte(EEPROM_ADDRESS, E_PTATOFF_4);
  ptatoff_float = *(float*)b;
  b[0] = read_EEPROM_byte(EEPROM_ADDRESS, E_PIXCMIN_1);
  b[1] = read_EEPROM_byte(EEPROM_ADDRESS, E_PIXCMIN_2);
  b[2] = read_EEPROM_byte(EEPROM_ADDRESS, E_PIXCMIN_3);
  b[3] = read_EEPROM_byte(EEPROM_ADDRESS, E_PIXCMIN_4);
  pixcmin = *(float*)b;
  b[0] = read_EEPROM_byte(EEPROM_ADDRESS, E_PIXCMAX_1);
  b[1] = read_EEPROM_byte(EEPROM_ADDRESS, E_PIXCMAX_2);
  b[2] = read_EEPROM_byte(EEPROM_ADDRESS, E_PIXCMAX_3);
  b[3] = read_EEPROM_byte(EEPROM_ADDRESS, E_PIXCMAX_4);
  pixcmax = *(float*)b;
  epsilon = read_EEPROM_byte(EEPROM_ADDRESS, E_EPSILON);
  globaloff = read_EEPROM_byte(EEPROM_ADDRESS, E_GLOBALOFF);
  globalgain = read_EEPROM_byte(EEPROM_ADDRESS, E_GLOBALGAIN_2) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_GLOBALGAIN_1);
  // --- DeadPixAdr ---
  for (int i = 0; i < nrofdefpix; i++) {
    deadpixadr[i] = read_EEPROM_byte(EEPROM_ADDRESS, E_DEADPIXADR + 2 * i + 1 ) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_DEADPIXADR + 2 * i);
    if (deadpixadr[i] > 512) {    // adaptedAdr:
      deadpixadr[i] = 1024 + 512 - deadpixadr[i] + 2 * (deadpixadr[i] % 32 ) - 32;
    }
  }
  // --- DeadPixMask ---
  for (int i = 0; i < nrofdefpix; i++) {
    deadpixmask[i] = read_EEPROM_byte(EEPROM_ADDRESS, E_DEADPIXMASK + i);
  }

  // --- Thgrad_ij ---
  int m = 0;
  int n = 0;
  uint16_t addr_i = 0x0740; // start address
  // top half
  for (int i = 0; i < 512; i++) {
    addr_i = 0x0740 + 2 * i;
    thgrad[m][n] = read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 2 * i);
    n++;
    if (n == 32) {
      n = 0;
      m++;
    }
  }
  // bottom half
  for (int i = 0; i < sensor.number_col; i++) {

    thgrad[31][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0400 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0400 + 2 * i);
    thgrad[30][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0400 + 1 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0400 + 1 * 64 + 2 * i);
    thgrad[29][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0400 + 2 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0400 + 2 * 64 + 2 * i);
    thgrad[28][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0400 + 3 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0400 + 3 * 64 + 2 * i);

    thgrad[27][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0500 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0500 + 2 * i);
    thgrad[26][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0500 + 1 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0500 + 1 * 64 + 2 * i);
    thgrad[25][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0500 + 2 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0500 + 2 * 64 + 2 * i);
    thgrad[24][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0500 + 3 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0500 + 3 * 64 + 2 * i);

    thgrad[23][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0600 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0600 + 2 * i);
    thgrad[22][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0600 + 1 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0600 + 1 * 64 + 2 * i);
    thgrad[21][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0600 + 2 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0600 + 2 * 64 + 2 * i);
    thgrad[20][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0600 + 3 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0600 + 3 * 64 + 2 * i);

    thgrad[19][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0700 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0700 + 2 * i);
    thgrad[18][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0700 + 1 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0700 + 1 * 64 + 2 * i);
    thgrad[17][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0700 + 2 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0700 + 2 * 64 + 2 * i);
    thgrad[16][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0700 + 3 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THGRAD + 0x0700 + 3 * 64 + 2 * i);

  }

  // --- ThOffset_ij ---
  m = 0;
  n = 0;
  // top half
  for (int i = 0; i < 512; i++) {
    addr_i = 0x0F40 + 2 * i;
    thoffset[m][n] = read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 2 * i);
    n++;
    if (n == 32) {
      n = 0;
      m++;
    }
  }

  // bottom half
  for (int i = 0; i < sensor.number_col; i++) {
    thoffset[31][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0400 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0400 + 2 * i);
    thoffset[30][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0400 + 1 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0400 + 1 * 64 + 2 * i);
    thoffset[29][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0400 + 2 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0400 + 2 * 64 + 2 * i);
    thoffset[28][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0400 + 3 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0400 + 3 * 64 + 2 * i);

    thoffset[27][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0500 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0500 + 2 * i);
    thoffset[26][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0500 + 1 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0500 + 1 * 64 + 2 * i);
    thoffset[25][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0500 + 2 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0500 + 2 * 64 + 2 * i);
    thoffset[24][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0500 + 3 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0500 + 3 * 64 + 2 * i);

    thoffset[23][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0600 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0600 + 2 * i);
    thoffset[22][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0600 + 1 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0600 + 1 * 64 + 2 * i);
    thoffset[21][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0600 + 2 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0600 + 2 * 64 + 2 * i);
    thoffset[20][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0600 + 3 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0600 + 3 * 64 + 2 * i);

    thoffset[19][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0700 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0700 + 2 * i);
    thoffset[18][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0700 + 1 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0700 + 1 * 64 + 2 * i);
    thoffset[17][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0700 + 2 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0700 + 2 * 64 + 2 * i);
    thoffset[16][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0700 + 3 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_THOFFSET + 0x0700 + 3 * 64 + 2 * i);
  }


  //---VddCompGrad---

  // top half
  for (int i = 0; i < sensor.number_col; i++) {
    // top half
    vddcompgrad[0][i] = read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPGRAD + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPGRAD + 2 * i);
    vddcompgrad[1][i] = read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPGRAD + 1 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPGRAD + 1 * 64 + 2 * i);
    vddcompgrad[2][i] = read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPGRAD + 2 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPGRAD + 2 * 64 + 2 * i);
    vddcompgrad[3][i] = read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPGRAD + 3 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPGRAD + 3 * 64 + 2 * i);
    // bottom half (backwards)
    vddcompgrad[7][i] = read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPGRAD + 4 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPGRAD + 4 * 64 + 2 * i);
    vddcompgrad[6][i] = read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPGRAD + 5 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPGRAD + 5 * 64 + 2 * i);
    vddcompgrad[5][i] = read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPGRAD + 6 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPGRAD + 6 * 64 + 2 * i);
    vddcompgrad[4][i] = read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPGRAD + 7 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPGRAD + 7 * 64 + 2 * i);

  }

  //---VddCompOff---

  // top half
  for (int i = 0; i < sensor.number_col; i++) {
    // top half
    vddcompoff[0][i] = read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPOFF + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPOFF + 2 * i);
    vddcompoff[1][i] = read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPOFF + 1 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPOFF + 1 * 64 + 2 * i);
    vddcompoff[2][i] = read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPOFF + 2 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPOFF + 2 * 64 + 2 * i);
    vddcompoff[3][i] = read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPOFF + 3 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPOFF + 3 * 64 + 2 * i);
    // bottom half
    vddcompoff[7][i] = read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPOFF + 4 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPOFF + 4 * 64 + 2 * i);
    vddcompoff[6][i] = read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPOFF + 5 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPOFF + 5 * 64 + 2 * i);
    vddcompoff[5][i] = read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPOFF + 6 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPOFF + 6 * 64 + 2 * i);
    vddcompoff[4][i] = read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPOFF + 7 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_VDDCOMPOFF + 7 * 64 + 2 * i);

  }


  // --- P_ij ---
  m = 0;
  n = 0;
  // top half
  for (int i = 0; i < 512; i++) {
    addr_i = 0x0F40 + 2 * i;
    pij[m][n] = read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 2 * i);
    n++;
    if (n == 32) {
      n = 0;
      m++;
    }
  }

  // bottom half
  for (int i = 0; i < sensor.number_col; i++) {
    pij[31][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0400 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0400 + 2 * i);
    pij[30][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0400 + 1 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0400 + 1 * 64 + 2 * i);
    pij[29][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0400 + 2 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0400 + 2 * 64 + 2 * i);
    pij[28][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0400 + 3 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0400 + 3 * 64 + 2 * i);

    pij[27][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0500 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0500 + 2 * i);
    pij[26][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0500 + 1 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0500 + 1 * 64 + 2 * i);
    pij[25][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0500 + 2 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0500 + 2 * 64 + 2 * i);
    pij[24][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0500 + 3 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0500 + 3 * 64 + 2 * i);

    pij[23][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0600 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0600 + 2 * i);
    pij[22][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0600 + 1 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0600 + 1 * 64 + 2 * i);
    pij[21][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0600 + 2 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0600 + 2 * 64 + 2 * i);
    pij[20][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0600 + 3 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0600 + 3 * 64 + 2 * i);

    pij[19][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0700 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0700 + 2 * i);
    pij[18][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0700 + 1 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0700 + 1 * 64 + 2 * i);
    pij[17][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0700 + 2 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0700 + 2 * 64 + 2 * i);
    pij[16][i] =  read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0700 + 3 * 64 + 2 * i + 1) << 8 | read_EEPROM_byte(EEPROM_ADDRESS, E_PIJ + 0x0700 + 3 * 64 + 2 * i);

  }


}

/********************************************************************
   Function:        void write_user_settings_to_sensor()

   Description:     write calibration data (from eeprom) to trim registers (sensor)

   Dependencies:
 *******************************************************************/
void write_user_settings_to_sensor() {

  write_sensor_byte(SENSOR_ADDRESS, TRIM_REGISTER1, mbit_user);
  delay(5);
  write_sensor_byte(SENSOR_ADDRESS, TRIM_REGISTER2, bias_user);
  delay(5);
  write_sensor_byte(SENSOR_ADDRESS, TRIM_REGISTER3, bias_user);
  delay(5);
  write_sensor_byte(SENSOR_ADDRESS, TRIM_REGISTER4, clk_user);
  delay(5);
  write_sensor_byte(SENSOR_ADDRESS, TRIM_REGISTER5, bpa_user);
  delay(5);
  write_sensor_byte(SENSOR_ADDRESS, TRIM_REGISTER6, bpa_user);
  delay(5);
  write_sensor_byte(SENSOR_ADDRESS, TRIM_REGISTER7, pu_user);
}

/********************************************************************
   Function:        void write_calibration_settings_to_sensor()

   Description:     write calibration data (from eeprom) to trim registers (sensor)

   Dependencies:
 *******************************************************************/
void write_calibration_settings_to_sensor() {

  write_sensor_byte(SENSOR_ADDRESS, TRIM_REGISTER1, mbit_calib);
  delay(5);
  write_sensor_byte(SENSOR_ADDRESS, TRIM_REGISTER2, bias_calib);
  delay(5);
  write_sensor_byte(SENSOR_ADDRESS, TRIM_REGISTER3, bias_calib);
  delay(5);
  write_sensor_byte(SENSOR_ADDRESS, TRIM_REGISTER4, clk_calib);
  delay(5);
  write_sensor_byte(SENSOR_ADDRESS, TRIM_REGISTER5, bpa_calib);
  delay(5);
  write_sensor_byte(SENSOR_ADDRESS, TRIM_REGISTER6, bpa_calib);
  delay(5);
  //---------------------------------------------------------------------------------------------------------------------------------------------------------------
  //write_sensor_byte(SENSOR_ADDRESS, TRIM_REGISTER7, pu_calib);
  write_sensor_byte(SENSOR_ADDRESS, TRIM_REGISTER7, pu_user);
}


/********************************************************************
   Function:        void read_sensor_register( uint16_t addr, uint8_t *dest, uint16_t n)

   Description:     read sensor register

   Dependencies:    register address (addr),
                    number of bytes (n)
 *******************************************************************/
void read_sensor_register( uint16_t addr, uint8_t *dest, uint16_t n) {

  Wire.requestFrom(SENSOR_ADDRESS, n, addr, 1, 1);
  while (Wire.available()) {
    *dest++  = Wire.read();
  }
  Wire.endTransmission();
}



/********************************************************************
   Function:        void write_SENDOR_byte(int deviceaddress, unsigned int eeaddress )

   Description:     write sensor register

   Dependencies:    device address (deviceaddress)
                    register address (registeraddress)
                    input byte (input)
 *******************************************************************/
void write_EEPROM_byte(int deviceaddress, unsigned int eeaddress, uint8_t input) {

  Wire.beginTransmission(deviceaddress);
  Wire.write((int)(eeaddress >> 8));   // MSB
  Wire.write((int)(eeaddress & 0xFF)); // LSB
  Wire.write(input); // LSB
  Wire.endTransmission();

}

/********************************************************************
   Function:        void read_sensor_register( uint16_t addr, uint8_t *dest, uint16_t n)

   Description:     read sensor register

   Dependencies:    register address (addr),
                    number of bytes (n)
 *******************************************************************/
void write_sensor_byte(uint8_t deviceaddress, uint8_t registeraddress, uint8_t input) {

  Wire.beginTransmission(deviceaddress);
  Wire.write(registeraddress);
  Wire.write(input);
  Wire.endTransmission();

}























/****************************************************************************************************************************************

   END OF READ-OUT PROGRAMM

   the following function is used to chat with the GUI:
      "Heimann Sensor ArraySoft v2"

 **************************************************************************************************************************************/

































/********************************************************************
   Function:        void send_udp_packets()

   Description:

   Dependencies:
 *******************************************************************/
void send_udp_packets() {
  // if there's data available, read a packet
  int packetSize = Udp.parsePacket();
  char packetBuffer[1000] = {""};
  // strings with unknown parts:
  char packetChangeIP[] = {"HTPA device IP change request to "};
  char packetChangeEPSILON[] = {"Set Emission to "};
  uint8_t change_ip, change_epsilon, change_id;


  if (packetSize) {
    IPAddress remote = Udp.remoteIP();

    /*  debug
      Serial.print("Received packet of size ");
      Serial.println(packetSize);

      Serial.print("From ");
      IPAddress remote = Udp.remoteIP();

      for (int i = 0; i < 4; i++) {
        Serial.print(remote[i], DEC);
        if (i < 3) {
          Serial.print(".");
        }
      }

      Serial.print(", port ");
      Serial.println(Udp.remotePort());
    */

    if ( (ip_partner[0] == Udp.remoteIP()[0] &&
          ip_partner[1] == Udp.remoteIP()[1] &&
          ip_partner[2] == Udp.remoteIP()[2] &&
          ip_partner[3] == Udp.remoteIP()[3]) || device_bind == 0) {

      // read the packet into packetBufffer
      Udp.read(packetBuffer, 1000);
    }

    else {
      return;
    }


    // ----------------------------------------------------
    // HTPA RESPONSED
    if (strcmp(packetBuffer, "Calling HTPA series devices") == 0) {
      Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
      Udp.write("HTPA series responsed! I am Arraytype ");
      Udp.print(arraytype);
      Udp.write(" MODTYPE 005\r\nADC: ");
      Udp.print( (mbit_calib & 15) + 4);    // calc ADC resolution
      Udp.write("\r\n");
      Udp.write("HTPA32x32d v.0.01 Heimann Sensor GmbH; written by D. Pauer 2019-11-13\r\n");
      Udp.write("I am running on ");
      float clk_float = 12000000 / 63 * clk_calib + 1000000;    // calc clk in MHz
      Udp.print(clk_float / 1000, 1); // print clk in kHz
      Udp.write(" kHz\r\n");
      Udp.write("MAC-ID: ");
      for (int i = 0; i < 6; i++) {
        if (mac[i] < 0x10) {
          Udp.write("0");
        }
        Udp.print(mac[i], HEX);
        if (i < 5) {
          Udp.write(".");
        }
      }
      Udp.write(" IP: ");
      for (int i = 0; i < 4; i++) {

        if (Ethernet.localIP()[i] < 10) {
          Udp.write("00");
        }
        else if (Ethernet.localIP()[i] < 100) {
          Udp.write("0");
        }
        Udp.print(Ethernet.localIP()[i]);
        if (i < 3) {
          Udp.write(".");
        }
      }
      Udp.write(" DevID: ");
      for (int i = 1; i < 9; i++) {
        if (id < pow(10, i)) {
          Udp.write("0");
        }
      }
      Udp.print(id);
      Udp.endPacket();

    }


    // ----------------------------------------------------
    // CHANGE EPSILON
    if (packetSize > 16) {
      change_epsilon = 1;
      // compare the first position of string
      for (int i = 0; i < 16; i++) {
        if (packetBuffer[i] != packetChangeEPSILON[i]) {
          change_epsilon = 0;
        }
      }
      if (change_epsilon) {
        send_data = 0;
        TimerLib.clearTimer();
        epsilon = (int)(packetBuffer[16] - '0') * 100 + (int)(packetBuffer[17] - '0') * 10 + (int)(packetBuffer[18] - '0');


        // write new epsilon to eeprom
        Wire.setClock(CLOCK_EEPROM);
        delay(1000);
        write_EEPROM_byte(EEPROM_ADDRESS, E_EPSILON, epsilon);

        // calculate pixcij with new epsilon
        calculate_pixcij();


        Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
        Udp.write("Emission changed to ");
        Udp.print(epsilon);
        Udp.write("%\r\n\r\n");
        Udp.endPacket();


        Wire.setClock(CLOCK_SENSOR);
        delay(1000);
        TimerLib.setInterval_us(readblockinterrupt, timer_duration);
      }
    }


    // ----------------------------------------------------
    // CHANGE IP AND SUBNET
    if (packetSize > 33) {
      change_ip = 1;
      // compare the first position of string
      for (int i = 0; i < 33; i++) {
        if (packetBuffer[i] != packetChangeIP[i]) {
          change_ip = 0;

        }
      }

      if (change_ip) {
        send_data = 0;
        TimerLib.clearTimer();

        byte new_ip[4];
        byte new_subnet[4];

        new_ip[0] = (int)(packetBuffer[33] - '0') * 100 + (int)(packetBuffer[34] - '0') * 10 + (int)(packetBuffer[35] - '0');
        new_ip[1] = (int)(packetBuffer[37] - '0') * 100 + (int)(packetBuffer[38] - '0') * 10 + (int)(packetBuffer[39] - '0');
        new_ip[2] = (int)(packetBuffer[41] - '0') * 100 + (int)(packetBuffer[42] - '0') * 10 + (int)(packetBuffer[43] - '0');
        new_ip[3] = (int)(packetBuffer[45] - '0') * 100 + (int)(packetBuffer[46] - '0') * 10 + (int)(packetBuffer[47] - '0');
        new_subnet[0] = (int)(packetBuffer[49] - '0') * 100 + (int)(packetBuffer[50] - '0') * 10 + (int)(packetBuffer[51] - '0');
        new_subnet[1] = (int)(packetBuffer[53] - '0') * 100 + (int)(packetBuffer[54] - '0') * 10 + (int)(packetBuffer[55] - '0');
        new_subnet[2] = (int)(packetBuffer[57] - '0') * 100 + (int)(packetBuffer[58] - '0') * 10 + (int)(packetBuffer[59] - '0');
        new_subnet[3] = (int)(packetBuffer[61] - '0') * 100 + (int)(packetBuffer[62] - '0') * 10 + (int)(packetBuffer[63] - '0');

        Wire.setClock(CLOCK_EEPROM);
        delay(10);
        for (int i = 0; i < 4; i++) {
          // write new ip to eeprom
          write_EEPROM_byte(EEPROM_ADDRESS, E_IP + i, new_ip[i]);
          delay(10);

          // write new subnet to eeprom
          write_EEPROM_byte(EEPROM_ADDRESS, E_SUBNET + i, new_subnet[i]);
          delay(10);
        }
        Wire.setClock(CLOCK_SENSOR);
        delay(100);


        Udp.beginPacket(ip_partner, Udp.remotePort());
        Udp.write("Device changed IP to ");
        for (int i = 0; i < 4; i++) {
          if (new_ip[i] < 10) {
            Udp.write("00");
          } else if (new_ip[i] < 100) {
            Udp.write("0");
          }
          Udp.print(new_ip[i]);
          Udp.write(".");
        }

        Udp.write(" and Subnet to ");

        for (int i = 0; i < 4; i++) {
          if (new_subnet[i] < 10) {
            Udp.write("00");
          } else if (new_subnet[i] < 100) {
            Udp.write("0");
          }
          Udp.print(new_subnet[i]);
          Udp.write(".");
        }
        Udp.write("\r\n");
        Udp.endPacket();


        IPAddress new_ip1(new_ip[0], new_ip[1], new_ip[2], new_ip[3]);
        IPAddress new_subnet1(new_subnet[0], new_subnet[1], new_subnet[2], new_subnet[3]);
        IPAddress new_myDns1(new_ip[0], new_ip[1], new_ip[2], 1);
        IPAddress new_gateway1(new_ip[0], new_ip[1], new_ip[2], 1);

        delay(100);


        Udp.stop();
        Ethernet.begin(mac, new_ip1, new_myDns1, new_gateway1, new_subnet1);
        delay(2000);
        Udp.begin(localPort);
        delay(2000);


        TimerLib.setInterval_us(readblockinterrupt, timer_duration);
      }
    }





    // ----------------------------------------------------
    // SEND IP AND MAC (HW FILTER)
    if (strcmp(packetBuffer, "Bind HTPA series device") == 0) {
      if (device_bind == 0) {
        Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
        Udp.write("HW Filter is ");
        for (int i = 0; i < 4; i++) {

          if (Ethernet.localIP()[i] < 10) {
            Udp.write("00");
          }
          else if (Ethernet.localIP()[i] < 100) {
            Udp.write("0");
          }
          Udp.print(Ethernet.localIP()[i]);
          if (i < 3) {
            Udp.write(".");
          }
        }
        Udp.write(" MAC ");
        for (int i = 0; i < 6; i++) {
          if (mac[i] < 0x10) {
            Udp.write("0");
          }
          Udp.print(mac[i], HEX);
          if (i < 5) {
            Udp.write(".");
          }
        }
        Udp.write("\n\r");
        Udp.endPacket();

        device_bind = 1;
        for (int i = 0; i < 4; i++) {
          ip_partner[i] = Udp.remoteIP()[i];
        }

      }
      else {
        Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
        Udp.write("Device already bound\n\r");
        Udp.endPacket();
      }
    }

    // ----------------------------------------------------
    //USER SETTING
    if (strcmp(packetBuffer, "G") == 0) {
      Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
      Udp.write("HTPA32x32d 2019/11/31 v.0.01 Heimann Sensor GmbH; written by D. Pauer\n\r");
      Udp.write("BIAS: ");
      if (bias_user < 0x10) {
        Udp.write("0");
      }
      Udp.print(bias_user, HEX);
      Udp.write("Clock: ");
      if (clk_calib < 0x10) {
        Udp.write("0");
      }
      Udp.print(clk_user, HEX);
      Udp.write("MBIT: ");
      if (mbit_user < 0x10) {
        Udp.write("0");
      }
      Udp.print(mbit_user, HEX);
      Udp.write("BPA: ");
      if (bpa_user < 0x10) {
        Udp.write("0");
      }
      Udp.print(bpa_user, HEX);
      Udp.write("PU: ");
      if (pu_user < 0x10) {
        Udp.write("0");
      }
      Udp.print(pu_user, HEX);
      Udp.write("GlobalOffset: ");
      Udp.print(globaloff, HEX);
      Udp.write("GlobalGain: ");
      Udp.print(globalgain, HEX);

      Udp.endPacket();

    }


    // ----------------------------------------------------
    // DECREAS/INCREASE CLK
    if (strcmp(packetBuffer, "a") == 0 || strcmp(packetBuffer, "A") == 0 ) {

      TimerLib.clearTimer();
      send_data = 0;

      Wire.setClock(CLOCK_EEPROM);

      // write new subnet to eeprom
      if (strcmp(packetBuffer, "a") == 0  && clk_user > 0) {
        clk_user = clk_user - 1;
        write_EEPROM_byte(EEPROM_ADDRESS, E_CLK_USER, clk_user);
      }
      if (strcmp(packetBuffer, "A") == 0 && clk_user < 63) {
        clk_user = clk_user + 1;
        write_EEPROM_byte(EEPROM_ADDRESS, E_CLK_USER, clk_user);
      }
      delay(5);

      Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
      Udp.write("MHZClk is ");
      float clk_float = 12000000 / 63 * clk_user + 1000000;    // calc clk in MHz

      Udp.print(clk_float / 1000, 1);
      Udp.write(" kHz\r\n");
      Udp.endPacket();



      Wire.setClock(CLOCK_SENSOR);
      TimerLib.setInterval_us(readblockinterrupt, timer_duration);
    }

    // ----------------------------------------------------
    // DECREAS/INCREASE BIAS
    if (strcmp(packetBuffer, "i") == 0 || strcmp(packetBuffer, "I") == 0 ) {

      TimerLib.clearTimer();
      send_data = 0;
      Wire.setClock(CLOCK_EEPROM);

      // write new subnet to eeprom
      if (strcmp(packetBuffer, "i") == 0 && bias_user > 0) {
        bias_user = bias_user - 1;
        write_EEPROM_byte(EEPROM_ADDRESS, E_BIAS_USER, bias_user);
      }
      if (strcmp(packetBuffer, "I") == 0 && bias_user < 31) {
        bias_user = bias_user + 1;
        write_EEPROM_byte(EEPROM_ADDRESS, E_BIAS_USER, bias_user);
      }
      delay(5);

      Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
      Udp.write("BIAS-Trim: ");
      Udp.print(bias_user, HEX);
      Udp.write("\r\n");
      Udp.endPacket();


      Wire.setClock(CLOCK_SENSOR);
      TimerLib.setInterval_us(readblockinterrupt, timer_duration);

    }


    // ----------------------------------------------------
    // DECREAS/INCREASE BPA
    if (strcmp(packetBuffer, "j") == 0 || strcmp(packetBuffer, "J") == 0 ) {

      TimerLib.clearTimer();
      send_data = 0;
      Wire.setClock(CLOCK_EEPROM);

      // write new subnet to eeprom
      if (strcmp(packetBuffer, "j") == 0 && bpa_user > 0) {
        bpa_user = bpa_user - 1;
        write_EEPROM_byte(EEPROM_ADDRESS, E_BPA_USER, bpa_user);
      }
      if (strcmp(packetBuffer, "J") == 0 && bpa_user < 31) {
        bpa_user = bpa_user + 1;
        write_EEPROM_byte(EEPROM_ADDRESS, E_BPA_USER, bpa_user);
      }
      delay(5);

      Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
      Udp.write("BPA-Trim: ");
      Udp.print(bpa_user, HEX);
      Udp.write("\r\n");
      Udp.endPacket();


      Wire.setClock(CLOCK_SENSOR);
      TimerLib.setInterval_us(readblockinterrupt, timer_duration);

    }

    // ----------------------------------------------------
    // DECREAS/INCREASE MBIT
    if (strcmp(packetBuffer, "r") == 0 ||
        strcmp(packetBuffer, "R") == 0 ||
        strcmp(packetBuffer, "o") == 0 ||
        strcmp(packetBuffer, "O") == 0 ) {

      TimerLib.clearTimer();
      send_data = 0;

      uint8_t adc_res, adc_ref;
      adc_res = mbit_user & 15;
      adc_ref = (mbit_user & 48) >> 4;

      Wire.setClock(CLOCK_EEPROM);

      // write new subnet to eeprom
      if (strcmp(packetBuffer, "r") == 0 && adc_res > 4) {
        adc_res = adc_res - 1;
      }
      if (strcmp(packetBuffer, "R") == 0 && adc_res < 12) {
        adc_res = adc_res + 1;
      }
      if (strcmp(packetBuffer, "o") == 0 && adc_ref > 0) {
        adc_ref = adc_ref - 1;
      }
      if (strcmp(packetBuffer, "O") == 0 && adc_ref < 3) {
        adc_ref = adc_ref + 1;
      }

      mbit_user = adc_ref << 4 | adc_res;

      write_EEPROM_byte(EEPROM_ADDRESS, E_MBIT_USER, mbit_user);
      delay(5);

      if (strcmp(packetBuffer, "r") == 0 ||
          strcmp(packetBuffer, "R") == 0 ) {
        Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
        Udp.write("Resolution: ");
        if (adc_res + 4 < 10) {
          Udp.write("0");
        }
        Udp.print(adc_res + 4);
        Udp.write(" bit\r\n");
        Udp.endPacket();
      }

      if (strcmp(packetBuffer, "o") == 0 ||
          strcmp(packetBuffer, "O") == 0 ) {
        Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
        Udp.write("ADC-Ref: ");
        if (adc_ref < 10) {
          Udp.write("0");
        }
        Udp.print(adc_ref);
        Udp.write(" bit\r\n");
        Udp.endPacket();
      }


      Wire.setClock(CLOCK_SENSOR);
      TimerLib.setInterval_us(readblockinterrupt, timer_duration);
    }

    // ----------------------------------------------------
    // DECREAS/INCREASE PU
    if (strcmp(packetBuffer, "p") == 0) {

      TimerLib.clearTimer();
      send_data = 0;
      Wire.setClock(CLOCK_EEPROM);
      if (pu_user == 17) {
        pu_user = 34;
      }
      else if (pu_user == 34) {
        pu_user = 68;
      }
      else if (pu_user == 68) {
        pu_user = 136;
      }
      else if (pu_user == 136) {
        pu_user = 17;
      }
      Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
      Udp.write("PU-Trim: ");
      Udp.print(pu_user, HEX);
      Udp.write("\r\n");
      Udp.endPacket();

      write_EEPROM_byte(EEPROM_ADDRESS, E_PU_USER, pu_user);

      Wire.setClock(CLOCK_SENSOR);
      TimerLib.setInterval_us(readblockinterrupt, timer_duration);
    }

    // ----------------------------------------------------
    //HW RELEASED
    if (strcmp(packetBuffer, "x Release HTPA series device") == 0) {
      send_data = 0;
      device_bind = 0;
      for (int i = 0; i < 4; i++) {
        ip_partner[i] = 0;
      }
      Udp.beginPacket(ip_partner, Udp.remotePort());
      Udp.write("HW-Filter released\r\n");
      Udp.endPacket();
    }



    // ----------------------------------------------------
    // SEND DATA (TEMPS)
    if (strcmp(packetBuffer, "K") == 0) {
      if (gui_mode == 1) {
        TimerLib.clearTimer();
        // write user calobration to sensor
        write_calibration_settings_to_sensor();
        delay(100);
        timer_duration = calc_timer_duration(bw, clk_calib, mbit_calib);
        TimerLib.setInterval_us(readblockinterrupt, timer_duration);
        gui_mode = 0;
        wait_pic = 0;
      }
      send_data = 1;
    }


    // ----------------------------------------------------
    // SEND DATA (VOLTAGE)
    if (strcmp(packetBuffer, "t") == 0) {
      TimerLib.clearTimer();
      // write user settings to sensor
      write_user_settings_to_sensor();
      delay(100);
      timer_duration = calc_timer_duration(bw, clk_user, mbit_user);

      TimerLib.setInterval_us(readblockinterrupt, timer_duration);
      gui_mode = 1;
      send_data = 2;
      wait_pic = 0;
    }

    // ----------------------------------------------------
    // STOP SENDING
    if (strcmp(packetBuffer, "x") == 0 || strcmp(packetBuffer, "X") == 0) {
      send_data = 0;
      device_bind = 0;
      wait_pic = 0;
    }


  }





  // send data
  if (send_data > 0) {
    // convert data in 2 udp packets


    uint8_t packet1[1292];
    uint8_t packet2[1288];
    int p = 0;
    for (int m = 0; m < sensor.number_row; m++) {
      for (int n = 0; n < sensor.number_col; n++) {

        if (send_data == 1) {
          if (p < 1292) {
            packet1[p] = ((uint16_t) temp_pix_uint32[m][n])  & 0x00ff; // low byte
            p++;
            packet1[p] = (((uint16_t) temp_pix_uint32[m][n])  & 0xff00) >> 8; // high byte
            p++;
          }

          else {
            packet2[p - 1292] = ((uint16_t) temp_pix_uint32[m][n])  & 0x00ff; // low byte
            p++;
            packet2[p - 1292] = (((uint16_t) temp_pix_uint32[m][n])  & 0xff00) >> 8; // high byte
            p++;
          }
        }

        if (send_data == 2) {
          if (p < 1292) {
            packet1[p] = ((uint16_t) data_pixel[m][n])  & 0x00ff; // low byte
            p++;
            packet1[p] = (((uint16_t) data_pixel[m][n])  & 0xff00) >> 8; // high byte
            p++;
          }

          else {
            packet2[p - 1292] = ((uint16_t) data_pixel[m][n])  & 0x00ff; // low byte
            p++;
            packet2[p - 1292] = (((uint16_t) data_pixel[m][n])  & 0xff00) >> 8; // high byte
            p++;
          }
        }

      }
    }

    for (int m = 0; m < 8; m++) {
      for (int n = 0; n < sensor.number_col; n++) {
        packet2[p - 1292] = eloffset[m][n] & 0x00ff; // low byte
        p++;
        packet2[p - 1292] = (eloffset[m][n] & 0xff00) >> 8; // high byte
        p++;
      }

    }

    packet2[1268] = ((uint16_t) vdd_av_uint16) & 0x00ff; // low byte
    packet2[1269] = (((uint16_t) vdd_av_uint16) & 0xff00) >> 8; // high byte
    packet2[1270] = ((uint16_t) ambient_temperature) & 0x00ff; // low byte
    packet2[1271] = (((uint16_t) ambient_temperature) & 0xff00) >> 8; // high byte
    packet2[1272] = ptat_top_block0 & 0x00ff; // low byte
    packet2[1273] = (ptat_top_block0 & 0xff00) >> 8; // high byte
    packet2[1274] = ptat_top_block1 & 0x00ff; // low byte
    packet2[1275] = (ptat_top_block1 & 0xff00) >> 8; // high byte
    packet2[1276] = ptat_top_block2 & 0x00ff; // low byte
    packet2[1277] = (ptat_top_block2 & 0xff00) >> 8; // high byte
    packet2[1278] = ptat_top_block3 & 0x00ff; // low byte
    packet2[1279] = (ptat_top_block3 & 0xff00) >> 8; // high byte
    packet2[1280] = ptat_bottom_block0 & 0x00ff; // low byte
    packet2[1281] = (ptat_bottom_block0 & 0xff00) >> 8; // high byte
    packet2[1282] = ptat_bottom_block1 & 0x00ff; // low byte
    packet2[1283] = (ptat_bottom_block1 & 0xff00) >> 8; // high byte
    packet2[1284] = ptat_bottom_block2 & 0x00ff; // low byte
    packet2[1285] = (ptat_bottom_block2 & 0xff00) >> 8; // high byte
    packet2[1286] = ptat_bottom_block3 & 0x00ff; // low byte
    packet2[1287] = (ptat_bottom_block3 & 0xff00) >> 8; // high byte


    if (wait_pic > 5) {
      // packet 1
      //Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
      Udp.beginPacket(ip_partner, Udp.remotePort());
      Udp.write(packet1, 1292);
      Udp.endPacket();


      // packet 2
      Udp.beginPacket(ip_partner, Udp.remotePort());
      Udp.write(packet2, 1288);
      Udp.endPacket();
    }
    else
    {
      wait_pic++;
    }
  }


}
