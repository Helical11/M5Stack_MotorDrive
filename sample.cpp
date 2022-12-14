/*
 * bldc6p.c
 * A test of driving a blushless DC motor with 6-pulse contol by Raspberry Pi
 * Depends on pigpio.h
 * (c) 2021 @RR_Inyo
 * Released under the MIT lisence
 * https://opensource.org/licenses/mit-license.php
 */

#include <stdio.h>
#include <stdlib.h>
#include <pigpio.h>

/* Define motor and drive parameter */
#define P_PAIR  7       /* Number of pole pairs */
#define F_PWM   15000   /* [Hz], PWM carrier frequency */
#define MOD_I   400000  /* modulaiton index at the start of 6-pulse control: Note unity = 1,000,000 */
#define NUM_F   8       /* Number of turns in terms for initial forced commutation */
#define MOD_F   400000  /* Modulation index for initial forced commutation: Note unity = 1,000,000 */
#define TICK_F  100     /* [microseconds] to stay in a section */
#define D_MOD   20000   /* Change of modulation index by one command */
#define CHKDLY  50000   /* [microsecond] to wait to check the callback function takes over control */
#define MAF     540     /* Moving average filter to count revolution */

/* Define GPIO pins connected to P-NUCLEO-IHM001 */
/* Hall sensors */
/* Note: H1, H2, and H3 becomes 1 (high) when they detect a south pole. */
#define H1      2
#define H2      3
#define H3      4

/* Enable (deblock) signals */
#define EN1     16
#define EN2     20
#define EN3     21

/* Input (gate) signals */
/* Assign GPIO pins capable of hardware PWM */
/* PWM0: GPIO12, 18 for U- and W-phases V */
/* PWM1: GPIO13 for V-phase */
#define IN1     12
#define IN2     13
#define IN3     18

/* Sector 1 signal */
#define SEC1    25

/* Constant aliases */
#define GB      0
#define DEB     1

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

/* The main function */
int main(int argc, char *argv[])
{
    /* Show initial message */
    printf("=====================================================\n");
    printf(" bldc6p - BLDC motor 6-pulse control by Raspberry Pi \n");
    printf(" (c) 2021 @RR_Inyo                                   \n");
    printf("=====================================================\n");
    printf("Commands:                                            \n");
    printf("  s: Start motor                                     \n");
    printf("  h: Stop motor                                      \n");
    printf("  r: Raise modulation index                          \n");
    printf("  l: Lower modulation index                          \n");
    printf("  t: Show rotational speed                           \n");
    printf("  e: End this program                                \n");

    /* Set GPIO pins to communicate with P-NUCLEO-IHM001 */
    printf("Setting GPIO pins...\n");
    setGPIO();

    /* Infinate loop, outer */
    while(1) {

        /* Show prompt to user and wait for start command */
        while(st == STILL) {
            processCommand();
        }

        /* Forced commutation to start up */
        printf("Trying forced commutation...\n");
        mod = MOD_F;
        forcedCommutate(NUM_F, P_PAIR, TICK_F);

        /* Lower the flag */
        called = 0;

        /* Set callback function and start 6-pulse control with Hall sensor signals */
        printf("Getting into the 6-pulse (120-degree) control mode by ISR callback functions...\n");
        mod = MOD_I;
        gpioSetISRFunc(H1, EITHER_EDGE, TIMEOUT, cbDriveMotor);
        gpioSetISRFunc(H2, EITHER_EDGE, TIMEOUT, cbDriveMotor);
        gpioSetISRFunc(H3, EITHER_EDGE, TIMEOUT, cbDriveMotor);

        /* Check whether successfully got into the 6-pulse mode */
        gpioDelay(CHKDLY);
        if(!called) {
            printf("Failed to get into the 6-pulse (120-degree) control  mode by ISR callback functions.\n");
            gpioSetISRFunc(H1, EITHER_EDGE, TIMEOUT, NULL);
            gpioSetISRFunc(H2, EITHER_EDGE, TIMEOUT, NULL);
            gpioSetISRFunc(H3, EITHER_EDGE, TIMEOUT, NULL);
            gateBlock();
            st = STILL;
            continue;
        }
        else {
            printf("Succeeded in getting into the 6-pulse (120-degree) control mode by ISR callback functions.\n");
        }

        /* Infinate loop, inner */
        while(st == RUNNING) {
            processCommand();
        }

        /* Cancel callback functions */
        gpioSetISRFunc(H1, EITHER_EDGE, TIMEOUT, NULL);
        gpioSetISRFunc(H2, EITHER_EDGE, TIMEOUT, NULL);
        gpioSetISRFunc(H3, EITHER_EDGE, TIMEOUT, NULL);

        /* Gate block */
        gateBlock();
    }
}

/* Function to set GPIO pins to communicate with P-NUCLEO-IHM001 */
void setGPIO()
{
    /* Initialize GPIO */
    if(gpioInitialise() < 0) {
        printf("GPIO initialization failed.\n");
        exit(1);
    }
    else {
        printf("GPIO initialization OK.\n");
    }

    /* Set GPIO pins to Hall sensors as input */
    gpioSetMode(H1, PI_INPUT);
    gpioSetMode(H2, PI_INPUT);
    gpioSetMode(H3, PI_INPUT);

    /* Set GPIO pins to enable (deblock) signals as output */
    /* Initial values shall be 0 (low) */
    gpioWrite(EN1, GB);
    gpioWrite(EN2, GB);
    gpioWrite(EN3, GB);
    gpioSetMode(EN1, PI_OUTPUT);
    gpioSetMode(EN2, PI_OUTPUT);
    gpioSetMode(EN3, PI_OUTPUT);

    /* Set GPIO pins to input (gate) signals as output */
    /* Initial values shall be 0 (low) */
    gpioWrite(IN1, 0);
    gpioWrite(IN2, 0);
    gpioWrite(IN3, 0);
    gpioSetMode(IN1, PI_OUTPUT);
    gpioSetMode(IN2, PI_OUTPUT);
    gpioSetMode(IN3, PI_OUTPUT);

    /* Set GPIO pin for Sector 1 signal */
    gpioWrite(SEC1, 0);
    gpioSetMode(SEC1, PI_OUTPUT);
}

/* Function to process command from user */
void processCommand()
{
    int c;
    int j = 0;
    uint64_t tick_ave = 0;

    /* Show prompt */
    printf("bldc6p>> ");

    /* Obtain input */
    while(1) {
        /* Get char from stdin */
        c = getchar();

        /* Process obtained command */
        switch (c) {
            case 's':   /* Start motor */
                if (st == RUNNING) {
                    printf("Motor already running.\n");
                }
                else if (st == STILL) {
                    st = RUNNING;
                    printf("Starting motor...\n");
                }
                return;
            case 'h':   /* Stop (halt) motor */
                if (st == RUNNING) {
                    st = STILL;
                    printf("Stopping motor...\n");
                }
                else if (st == STILL) {
                    printf("Motor already standstill.\n");
                }
                return;

            case 'r':   /* Raise modulation index */
                mod += D_MOD;
                if (mod > 1000000) {
                    mod = 1000000;
                }
                printf("Modulation index raised up to: %.2f\n", mod / 1000000.0);
                return;

            case 'l':   /* Lower modulation index */
                mod -= D_MOD;
                if (mod < 0) {
                    mod = 0;
                }
                printf("Modulation index lowered down to: %.2f\n", mod / 1000000.0);
                return;

            case 't':   /* Show rotational speed */
                for(j = 0; j < MAF; j++) {
                    tick_ave += tick_diff[j];
                }
                tick_ave /= MAF;
                printf("Average tick difference for one electrical rotation: %d microsec\n", (int) tick_ave);
                printf("Rotational speed: %.2f Hz\n", 1 / (tick_ave / 1e6) / P_PAIR);
                printf("Rotational speed: %.2f rpm\n", 60.0 / (tick_ave / 1e6) / P_PAIR);
                return;

            case 'e':   /* Exit from this program */
                printf("Exiting from the program...\n");
                gateBlock();
                gpioTerminate();
                exit(0);

            case '\n':
                break;

            default:
                printf("Unknown command.\n");
        }
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
            tick = gpioTick();
            produceSignal(sector + 1);
            while(gpioTick() - tick < tick_f);
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
            gpioHardwarePWM(IN1, F_PWM, mod);
            gpioWrite(IN2, 0);
            gpioWrite(IN3, 0);

            /* Produce enable (deblock) signals */
            gpioWrite(EN1, DEB);
            gpioWrite(EN2, GB);
            gpioWrite(EN3, DEB);

            /* Write 1 to Sector 1 signal */
            gpioWrite(SEC1, 1);

            break;

        case 2:     /* Sector 2 */
            /* Produce input (gate) signals */
            gpioWrite(IN1, 0);
            gpioHardwarePWM(IN2, F_PWM, mod);
            gpioWrite(IN3, 0);

            /* Produce enable (deblock) signals */
            gpioWrite(EN1, GB);
            gpioWrite(EN2, DEB);
            gpioWrite(EN3, DEB);

            /* Write 0 to Sector 1 signal */
            gpioWrite(SEC1, 0);

            break;

        case 3:     /* Sector 3 */
            /* Produce input (gate) signals */
            gpioWrite(IN1, 0);
            gpioHardwarePWM(IN2, F_PWM, mod);
            gpioWrite(IN3, 0);

            /* Produce enable (deblock) signals */
            gpioWrite(EN1, DEB);
            gpioWrite(EN2, DEB);
            gpioWrite(EN3, GB);

            /* Write 0 to Sector 1 signal */
            gpioWrite(SEC1, 0);

            break;

        case 4:     /* Sector 4 */
            /* Produce input (gate) signals */
            gpioWrite(IN1, 0);
            gpioWrite(IN2, 0);
            gpioHardwarePWM(IN3, F_PWM, mod);

            /* Produce enable (deblock) signals */
            gpioWrite(EN1, DEB);
            gpioWrite(EN2, GB);
            gpioWrite(EN3, DEB);

            /* Write 0 to Sector 1 signal */
            gpioWrite(SEC1, 0);

            break;

        case 5:     /* Sector 5 */
            /* Produce input (gate) signals */
            gpioWrite(IN1, 0);
            gpioWrite(IN2, 0);
            gpioHardwarePWM(IN3, F_PWM, mod);

            /* Produce enable (deblock) signals */
            gpioWrite(EN1, GB);
            gpioWrite(EN2, DEB);
            gpioWrite(EN3, DEB);

            /* Write 0 to Sector 1 signal */
            gpioWrite(SEC1, 0);

            break;

        case 6:     /* Sector 6 */
            /* Produce input (gate) signals */
            gpioHardwarePWM(IN1, F_PWM, mod);
            gpioWrite(IN2, 0);
            gpioWrite(IN3, 0);

            /* Produce enable (deblock) signals */
            gpioWrite(EN1, DEB);
            gpioWrite(EN2, DEB);
            gpioWrite(EN3, GB);

            /* Write 0 to Sector 1 signal */
            gpioWrite(SEC1, 0);

            break;
    }
}

/* Callback function to choose sector depending on Hall sensor signals */
void cbDriveMotor(int gpio, int level, uint32_t tick)
{
    /* Raise the flag */
    called = 1;

    /* Choose sector depending on Hall sensor signals */
    /* From 010 to 011 */
    if (gpio == H3 && level == 1) {

        /* Storing tick difference into buffer */
        tick_1 = tick_0;
        tick_0 = tick;
        tick_diff[k] = tick_0 - tick_1;
        k = (k + 1) % MAF;

        produceSignal(1);
    }
    /* From 011 to 001 */
    else if (gpio == H2 && level == 0) {
        produceSignal(2);
    }
    /* From 001 to 101 */
    else if (gpio == H1 && level == 1) {
        produceSignal(3);
    }
    /* From 101 to 100 */
    else if (gpio == H3 && level == 0) {
        produceSignal(4);
    }
    /* From 100 to 110 */
    else if (gpio == H2 && level == 1) {
        produceSignal(5);
    }
    /* From 110 to 010 */
    else if (gpio == H1 && level == 0) {
        produceSignal(6);
    }
}

/* Function to gate block */
void gateBlock()
{
    gpioWrite(EN1, GB);
    gpioWrite(EN2, GB);
    gpioWrite(EN3, GB);
}