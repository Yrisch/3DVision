//**********************************************
//  Appli timer pour commander des lunettes 3D
// met en oeuvre :
// timer3 en mode fast PWM OCR3A=TOP (mode 15)
//    - irq OCR3B (front descendant) : irq pour la comptage des pulse PWM ; elle arrive en cours de pulse.
//    - traitement irq : arret du compage (prescal=0) en cours de pulse : TOP reste inchangé
//    - TOP (la freq du PWM) est remis à jour lorsque TCNT3=TOP
//    - astuce pour synchroniser : mettre à jour manuellement TCNT3 quelques pulses avant TOP (ici 5clk @16MHz = 5*63ns) ==> Il doit y avoir mieux à faire
//
// timer5 en mode 4 ctc autoreload avec sortie en toggle on compare match.
//    - la fréquence du SQW est donc la moitié du temps de comptage
//    - la fréquence des irq est elle double du SQW donc = au temps de comptage
//    - ici sert à simuler la fréquence video


#define SW_VERSION "<VERSION:0.4a>"
//#include <digitalWriteFast.h>

//###############################################"
// DEFINE
// !! bug dans digitalWriteFast.h : __atomicWrite__ le cas if ne marche pas ?
// un write sur un port d'adr < 0x3F est plus rapide de ~0.5 à 1µs ?
#define __digitalWrite digitalWrite
#define __digitalRead  digitalRead

#define OC3APin5 5
#define OC3BPin2 2
#define OC3CPin3 3
#define OC5CPin46 46
#define probAPin 14
#define probBPin 15
#define probCPin 16

//###############################################
// GLOB DATA
unsigned long ulLoopCnt=0;
boolean bDtEnableExtSync = false;
volatile uint8_t ucNbPulses=0;
volatile boolean bToggleGlass=true;
String strDataSerialPc;

//###############################################"
#define Serial_Pc_CtrlBox Serial  // Serial = port Serie USB
#define fnComReplyPC(a)  Serial_Pc_CtrlBox.println(a)

void setup()
{
  Serial_Pc_CtrlBox.begin(115200);
  fnComReplyPC("Epg...");
  pinMode(OC3APin5, OUTPUT);
  pinMode(OC3BPin2, OUTPUT);
  pinMode(OC3CPin3, OUTPUT);
  pinMode(OC5CPin46, OUTPUT);
  pinMode(probAPin, OUTPUT);
  pinMode(probBPin, OUTPUT);
  pinMode(probCPin, OUTPUT);
  __digitalWrite(probAPin,LOW);
  __digitalWrite(probBPin,LOW);
  __digitalWrite(probCPin,LOW);
  //***********************************
  //
  //***********************************
  //probAPin_port = portOutputRegister(digitalPinToPort(probAPin));
  //probAPin_mask = digitalPinToBitMask(probAPin);
  fnHoldTimer0();
  fnSetTimer5Tick1ms();
  fnSetTimer3();
}

void loop()
{
  //**************
  ulLoopCnt++;
  //**************
  fnComPc();
  //**************
  if (bDtEnableExtSync) {
  }
}

//***********************************************
ISR(TIMER5_COMPA_vect)  // mode 4 : CTC=TOP=OCRnA (si COMnA1,COMnA0 = 10)
{
  static unsigned char ucBiToggle = 0;
  __digitalWrite(probAPin,bToggleGlass);
  ucBiToggle++;
  if (ucBiToggle < 2) {
    fnSetT3Right();
  } else if (ucBiToggle < 3) {
    fnSetT3Left();
    ucBiToggle = 0;
  }
  bToggleGlass = !bToggleGlass;
  //__digitalWrite(probAPin,LOW);
}

//***********************************************
// mode 9 : deux IRQ par periode @ OCM=TOP=OCR3A (si COM3A1,COM3A0 = 10)
// mode 15 : une IRQ par periode @ OCM=TOP=OCR3A puis bottom
ISR(TIMER3_COMPA_vect)  
{
//  __digitalWrite(probAPin,HIGH);
//  __digitalWrite(probAPin,LOW);
}

//***********************************************
ISR(TIMER3_COMPB_vect)
// mode 9 : deux IRQ par periode @ OCM=OCR3B (si COM3A1,COM3A0 = 10)
// mode 15 : tombe au front desc de OCM (si COM3A1,COM3A0 = 10)
{
  __digitalWrite(probBPin,HIGH);
  if(ucNbPulses > 1) ucNbPulses--;
  else fnHoldTimer3();
  __digitalWrite(probBPin,LOW);
}

//***********************************************
ISR(TIMER3_COMPC_vect)
{
}

//***********************************************
ISR(TIMER3_OVF_vect)  // en mode 9 : TOV=Bottom
{
//  __digitalWrite(probCPin,HIGH);
//  __digitalWrite(probCPin,LOW);
}


void fnSetTimer5Tick1ms()
{
  // CLI
  uint8_t register saveSreg = SREG;
  cli();

  // COM5A1,COM5A0,COM5B1,COM3B0,COM5C1,COM5C0,WGM51,WGM50
  // (10 & 11) : toggle on compare match ; mode 9 : 01=toggle, 10=toujours=1, 11=toujours=0
  TCCR5A = 0b01000000;
  // ICNC5,ICES5,-,WGM53,WGM52,CS52,CS51,CS50
  // IC: input capture, NC :noise cancel ES : edge select
  // mode4 => WGM=0100=ctc (auto reload) ; top=OCR5A = (F_CPU / frequency / 2 / prescaler)-1 ; ex PWM@1kHz=>top=124 pre=64
  // prescaler, CS5x : 0 timer stop, 1..5 = /(1, 8, 64, 256, 1024)
  // Fcpu=16M
  TCCR5B = 0b00001100;
  // FOCA,FOCB,FOCC,nu,nu,nu,nu,nu
  TCCR5C = 0b00000000;
  // 
  //OCR5A = 0x007C;  // @1kHz 124=0x7C = 500µs entre chaque IRQ
  //OCR5A = 0x0103;  // @240Hz = 259 => 41667µs entre chaque IRQ
  OCR5A = 0x0207;  // @120Hz 519 = 0x207 => 8300µsentre chaque
  //
  OCR5B = 0x0000;
  //
  OCR5C = 0x0000;
  // nu,nu,ICIE3,nu,OCIE3C,OCIE3B,OCIE3A,TOIE3
  TIFR5  = 0; // clear all pending timer3 interrupt
  TIMSK5 = 0b00000010;
  
  // synchronize TCNTn
  uint8_t register saveUByte = TCCR5B;
  TCCR5B = 0;
  TCNT5 = 0;
  TCCR5B = saveUByte;  
  // restore IRQ
  SREG = saveSreg;
}


void fnSetTimer3()
{
  // CLI
  uint8_t register saveSreg = SREG;
  cli();

  // COM3A1,COM3A0,COM3B1,COM3B0,COM3C1,COM3C0,WGM31,WGM30
  // mode 9 : (10 & 11) = toggle on compare match ; 01=toggle OCnA, 10=toujours=1, 11=toujours=0
  // mode 15 : (10 & 11) = clear/set on compare match, set/clear at bottom ; 01=toggle OCnA, , 10=toujours=1, 11=toujours=0
  TCCR3A = 0b01100011;
  // ICNC3,ICES3,-,WGM33,WGM32,CS32,CS31,CS30
  // IC: input capture, NC :noise cancel ES : edge select
  // mode9  => WGM=1001=PWM ph&freq correct ; top=OCR3A = F_CPU / frequency (or *period) / 2 / prescaler ; ex PWM@10kHz=>top=800
  // mode15 => WGM=1111=Fast ; top=OCR3A = (F_CPU / frequency / prescaler)-1 ; ex PWM@10kHz=>top=1599=0x063F ; ex2 PWM@38µs=>top=607=0x25F
  // prescaler, CS3x : 0 timer stop, 1..5 = /(1, 8, 64, 256, 1024)
  // Fcpu=16M
  TCCR3B = 0b00011000;
  // FOCA,FOCB,FOCC,nu,nu,nu,nu,nu
  TCCR3C = 0b00000000;
  // 
  OCR3A = 0x025F;  // PWM@38µs=>top=607=0x25F
  //
  OCR3B = 0x0120;  //16*18µs=288=0x120
  //
  OCR3C = 0x0000;
  // resynchronize TCNT3
  TCNT3 = 0;
  // nu,nu,ICIEn,nu,OCIEnC,OCIEnB,OCIEnA,TOIEn
  // mode 15 : OCnB=1IRQ@OCB@freq, OCnA=1IRQ@OCA@freq, TOVn=OCnA (mais apres si les deux sont actives)
  TIFR3  = 0; // clear all pending timer3 interrupt
  TIMSK3 = 0b00000100;
  // restore IRQ
  SREG = saveSreg;
}

void fnSetT3Right()
{
  // CLI
  uint8_t register saveSreg = SREG;
  cli();
  ucNbPulses = 3;
  
  // resynchronize TCNT3
  TCCR3B |= 0b00000001;  // pour relancer le comptage sinon pb sur mise à jour TCNTn
  //TCNT3 = OCR3A-20;
  // 
  OCR3A = 0x025F;  // PWM@38µs=>top=607=0x25F
  //
  OCR3B = 0x0120;  // 16*18µs=288=0x120

  TCNT3 = 0x04DF-5;
  
  // restore IRQ
  SREG = saveSreg;
}

void fnSetT3Left()
{
  // CLI
  uint8_t register saveSreg = SREG;
  cli();
  ucNbPulses = 2;
  
  // resynchronize TCNT3
  TCCR3B |= 0b00000001;     // pour relancer le comptage sinon pb sur mise à jour TCNTn
  //TCNT3 = OCR3A-20;
  // 
  OCR3A = 0x04DF;  // PWM@78µs=>top=1247=0x4DF
  //
  OCR3B = 0x0120;  // 16*18µs=288=0x120

  TCNT3 = 0x025F-5;
  
  // restore IRQ
  SREG = saveSreg;
}

void fnHoldTimer0()
{
  // CLI
  uint8_t register saveSreg = SREG;
  cli();
  TCCR0B &= 0b11111000;  
  // restore IRQ
  SREG = saveSreg;
}


void fnHoldTimer3()
{
  // CLI
  uint8_t register saveSreg = SREG;
  cli();
  TCCR3B &= 0b11111000;  
  // restore IRQ
  SREG = saveSreg;
}

void fnComPc(){
  int nInd;
  if(fnComPcWatcher()){
    String strParse;
    if(strDataSerialPc == "<VERSION>"){
      fnComReplyPC(SW_VERSION);
    }
    else if (strDataSerialPc == "<AR>"){
      fnComReplyPC("<OK>");
    }
    else if (strDataSerialPc == "<ST>"){
      //fnSetTimer3();
      fnComReplyPC("<OK>");
    }
    else if (strDataSerialPc == "<SP>"){
      fnHoldTimer3();
      fnComReplyPC("<OK>");
    }
    
    else if (strDataSerialPc.startsWith("<MANU:")){
      nInd = strDataSerialPc.indexOf(':');
      strParse = strDataSerialPc.substring(nInd+1);
      int steps = strParse.toInt();
      fnComReplyPC("<OK>");
    }
    else if (strDataSerialPc == "<MEM>"){
         String strReply = "<";
         strReply += freeRam();
         strReply += ">";
         fnComReplyPC(strReply);
    }
    else {
      fnComReplyPC("<UNKNOW>");
    }
  }
}


inline boolean fnComPcWatcher()
{
  boolean bRet = false;
  static char incomingBytePc = '\0';     // for incoming serial data
  static char dataComPc[32] = "";        // buffer for raw data
  static byte bufflen = 0;
  // if data is available on serial port
  while (Serial_Pc_CtrlBox.available() > 0)
  {
  // read one byte
    incomingBytePc = Serial_Pc_CtrlBox.read();
    if (incomingBytePc == '<')  { // if start of frame
      bufflen = 0;
    }
    dataComPc[bufflen] = incomingBytePc;
    bufflen++;
    if (bufflen >= sizeof(dataComPc)) {
      bufflen=0;
      Serial_Pc_CtrlBox.println("comRaspi overrun");
    }
    //if ((dataComPc[0] == '<') && (dataComPc[bufflen-1] == '>')) { // si Fin trame
    if ((dataComPc[0] == '<') && (dataComPc[bufflen-1] == '>')) { // si Fin trame
        dataComPc[bufflen] = '\0';          //on fini le tableau par un char nul pour construire un string
        strDataSerialPc = dataComPc;            //transforme le tableau en string
        bufflen=0;
        bRet = true;
        break;

    }
  }
  return(bRet);
}

int freeRam()
{
  extern int __heap_start, *__brkval;
  int v;
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval);
}
