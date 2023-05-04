#include <LiquidCrystal.h>
#include <SoftwareSerial.h>
#include <SPI.h>
#include <MFRC522.h>
#include <String.h>
#include "Timer.h"

#define RST_PIN 9
#define SDA_PIN 10
#define ROWS 4
#define COLS 3

char layout[ROWS][COLS] = {
        {'1','2','3'},
        {'4','5','6'},
        {'7','8','9'},
        {'*','0','#'}
};
byte row[ROWS] = {A0, A1, A2, A3}; 
byte col[COLS] = {A4, A5, 6}; 

bool BTUnlocked = false;
bool RFIDUnlocked = false;
bool keypadUnlocked = false;
bool keypadReset = false;
bool lockdown = false;
bool keypadUpdated = false;
bool displaying = false;

MFRC522 RFID_Reader(SDA_PIN, RST_PIN);
String PASSWORD = "1234";
String RFID_KEY = "8A 0A A4 80";
String BT_KEY = "56789";
String BTRead = "";
String RFIDRead = "";
String keypadInput = "";
String message = "";
String pswd = "";
int tries = 0;

const int rs = 8, en = 7, d4 = 5, d5 = 4, d6 = 3, d7 = 2;
LiquidCrystal LCD(rs, en, d4, d5, d6, d7);

enum MainSM_States {MainSM_Start, MainSM_Init, MainSM_Unlock, MainSM_Reset, MainSM_Lock, MainSM_Lockdown};
enum KeypadSM_States {KeypadSM_Start, KeypadSM_Read};
enum BTSerialSM_States {BTSerialSM_Start, BTSerialSM_Wait, BTSerialSM_Read};
enum RFIDReaderSM_States {RFIDReaderSM_Start, RFIDReaderSM_Wait, RFIDReaderSM_Read};
enum LCDUpdateSM_States {LCDUpdateSM_Start, LCDUpdateSM_DispMsg, LCDUpdateSM_Wait, LCDUpdateSM_DispPswd, LCDUpdateSM_Lockdown};

String keypadStateTeller(int state){
    switch(state){
        case KeypadSM_Start:
            return "KeypadSM_Start";
        case KeypadSM_Read:
            return "KeypadSM_Read";
        default:
            return "Cannot be determined: " + String(state);
    }
}

String RFIDStateTeller(int state){
    switch(state){
        case RFIDReaderSM_Start:
            return "RFIDReaderSM_Start";
        case RFIDReaderSM_Wait:
            return "RFIDReaderSM_Wait";
        case RFIDReaderSM_Read:
            return "RFIDReaderSM_Read";
        default:
            return "Cannot be determined: " + String(state);
    }
}

struct task{
    int state;
    unsigned long period;
    unsigned long elapsedTime;
    int (*TickFct)(int);
};

char getChar(){
    for(int i = 0; i < ROWS; i++){
      pinMode(row[i], INPUT_PULLUP);
    }
    for(int i = 0; i < COLS; i++){
    pinMode(col[i], OUTPUT);
    digitalWrite(col[i], LOW);
    for(int j = 0; j < ROWS; j++){
      if(!digitalRead(row[j])){
        digitalWrite(col[i], HIGH);
        pinMode(col[i], INPUT);
        // Serial.println("Got: " + String(layout[j][i]) + "\n");
        return layout[j][i];
      }
    }
    digitalWrite(col[i], HIGH);
    pinMode(col[i], INPUT);
  }
  return '\0';
}

void RFIDReadID(String& ID){
    ID = "";
    for(int i = 0; i < RFID_Reader.uid.size; i++){
        ID += String(RFID_Reader.uid.uidByte[i] < 0x10 ? " 0" : " ");
        ID += String(RFID_Reader.uid.uidByte[i], HEX);
    }
    ID.toUpperCase();
    ID.trim();
}

int TickFct_LCDUpdateSM(int LCDUpdateSM_State){
    static int dispCnt = 0;
    static int lockCnt = 0;
    Serial.print(dispCnt);
    switch(LCDUpdateSM_State){
        case LCDUpdateSM_Start:
            LCDUpdateSM_State = LCDUpdateSM_Wait;
            break;
        case LCDUpdateSM_DispMsg:
            if(dispCnt >= 20){
                LCDUpdateSM_State = LCDUpdateSM_Wait;
                displaying = false;
                dispCnt = 0;
            }
            break;
        case LCDUpdateSM_DispPswd:
            LCDUpdateSM_State = LCDUpdateSM_Wait;
            displaying = false;
        case LCDUpdateSM_Wait:
            if(message.length() > 0){
                LCDUpdateSM_State = LCDUpdateSM_DispMsg;
                LCD.clear();
                if(message.length() > 16){
                    LCD.print(message.substring(0, 16));
                    LCD.setCursor(0, 1);
                    LCD.print(message.substring(16));
                }
                else{
                    LCD.print(message);
                }
                message = "";
                displaying = true;
            }
            if(pswd.length() > 0){
                LCDUpdateSM_State = LCDUpdateSM_DispPswd;
                LCD.clear();
                if(message.length() > 16){
                    LCD.print(message.substring(0, 16));
                    LCD.setCursor(0, 1);
                    LCD.print(message.substring(16));
                }
                else{
                    LCD.print(message);
                }
                pswd = "";
                displaying = true;
            }
            if(lockdown){
                LCDUpdateSM_State = LCDUpdateSM_Lockdown;
            }
            break;
        case LCDUpdateSM_Lockdown:
            if(lockCnt >= 40){
                LCDUpdateSM_State = LCDUpdateSM_Wait;
                lockdown = false;
                displaying = false;
                lockCnt = 0;
            }
            break;
        default:
            LCDUpdateSM_State = LCDUpdateSM_Start;
            break;
    }
    switch(LCDUpdateSM_State){
        case LCDUpdateSM_Start:
            break;
        case LCDUpdateSM_DispMsg:
            dispCnt++;
            break;
        case LCDUpdateSM_DispPswd:
            break;
        case LCDUpdateSM_Wait:
            break;
        case LCDUpdateSM_Lockdown:
            lockCnt++;
            break;
    }
    return LCDUpdateSM_State;
}

int TickFct_MainSM(int mainSM_State){
    static int unlockCounter = 0;
    static int lockdownCounter = 0;
    String temp = "";
    switch(mainSM_State){
        case MainSM_Start:
            mainSM_State = MainSM_Lock;
            break;
        case MainSM_Lock:
            if(BTUnlocked || RFIDUnlocked || keypadUnlocked){
                Serial.println("Unlocking");
                mainSM_State = MainSM_Unlock;
                String message = "UNLOCKED,PRESS * TO RESET";
                unlockCounter = 0;
                tries = 0;
            }
            if(lockdown){
                mainSM_State = MainSM_Lockdown;
            }
            break;
        case MainSM_Unlock:
            if(unlockCounter >= 40 && !keypadReset){
                mainSM_State = MainSM_Lock;
                unlockCounter = 0;
            }
            break;
        case MainSM_Reset:
            if(!keypadReset){
              mainSM_State = MainSM_Lock;
            }
            break;
        case MainSM_Lockdown:
            if(lockdownCounter >= 20){
                mainSM_State = MainSM_Init;
                lockdownCounter = 0;
                lockdown = false;
                tries = 0;
            }
            break;
          break;
        default:
          mainSM_State = MainSM_Start;
          break;
    }
    switch(mainSM_State){
        case MainSM_Start:
            break;
        case MainSM_Init:
            break;
        case MainSM_Lock:
            break;
        case MainSM_Unlock:
            unlockCounter++;
            break;
        case MainSM_Reset:
            break;
        case MainSM_Lockdown:
            lockdownCounter++;
            break;
          break;
        default:
            break;
    }
    return mainSM_State;
}

void TickFct_KeypadSM(int keypadSM_State){
    static char prevKey = '\0';
    // Serial.println("TickFct_KeypadSM");
    switch(keypadSM_State){
        case KeypadSM_Start:
            // Serial.println("KeypadSM_Start Trans");
            keypadSM_State = KeypadSM_Read;
            keypadInput = "";
            break;
        case KeypadSM_Read:
            Serial.println("KeypadSM_Read Trans");
            keypadSM_State = KeypadSM_Read;
            break;
        default:
            // Serial.println("default Trans");
            keypadSM_State = KeypadSM_Read;
            break;
    }
    switch(keypadSM_State){
        case KeypadSM_Start:
            pswd = "PASSWORD:";
            break;
        case KeypadSM_Read:
            // Serial.println("KeypadSM_Read");
            if(displaying){
                break;
            }
            char key = getChar();
            if(key && key != prevKey){
                if(!keypadUnlocked && !lockdown && !keypadReset){
                    if(key == '*' && keypadInput.length() > 0){
                        keypadInput = keypadInput.substring(0, keypadInput.length() - 1);
                    }else if(key == '#' && keypadInput == PASSWORD){
                        keypadUnlocked = true;
                        message = "UNLOCKED, PRESS * TO RESET";
                        keypadInput = "";
                    }else if(key == '#' && keypadInput != PASSWORD){
                        keypadInput = "";
                        tries++;
                        if(tries < 3){
                          message = "WRONG PASSWORD, TRY AGAIN:" + String(tries) + "/3";
                          // pswd = "PASSWORD:";
                        }else{
                          lockdown = true;
                          tries = 0;
                        }
                    }
                    else if(key == '*' && keypadInput.length() > 0){
                        keypadInput = keypadInput.substring(0, keypadInput.length() - 1);
                    }
                    else{
                        keypadInput += key;
                        message = "PASSWORD:" + keypadInput;
                    }
                }else{
                    if(key == '*'){
                        keypadReset = true;
                        keypadUnlocked = false;
                        keypadInput = "";
                        message = "NEW PASSWORD:";
                    }
                }
                if(keypadReset){
                    if(key == '*' && keypadInput.length() > 0){
                        keypadInput = keypadInput.substring(0, keypadInput.length() - 1);
                    }else if(key == '#' && keypadInput.length() > 0){
                        PASSWORD = keypadInput;
                        keypadReset = false;
                        keypadInput = "";
                        message = "PASSWORD RESET";
                    }else{
                        keypadInput += key;
                    }
                }
            }
            prevKey = key;
            break;
        default:
            break;
    }
    // Serial.println(keypadStateTeller(keypadSM_State));
    return keypadSM_State;
}

int TickFct_BTSerialSM(int BTSerialSM_State){
    switch(BTSerialSM_State){
        case BTSerialSM_Start:
            BTSerialSM_State = BTSerialSM_Wait;
            break;
        case BTSerialSM_Wait:
            if(Serial.available()){
                BTRead = Serial.readStringUntil('#');
                BTSerialSM_State = BTSerialSM_Read;
            }
            break;
        case BTSerialSM_Read:
            if(!Serial.available()){
                BTSerialSM_State = BTSerialSM_Wait;
                BTUnlocked = false;
            }
            break;
        default:
            BTSerialSM_State = BTSerialSM_Start;
            break;
    }
    switch (BTSerialSM_State){
        case BTSerialSM_Start:
            break;
        case BTSerialSM_Wait:
            break;
        case BTSerialSM_Read:
            Serial.print(BTRead);
            if(BTRead == BT_KEY){
                BTUnlocked = true;
            }
            break;
        default:
            break;
    }
    return BTSerialSM_State;
}

int TickFct_RFIDReaderSM(int RFIDReaderSM_State){
    switch(RFIDReaderSM_State){
        case RFIDReaderSM_Start:
            RFIDReaderSM_State = RFIDReaderSM_Wait;
            break;
        case RFIDReaderSM_Wait:
            // Serial.println("Wait");
            if(RFID_Reader.PICC_IsNewCardPresent() && RFID_Reader.PICC_ReadCardSerial()){
                RFIDReadID(RFIDRead);
                RFIDReaderSM_State = RFIDReaderSM_Read;
            }
            break;
        case RFIDReaderSM_Read:
            // Serial.println("Read!");
            if(!RFID_Reader.PICC_IsNewCardPresent() || !RFID_Reader.PICC_ReadCardSerial()){
                RFIDReaderSM_State = RFIDReaderSM_Wait;
                RFIDUnlocked = false;
            }
            break;
        default:
            RFIDReaderSM_State = RFIDReaderSM_Start;
            break;
    }
    switch(RFIDReaderSM_State){
        case RFIDReaderSM_Start:
            break;
        case RFIDReaderSM_Wait:
            break;
        case RFIDReaderSM_Read:
            if(RFIDRead == RFID_KEY){
                RFIDUnlocked = true;
            }
            break;
        default:
            break;
    }
    return RFIDReaderSM_State;
}

task tasks[5];

void setup(){
    Serial.begin(9600);
    TimerSet(100);
    TimerOn();
    SPI.begin();
    RFID_Reader.PCD_Init();
    delay(4);
    LCD.begin(16, 2);
    // RFID_Reader.PCD_DumpVersionToSerial();

    unsigned char i = 0;
    tasks[i].state = MainSM_Start;
    tasks[i].period = 500;
    tasks[i].elapsedTime = tasks[i].period;
    tasks[i].TickFct = &TickFct_MainSM;
    i++;
    tasks[i].state = KeypadSM_Start;
    tasks[i].period = 100;
    tasks[i].elapsedTime = tasks[i].period;
    tasks[i].TickFct = &TickFct_KeypadSM;
    i++;
    tasks[i].state = BTSerialSM_Start;
    tasks[i].period = 500;
    tasks[i].elapsedTime = tasks[i].period;
    tasks[i].TickFct = &TickFct_BTSerialSM;
    i++;
    tasks[i].state = RFIDReaderSM_Start;
    tasks[i].period = 500;
    tasks[i].elapsedTime = tasks[i].period;
    tasks[i].TickFct = &TickFct_RFIDReaderSM;
    i++;
    tasks[i].state = LCDUpdateSM_Start;
    tasks[i].period = 500;
    tasks[i].elapsedTime = tasks[i].period;
    tasks[i].TickFct = &TickFct_LCDUpdateSM;

}

void loop(){
    //Tick functions here
    for(unsigned int i = 0; i < 5; i++){
        // Serial.println("test");
        if(tasks[i].elapsedTime >= tasks[i].period){
            tasks[i].state = tasks[i].TickFct(tasks[i].state);
            tasks[i].elapsedTime = 0;
            // if(i == 1){
                // Serial.println("KeypadSM: " + keypadStateTeller(tasks[i].state));
            // }
        }
        // Serial.println(String(i) + ": " + String(tasks[i].state));
        tasks[i].elapsedTime += 500;
    }
    while(!TimerFlag){}
    TimerFlag = 0;
}