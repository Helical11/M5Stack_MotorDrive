/*
 * bldc6p.c
 * A test of driving a blushless DC motor with 6-pulse contol by Raspberry Pi
 * Depends on pigpio.h
 * (c) 2021 @RR_Inyo
 * Released under the MIT lisence
 * https://opensource.org/licenses/mit-license.php
 */

#include <M5Stack.h>

/* Define motor and drive parameter */
#define P_PAIR  7       /* Number of pole pairs */
#define F_PWM   20000   /* [Hz], PWM carrier frequency */
#define MOD_I   400000  /* modulaiton index at the start of 6-pulse control: Note unity = 1,000,000 */
#define NUM_F   1000    /* Number of turns in terms for initial forced commutation */
#define MOD_F   400000  /* Modulation index for initial forced commutation: Note unity = 1,000,000 */
#define TICK_F  100     /* [microseconds] to stay in a section */
#define D_MOD   20000   /* Change of modulation index by one command */
#define CHKDLY  50000   /* [microsecond] to wait to check the callback function takes over control */
#define MAF     540     /* Moving average filter to count revolution */

/* Define GPIO pins connected to P-NUCLEO-IHM001 */
/* Hall sensors */
/* Note: H1, H2, and H3 becomes 1 (high) when they detect a south pole. */
#define H1      17
#define H2      35
#define H3      36

/* Enable (deblock) signals */
#define EN1     21
#define EN2     22
#define EN3     16

/* Input (gate) signals */
/* Assign GPIO pins capable of hardware PWM */
/* PWM0: GPIO2, 5 for U- and W-phases V */
/* PWM1: GPIO25 for V-phase */
#define IN1     2
#define IN2     5
#define IN3     26
#define IN1PWM  0
#define IN2PWM  2
#define IN3PWM  4

/* Sector 1 signal */
#define SEC1    25

/* Constant aliases */
 int GB =  0x0;
 int DEB = 0x1;

/* Motor status */
#define STILL   0
#define RUNNING 1

/* Define pigpio parameters */
#define TIMEOUT 100000

/* Define global variables */
int st = STILL;         /* Enable (deblock) status */
int mod = 0;            /* Modulation index */
int called = 0;         /* Flag to indicate the callback function being called */
uint32_t tick_0 = 0;    /* [microsecond] for one electrical cycle */
uint32_t tick_1 = 0;    /* [microsecond] for one electrical cycle, old value */
uint32_t tick_diff[MAF];/* Tick difference buffer */
int k = 0;              /* Index for tick difference buffer */

/* Define function protorypes */
void setGPIO();
void processCommand();
void forcedCommutate(unsigned num, unsigned polePair, uint32_t tick_f);
void produceSignal(unsigned sector);
void cbDriveMotor(int gpio, int level, uint32_t tick);
void gateBlock();

/* The setup function */
void setup() {
/*PinMode*/
    pinMode(H1,INPUT);
    pinMode(H2,INPUT);
    pinMode(H3,INPUT);
    pinMode(EN1,OUTPUT);
    pinMode(EN2,OUTPUT);
    pinMode(EN3,OUTPUT);
    digitalWrite(EN1,GB);
    digitalWrite(EN2,GB);
    digitalWrite(EN3,GB);

/*The setup PWM pin*/
    ledcSetup(IN1PWM,F_PWM,8);
    ledcSetup(IN2PWM,F_PWM,8);
    ledcSetup(IN3PWM,F_PWM,8);
    pinMode(IN1,OUTPUT);
    pinMode(IN2,OUTPUT);
    pinMode(IN3,OUTPUT);
    ledcAttachPin(IN1,IN1PWM);
    ledcAttachPin(IN2,IN2PWM);
    ledcAttachPin(IN3,IN3PWM);

  M5.begin();
  M5.Power.begin();
  Serial.begin(115200);
  M5.Lcd.fillScreen(WHITE);

}

/* The main function */
void loop() {
    /* Show initial message */
    M5.Lcd.print("=====================================================");
    M5.Lcd.print(" bldc6p - BLDC motor 6-pulse control by Raspberry Pi ");
    M5.Lcd.print(" (c) 2021 @RR_Inyo                                   ");
    M5.Lcd.print("=====================================================");
    M5.Lcd.print("Commands:                                            ");
    M5.Lcd.print("  s: Start motor                                     ");
    M5.Lcd.print("  h: Stop motor                                      ");
    M5.Lcd.print("  r: Raise modulation index                          ");
    M5.Lcd.print("  l: Lower modulation index                          ");
    M5.Lcd.print("  t: Show rotational speed                           ");
    M5.Lcd.print("  e: End this program                                ");

    /* Infinate loop, outer */
    while(1) {

        /* Show prompt to user and wait for start command */
        while(st == STILL) {
            processCommand();
        }

        /* Forced commutation to start up */
        M5.Lcd.print("Trying forced commutation...");
        mod = MOD_F;
        forcedCommutate(NUM_F, P_PAIR, TICK_F);

        M5.Lcd.print("Succeeded in getting into the 6-pulse (120-degree) control mode by ISR callback functions.");

        /* Infinate loop, inner */
        while(st == RUNNING) {
            processCommand();
        }

        /* Gate block */
        gateBlock();
    }
}

/* Function to process command from user */
void processCommand()
{
    int c = 0;
    int j = 0;
    uint64_t tick_ave = 0;

    /* Show prompt */
    M5.Lcd.print("bldc6p>> ");

    /* Obtain input */
    while(1) {
        /* Get char from stdin */
        M5.update();
        M5.Lcd.print(c);

        if ( M5.BtnA.wasPressed() ) {

        c = 1;

        }

        /* Process obtained command */
        switch (c) {
            case 1:   /* Start motor */
                if (st == RUNNING) {
                    M5.Lcd.print("Motor already running.");
                }
                else if (st == STILL) {
                    st = RUNNING;
                    M5.Lcd.print("Starting motor...");
                }
                return;
            case 2:   /* Stop (halt) motor */
                if (st == RUNNING) {
                    st = STILL;
                    M5.Lcd.print("Stopping motor...");
                }
                else if (st == STILL) {
                    M5.Lcd.print("Motor already standstill.");
                }
                return;

            case 3:   /* Raise modulation index */
                mod += D_MOD;
                if (mod > 1000000) {
                    mod = 1000000;
                }
//                M5.Lcd.print("Modulation index raised up to: %.2f", mod / 1000000);
                return;

            case 4:   /* Lower modulation index */
                mod -= D_MOD;
                if (mod < 0) {
                    mod = 0;
                }
//                M5.Lcd.print("Modulation index lowered down to: %.2f", mod / 1000000);
                return;

            case 5:   /* Show rotational speed */
                for(j = 0; j < MAF; j++) {
                    tick_ave += tick_diff[j];
                }
                tick_ave /= MAF;
//                M5.Lcd.print("Average tick difference for one electrical rotation: %d microsec", tick_ave);
//                M5.Lcd.print("Rotational speed: %.2f Hz", 1 / (tick_ave / 1e6) / (long)P_PAIR);
//                M5.Lcd.print("Rotational speed: %.2f rpm", 60.0 / (tick_ave / 1e6) / (long)P_PAIR);
                return;

            case 6:   /* Exit from this program */
                M5.Lcd.print("Exiting from the program...");
                gateBlock();
                exit(0);

            case 0:
                break;

            default:
                M5.Lcd.print("Unknown command.");
        }

        delay(100);
    }
}

/* Function to try to forced-commutatuon to start motor */
void forcedCommutate(unsigned num, unsigned polePair, uint32_t tick_f)
{
    unsigned i, sector;
    uint32_t tick;

    /* Try to drive motor in forced-commutatin mode */
    for (i = 0; i < num * polePair; i++) {
        for (sector = 0; sector < 6; sector++) {
            /* Choose sector one by one and wait */
            tick = micros();
            produceSignal(sector + 1);
//            M5.Lcd.print(tick);
            while(micros() - tick < tick_f);
        }
    }
}

/* Function to produce GPIO signals depending on selected sector */
void produceSignal(unsigned sector)
{
    /* Produce necessary signals for Sector 1-6 */
    switch(sector) {
        case 1:     /* Sector 1 */
            /* Produce input (gate) signals */
            ledcWrite(IN1PWM,128);
            ledcWrite(IN2PWM,0);
            ledcWrite(IN3PWM,0);


            /* Produce enable (deblock) signals */
            digitalWrite(EN1, DEB);
            digitalWrite(EN2, GB);
            digitalWrite(EN3, DEB);

            /* Write 1 to Sector 1 signal */
//            digitalWrite(SEC1, 1);

            break;

        case 2:     /* Sector 2 */
            /* Produce input (gate) signals */
            ledcWrite(IN1PWM,0);
            ledcWrite(IN2PWM,128);
            ledcWrite(IN3PWM,0);

            /* Produce enable (deblock) signals */
            digitalWrite(EN1, GB);
            digitalWrite(EN2, DEB);
            digitalWrite(EN3, DEB);

            /* Write 0 to Sector 1 signal */
//            digitalWrite(SEC1, 0);

            break;

        case 3:     /* Sector 3 */
            /* Produce input (gate) signals */
            ledcWrite(IN1PWM,0);
            ledcWrite(IN2PWM,128);
            ledcWrite(IN3PWM,0);

            /* Produce enable (deblock) signals */
            digitalWrite(EN1, DEB);
            digitalWrite(EN2, DEB);
            digitalWrite(EN3, GB);

            /* Write 0 to Sector 1 signal */
//            digitalWrite(SEC1, 0);

            break;

        case 4:     /* Sector 4 */
            /* Produce input (gate) signals */
            ledcWrite(IN1PWM,0);
            ledcWrite(IN2PWM,0);
            ledcWrite(IN3PWM,128);

            /* Produce enable (deblock) signals */
            digitalWrite(EN1, DEB);
            digitalWrite(EN2, GB);
            digitalWrite(EN3, DEB);

            /* Write 0 to Sector 1 signal */
//            digitalWrite(SEC1, 0);

            break;

        case 5:     /* Sector 5 */
            /* Produce input (gate) signals */
            ledcWrite(IN1PWM,0);
            ledcWrite(IN2PWM,0);
            ledcWrite(IN3PWM,128);

            /* Produce enable (deblock) signals */
            digitalWrite(EN1, GB);
            digitalWrite(EN2, DEB);
            digitalWrite(EN3, DEB);

            /* Write 0 to Sector 1 signal */
//            digitalWrite(SEC1, 0);

            break;

        case 6:     /* Sector 6 */
            /* Produce input (gate) signals */
            ledcWrite(IN1PWM,128);
            ledcWrite(IN2PWM,0);
            ledcWrite(IN3PWM,0);

            /* Produce enable (deblock) signals */
            digitalWrite(EN1, DEB);
            digitalWrite(EN2, DEB);
            digitalWrite(EN3, GB);

            /* Write 0 to Sector 1 signal */
//            digitalWrite(SEC1, 0);

            break;
    }
}

/* Function to gate block */
void gateBlock()
{
    digitalWrite(EN1, GB);
    digitalWrite(EN2, GB);
    digitalWrite(EN3, GB);
}