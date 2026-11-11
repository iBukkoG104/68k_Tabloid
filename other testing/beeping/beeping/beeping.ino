#define CE 3                      // enable arduino 
#define BUZZ 10                   // piezo-elettric speaker, connected through D10 - GND

#define DO_LO   264
#define RE      297
#define MI      330
#define FA      352
#define SOL     396
#define LA      440
#define SI      495
#define DO_HI   528
// idk why amerifats use letters for notes, so weird...


int pins[] = {4, 5, 6, 7};        // input multiplexed

void setup() {
  // put your setup code here, to run once:
  pinMode(CE, INPUT);
  pinMode(BUZZ, OUTPUT);

  for(int i = 0; i < (sizeof(pins) / sizeof(pins[0])); i++) {
    pinMode(pins[i], INPUT);
  }

}

void loop() {

  if (digitalRead(CE) == HIGH) {
    byte inputval = (PIND >> 4) & 0x0F;
    switch(inputval) {

      case 0b0000:
        tone(BUZZ, DO_LO);
        delay(20);
        noTone(BUZZ);
        break;
      
      case 0b0001:
        tone(BUZZ, RE);
        delay(20);
        noTone(BUZZ);
        break;

      case 0b0010:
        tone(BUZZ, MI);
        delay(20);
        noTone(BUZZ);
        break;

      case 0b0011:
        tone(BUZZ, FA);
        delay(20);
        noTone(BUZZ);
        break;

      case 0b0100:
        tone(BUZZ, SOL);
        delay(20);
        noTone(BUZZ);
        break;

      case 0b0101:
        tone(BUZZ, LA);
        delay(20);
        noTone(BUZZ);
        break;
      
      case 0b0110:
        tone(BUZZ, SI);
        delay(20);
        noTone(BUZZ);
        break;

      case 0b0111:
        tone(BUZZ, DO_HI);
        delay(20);
        noTone(BUZZ);
        break;
    }
  }
  
  
}
