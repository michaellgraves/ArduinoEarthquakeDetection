//Add the SPI library so we can communicate with the ADXL345 sensor
#include <SPI.h>
#include <EEPROM.h>

//Fona set-ups
//Fona libraries located here - C:\Program Files (x86)\Arduino\libraries\Adafruit_FONA
#include <SoftwareSerial.h>
#include <Adafruit_FONA.h>
#define FONA_RX 2
#define FONA_TX 3
#define FONA_RST 4
#ifdef __AVR__
SoftwareSerial fonaSS = SoftwareSerial(FONA_TX, FONA_RX);
SoftwareSerial *fonaSerial = &fonaSS;
#else
HardwareSerial *fonaSerial = &Serial1;
#endif
Adafruit_FONA fona = Adafruit_FONA(FONA_RST);

//Assign the Chip Select signal to pin 10.
 int8_t CS=10;

//This is a list of some of the registers available on the ADXL345.
//To learn more about these and the rest of the registers on the ADXL345, read the datasheet!
char POWER_CTL = 0x2D;	//Power Control Register
char DATA_FORMAT = 0x31;
char DATAX0 = 0x32;	//X-Axis Data 0
char DATAX1 = 0x33;	//X-Axis Data 1
char DATAY0 = 0x34;	//Y-Axis Data 0
char DATAY1 = 0x35;	//Y-Axis Data 1
char DATAZ0 = 0x36;	//Z-Axis Data 0
char DATAZ1 = 0x37;	//Z-Axis Data 1
char values[10]; //This buffer will hold values read from the ADXL345 registers.

int x,y,z; //These variables will be used to hold the x,y and z axis accelerometer values.
int calibrationCount=12; // # times called calibrate fxn

uint8_t numResults=100; //Number of readings to write out

int8_t mode = 0; //Operating Mode; 0=calibrate, 1=monitor, 2= collect, 3=flush and send buffer

//# times each mode should execute
int monitorDelay=500;
int callibrationStartDelay=1000;
int writeDelay=25;
int monitorCount=0; //used to determine when to call processSMS()
int quakeDelta=10; //used to determine when to call processSMS()
int smsSend=0;  //by default SMS messages are disabled

//average z used to determine if event occurred
int zAverage = 0;
//EEPROM Config
int address;
int startAddress = 99; // start address for earthquake data, provides 100 bytes of configuration data

//Fona variables
uint8_t type; //Fona Type
char sendto[21]="4158069938"; //phone number to send SMS
char replybuffer[255]; //character buffer used for SMS communications

void setup(){ 
 //Create a serial connection to display the data on the terminal.
  Serial.begin(9600);

  resetError(); //resetting error counter
  
//Initialize FONA
  Serial.println(F("Start-up..."));
  fonaSerial->begin(4800);
  if (! fona.begin(*fonaSerial)) {
    Serial.println(F("Couldn't find FONA"));
    addError ();
    while (1);
  }
  type = fona.type();
  strcpy(replybuffer,"FONA is OK");
  sendSMS(sendto);
  // Print SIM card IMEI number.
  char imei[15] = {0}; // MUST use a 16 character buffer for IMEI!
  uint8_t imeiLen = fona.getIMEI(imei);
  if (imeiLen > 0) {
    Serial.print(F("SIM card IMEI: ")); 
    Serial.println(imei);
  }

  //initialize GPRS, required to get network time
  initGPRS();
  //Initiate an SPI communication instance.
  SPI.begin();
  //Configure the SPI connection for the ADXL345.
  SPI.setDataMode(SPI_MODE3);  
  //Set up the Chip Select pin to be an output from the Arduino.
  pinMode(CS, OUTPUT);
  //Before communication starts, the Chip Select pin needs to be set high.
  digitalWrite(CS, HIGH);  
  //Put the ADXL345 into +/- 2G range by writing the value 0x01 to the DATA_FORMAT register.
  writeRegister(DATA_FORMAT, 0x00);  
  //Put the ADXL345 into Measurement Mode by writing 0x08 to the POWER_CTL register.
  writeRegister(POWER_CTL, 0x08);  //Measurement mode  
  deleteSMS(); //clean-up any SMS's on SIM card
}

void loop(){
  
  if (mode==0) {
    delay(callibrationStartDelay);
    calibrateADXL(calibrationCount);
  }
  else        {
    monitorEvent(monitorDelay);
    
    //drives frequency of SMS processing
    if (monitorCount <25) {monitorCount=monitorCount+1; } else {processSMS();
                                                                monitorCount=0;}
      }

}

void calibrateADXL (int calCount) {
        Serial.println(F("Calibrating"));
        delay(10);  //add small delay to ADXL reading
        int zPrior = getReading();
        zAverage=0; //reset zAverage

        for (int count = 0; count<calCount; count++) {      
          zAverage=calcAvgAccel (getReading(),count,zAverage);
          delay(100); //short delay for callibration
        }
        mode=1;
        Serial.println(F("Monitoring"));
}
  
void monitorEvent(int mdelay) {
      int8_t delta = getReading() - zAverage;
      delta = abs(delta); //cannot do math calcs in abs() 
      printResults(); //print to terminal current x,y,z readings                                  

      if (delta > quakeDelta)  {
                               writeEventLog(numResults,startAddress); //write out event log
                           if (smsSend==1) {
                              strcpy(replybuffer,"Event detected");
                              sendSMS(sendto);
                              setCharMessage(delta);
                              sendSMS(sendto);
                              sendEventLog(numResults); //send event log
                                                 }
                                  }      
      delay(mdelay);
}

void writeEventLog(int count, int start) {

       Serial.println(F("Write to eeprom"));
       int address=startAddress;
       
       for (int writeCount = 0; writeCount<count; writeCount++) {
         EEPROM.write(address, getReading()); //write to current address
         address = address + 1;  // advance to the next address
         if (address == 512) {break;} // Stop writing if address is out of bounds of EEPROM      
         Serial.println(z, DEC);         
         delay(writeDelay);
       }
       Serial.println(F("Finished Writing"));
       mode=0; //re-callibrate ADXL      
}

void sendEventLog (int count) {
          Serial.println(F("Transmitting")); 
          int messageCounter=0; //counter used to break down messages into 140 byte chunks
          String value;
          char cValue[4]; //signed three digit character array
          int address=startAddress;
          int8_t valueInt; //value read which is appended to message, supports range of -128 to 127
                  
       for (int readCount = 0; readCount<count; readCount++) {

          valueInt = EEPROM.read(address); //get ADXL value
          value = String(valueInt,DEC);//convert int to string, length of 3 to 4 characters
          value.toCharArray(cValue,4); //copy value to CValue

          int i=0;
          while(i<value.length())
            {
            replybuffer[messageCounter+i]    = cValue[i];
            ++i;
            }

            replybuffer[ messageCounter + value.length()] = ',';  //add comma to seperate ADXL values
            messageCounter=messageCounter+value.length()+1;       //increment message counter by messageCounter+ lenght()

        if (messageCounter>140) {

          if (smsSend==1) {sendSMS(sendto);}
              messageCounter=0;                                
        }
        
              address = address + 1;  // advance to the next address
       }
              if (smsSend==1) {sendSMS(sendto);
                                    }
              mode=0;
}

void processSMS () {

  // read the number of SMS's!

          int8_t smsnum = fona.getNumSMS();

        if (smsnum < 0) {
          Serial.println(F("No SMS's"));
        } else {
          Serial.print(smsnum);
          Serial.println(F(" SMS's on SIM card!"));
        }

  for (int smsMessage = 1; smsMessage <=smsnum; smsMessage++) {

        // Retrieve SMS sender address/phone number.
        if (! fona.getSMSSender(smsMessage, replybuffer, 250)) {
          Serial.println(F("Failed!"));
          addError ();
        } else {  

                Serial.print(F("FROM: ")); 
                Serial.println(replybuffer);

                // Retrieve SMS value.
                uint16_t smslen;
                if (! fona.readSMS(smsMessage, replybuffer, 250, &smslen)) { // pass in buffer and max len!
                Serial.println(F("Failed to read SMS"));
                addError ();
                }

                //control type  
                char controlType[2];
                controlType[0]=replybuffer[0];
                controlType[1]=replybuffer[1];
                controlType[2]=replybuffer[2];
        
                Serial.print(F("Control Type:")); 
                Serial.println(controlType);

                //control value
                char controlValue;
                controlValue= replybuffer[4];

                deleteSMS(smsMessage); //remove the SMS message from queue
        
                if (!setControl(controlType,controlValue,smsMessage)) {
                    strcpy(replybuffer,"Stupid commands!");
                    sendSMS (sendto);
                    }
        }//close else clause
  } //close for loop
}

boolean setControl (char controlT[2], char controlV, int smsMessage) {
  boolean match;

  if (strcmp(controlT, "Nmr")  == 0) {  
        setNumResults(controlV); //write ASCII value for # records
        strcpy(replybuffer,"Updating Results");
        sendSMS (sendto);
        match=true;
    } else if 
     (strcmp(controlT, "Sdl")  == 0) {  
        setWriteDelay(controlV); //write ASCII value for # records
        strcpy(replybuffer,"Write Delay");
        sendSMS (sendto);
        match=true;
    } else if 
     (strcmp(controlT, "Qde")  == 0) {  
        setQuakeDelta(controlV); //write ASCII value for quake delay
        strcpy(replybuffer,"Quake Delta");
        sendSMS (sendto);
        match=true;
     }
        else if 
     (strcmp(controlT, "Cde")  == 0) {  
        callibrationDelay(controlV); //write ASCII value for quake delay
        strcpy(replybuffer,"Calibration Delay");
        sendSMS (sendto);
        match=true;
    }   else if 
     (strcmp(controlT, "Res")  == 0) {  
        enableSMSSend();// set to SMS send mode
        strcpy(replybuffer,"Collecting");
        sendSMS (sendto);
        writeEventLog(numResults,startAddress); //write out event log and send
        sendEventLog(numResults); //send event log
        match=true;
    } else if 
     (strcmp(controlT, "SMS")  == 0) {  
        enableSMSSend(); //write out event log and send
        strcpy(replybuffer,"SMS enabled");
        sendSMS (sendto); 
        match=true;
    } else if 
     (strcmp(controlT, "NMS")  == 0) {  
        disableSMSSend(); //write out event log and send
        strcpy(replybuffer,"SMS disabled");
        sendSMS (sendto); 
        match=true;
    } else if 
     (strcmp(controlT, "Rss")  == 0) {
       getRSSI(); //copy signal to reply buffer
       sendSMS (sendto); 
       match=true;
    } else if 
     (strcmp(controlT, "Bat")  == 0) {
       getBat(); //copy signal to reply buffer
       sendSMS (sendto); 
       match=true;
    }
    else if 
     (strcmp(controlT, "Gtm")  == 0) {  
       getTime(); //copy time to replybuffer
       sendSMS (sendto); 
       match=true;
    }
    else if 
     (strcmp(controlT, "Err")  == 0) {  
       getError();
       sendSMS (sendto); 
       match=true;
    }
    else if 
     (strcmp(controlT, "Rst")  == 0) {  
        strcpy(replybuffer,"Restarting Fona");
        sendSMS (sendto); 
        deleteSMS(smsMessage); //remove the SMS message from queue
        delay(50);
        softReset(); 
    }
  else {match=false;}
 return match;
} 

void softReset() // Restarts program from beginning but does not reset the peripherals and registers
{
asm volatile ("  jmp 0");  
}

void setNumResults (char controlV) {
    int cV = (int)controlV;
    cV = (controlV/33*20) + (controlV-33)*2;
    numResults=cV; 
}

void setWriteDelay (char controlV) {
  int cV = (int)controlV;
  cV = (controlV/33*10) + (controlV-33)*2;
  writeDelay=cV; 
}

void setQuakeDelta (char controlV) {
  int cV = (int)controlV;
  cV = (controlV/33*10) + (controlV-33)*1;
  quakeDelta=cV; 
}

void callibrationDelay (char controlV) {
  int cV = (int)controlV;
  cV = (controlV/33*1000) + (controlV-33)*13;
  callibrationStartDelay=cV; 
}

void enableSMSSend () {
  smsSend=1;
}

void disableSMSSend () {
  smsSend=0;
}

void getError () {
    int8_t numResultInt8_t = EEPROM.read(3); // read a byte from the current address
    setCharMessage(numResultInt8_t);
    Serial.println(F("Get Error"));    
    Serial.println(numResultInt8_t);       
}

void addError () {
    int8_t numResultInt8_t = EEPROM.read(3); // read a byte from the current address
    ++numResultInt8_t;
    EEPROM.write(3,numResultInt8_t); // write new value
    Serial.println(F("Adding Error"));
    Serial.println(numResultInt8_t);
    
}

void resetError () {
    EEPROM.write(3,0); // write new value
}

void deleteSMS() {

        int8_t smsnum = fona.getNumSMS();

        if (smsnum < 0) {
          Serial.println(F("No SMS's"));
        } else {
          Serial.print(smsnum);
          Serial.println(F("SMS's on SIM card!"));
        }

  for (int smsMessage = 1; smsMessage <=smsnum; smsMessage++) {
          deleteSMS(smsMessage);
  }
}

    
void deleteSMS(int smsn) {
        if (fona.deleteSMS(smsn)) {
          Serial.println(F("Delete SMS"));
        } else {
          Serial.println(F("Couldn't delete"));
          addError ();
        }

}

void initGPRS () {
        // turn GPRS on
        if (!fona.enableGPRS(true)) {Serial.println(F("Failed to turn on GPRS"));
                                      addError ();
                                    }
        // enable NTP time sync
        if (!fona.enableNTPTimeSync(true, F("pool.ntp.org"))){Serial.println(F("Failed to enable NTP time sync"));
                                                              addError ();
                                                              }        
}

void getRSSI () {
        // read the RSSI
        uint8_t n = fona.getRSSI();
        int8_t r;
        if (n == 0) r = -115;
        if (n == 1) r = -111;
        if (n == 31) r = -52;
        if ((n >= 2) && (n <= 30)) {
          r = map(n, 2, 30, -110, -54);
        }
        
        setCharMessage(r);

}

void getBat () {

        // read the battery voltage and percentage
        uint16_t vbat;
        if (! fona.getBattVoltage(&vbat)) {
          Serial.println(F("Failed to read Batt"));
        } else {
          Serial.print(F("VBat = ")); Serial.print(vbat); Serial.println(F(" mV"));
          setIntMessage(vbat);
        }  
  
}

void setIntMessage (uint16_t r) {
          memset(replybuffer, 0, 255);
          int messageCounter=0; 
          String value = String(r,DEC);
          char cValue[6];
          value.toCharArray(cValue,6); //copy value to CValue

       int i=0;
          while(i<value.length())
            {
            replybuffer[messageCounter+i]    = cValue[i];
            ++i;
            }
  
}

void setCharMessage (int8_t r) {
          memset(replybuffer, 0, 255);
          int messageCounter=0; 
          String value = String(r,DEC);
          char cValue[4];
          value.toCharArray(cValue,4); //copy value to CValue

       int i=0;
          while(i<value.length())
            {
            replybuffer[messageCounter+i]    = cValue[i];
            ++i;
            }
  
}

void getTime () {
        char buffer[23];
        fona.getTime(buffer, 23);  // make sure replybuffer is at least 23 bytes!
        replybuffer[0]=buffer[10];
        replybuffer[1]=buffer[11];
        replybuffer[2]=buffer[12];
        replybuffer[3]=buffer[13];
        replybuffer[4]=buffer[14];
        replybuffer[5]=buffer[15];
        replybuffer[6]=buffer[16];
        replybuffer[7]=buffer[17];
        replybuffer[8]='\0';
}

void sendSMS (char recipient[21]) {
      Serial.println(F("Send SMS"));

       if (!fona.sendSMS(recipient, replybuffer)) {Serial.println(F("Failed to send"));
                                                   fona.sendSMS(recipient, replybuffer); //retry
                                                   addError ();
                                                   } 
       else {Serial.println(F("Sent!"));
             memset(replybuffer, 0, 255);
        }
}

int8_t getReading () {
  //Reading 6 bytes of data starting at register DATAX0 will retrieve the x,y and z acceleration values from the ADXL345.
  //The results of the read operation will get stored to the values[] buffer.
  readRegister(DATAX0, 6, values);
  //The ADXL345 gives 10-bit acceleration values, but they are stored as bytes (8-bits). To get the full value, two bytes must be combined for each axis.
  //The X value is stored in values[0] and values[1].
  x = ((int)values[1]<<8)|(int)values[0];
  //The Y value is stored in values[2] and values[3].
  y = ((int)values[3]<<8)|(int)values[2];
  //The Z value is stored in values[4] and values[5].
  z = ((int)values[5]<<8)|(int)values[4];

  z = z * 0.39; //scale to g's 

//Implement filter to keep ADXL values between int8_t range -128 to 127
  if (z<-128) 
        {z=-128;} 
      else if (z>127) 
          {z=127;}
  return z;  
}

int calcAvgAccel (int z, float index, int currAvg) {
  float weightNew = 1/index;
  float weightOld = (1-weightNew);
  int avg = z*weightNew+currAvg*weightOld;
  return avg;  
}

void printResults () {
  //Print the results to the terminal.
  Serial.print(F("z val:"));
  Serial.print(z, DEC);
  Serial.print(',');
  Serial.print(F("z Avg:"));
  Serial.print(zAverage,DEC);
  Serial.print(',');
  Serial.print(F("z Diff:"));
  Serial.println(z-zAverage,DEC);
}  

//This function will write a value to a register on the ADXL345.
//Parameters:
void writeRegister(char registerAddress, char value){
  //Set Chip Select pin low to signal the beginning of an SPI packet.
  digitalWrite(CS, LOW);
  //Transfer the register address over SPI.
  SPI.transfer(registerAddress);
  //Transfer the desired register value over SPI.
  SPI.transfer(value);
  //Set the Chip Select pin high to signal the end of an SPI packet.
  digitalWrite(CS, HIGH);
}


//This function will read a certain number of registers starting from a specified address and store their values in a buffer.
//Parameters:
//  char registerAddress - The register addresse to start the read sequence from.
//  int numBytes - The number of registers that should be read.
//  char * values - A pointer to a buffer where the results of the operation should be stored.
void readRegister(char registerAddress, int numBytes, char * values){
  //Since we're performing a read operation, the most significant bit of the register address should be set.
  char address = 0x80 | registerAddress;
  //If we're doing a multi-byte read, bit 6 needs to be set as well.
  if(numBytes > 1)address = address | 0x40;
  
  //Set the Chip select pin low to start an SPI packet.
  digitalWrite(CS, LOW);
  //Transfer the starting register address that needs to be read.
  SPI.transfer(address);
  //Continue to read registers until we've read the number specified, storing the results to the input buffer.
  for(int i=0; i<numBytes; i++){
    values[i] = SPI.transfer(0x00);
  }
  //Set the Chips Select pin high to end the SPI packet.
  digitalWrite(CS, HIGH);
}
