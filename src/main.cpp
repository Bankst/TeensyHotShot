#include <Arduino.h>

#include <EEPROM.h>
#include <TeensyThreads.h>

#include "build_defs.h"


/* INPUTS
 * ==========================================================================================
 * UPPER/LOWER Opto - abstractify
 *    optos are 12vdc
 *    each is 3pin, +12V, SENSE, GND
 *    harness connector has 4pin, +12V, UPPER_SENSE, LOWER_SENSE, GND
 *    On Connector P2, LOWER_SENSE = 9, UPPER_SENSE = 8
 *
 * COIN 1/2
 *    simple rising edge interrupt, maybe 12v?
 *    On Connector P2, COIN1 = 1, COIN2 = 2
 *
 * AUX1/AUX2/RESET programming buttons
 *    simple debounce. interrupt on RESET and AUX1 (to enter programming mode)
 */

#define UPPER_OPTO_IN 2
#define LOWER_OPTO_IN 3

#define COIN1_IN 4

#define AUX1_IN 5
#define AUX2_IN 6
#define RESET_IN 7

/* OUTPUTS
 * ==========================================================================================
 * TICKET/CREDIT counters 
 *    5v counter, ticks on rising edge.
 *    Same harness as AUX1/AUX2/RESET
 *    On Connector P3, TICKET_COUNTER = 1, COIN_COUNTER = 2
 * 
* TICKET_NOTCH 
 *    active low, pulse at 1ms intervals for however many tickets to dispense
 *    Connect directly to TeensyMainBoard
 * 
 * BALL GATE ACTUATOR
 *    active high, hold for gate open.
 *    runs through transistor to drive relay coil
 *    
 * 
 * STATUS LED 
 *    using LED_BUILTIN
 * 
 * CREDIT LED
 *    ??
 */

#define TICKET_COUNTER_OUT 14
#define CREDIT_COUNTER_OUT 15
#define TICKET_NOTCH_OUT 16
#define BALL_GATE_OUT 17

#define STATUS_LED LED_BUILTIN
#define STATUS_BLINK_MS 60
#define STATUS_BLINK_DELAY_MS 1000


/* INTERFACES
 * ==========================================================================================
 * 7Seg TIME/SCORE displays
 *    use 2 MAX7219s, one per display
 *    https://www.ebay.com/itm/MAXIM-MAX7219CNG-DIP-24-LED-Display-Driver-IC-NEW-C/141975802299
 */


/* GLOBALS
 * ==================================================================================================== 
 * Variable Name      Type       Default    Description
 * ==================================================================================================== 
 * curScore           uint8_t     0         number of balls scored in current game
 * lastScore          uint8_t     0         number of balls scored in last game
 * curTickets         uint16_t    0         number of tickets earned in current game
 * curCredits         uint8_t     0         current available credits
 * 
 * CONSTANTS (store in EEPROM for programmability, todo later)
 * ====================================================================================================
 * highScore          uint8_t     15        highest number of balls scored in previous games (saved to eeprom on every change)
 * ticketsPerScore    uint8_t     1         number of tickets earned per ball scored
 * playsPerCredit     uint8_t     1         number of plays per credit
 * // jackpotTickets  uint16_t    0         number of tickets earned when high score is beat (maybe just a multiplier of the high-score?)
 * playTime           uint8_t     60        time in seconds each game lasts
 * attractTime        uint8_t     240       time in seconds between attract-activations
 * 
 * DEFINES
 * TICKET_PULSE_DELAY             time between ticket pulses in ms
 * 
 * TODOs (Sound/lights)
 */

#define DISPLAY_ENABLE_OUT 23
#define DISPLAY_STROBE_OUT 22
#define DISPLAY_SDATA_OUT 21
#define DISPLAY_CLOCK_OUT 20

#define SEC_TO_MICROSEC(x) x * 1000000

#define TICKET_PULSE_DELAY 20

#define VERSION_MAJOR 0
#define VERSION_MINOR 1

const unsigned char completeVersion[] =
{
  'V',
  VERSION_MAJOR_INIT,
  '.',
  VERSION_MINOR_INIT,
  '-',
  BUILD_YEAR_CH0, BUILD_YEAR_CH1, BUILD_YEAR_CH2, BUILD_YEAR_CH3,
  '-',
  BUILD_MONTH_CH0, BUILD_MONTH_CH1,
  '-',
  BUILD_DAY_CH0, BUILD_DAY_CH1,
  '-',
  BUILD_HOUR_CH0, BUILD_HOUR_CH1,
  ':',
  BUILD_MIN_CH0, BUILD_MIN_CH1,
  ':',
  BUILD_SEC_CH0, BUILD_SEC_CH1,
  '\0'
};

uint8_t curScore, lastScore, curCredits;
uint16_t curTickets;

#define HIGH_SCORE_DEFAULT 15
#define TICKETS_PER_SCORE_DEFAULT 4
#define PLAYS_PER_CREDIT_DEFAULT 1
#define PLAY_TIME_DEFAULT 5
#define ATTRACT_TIME_DEFAULT 240

#define EEPROM_INITIALIZED_EEPROMADDR 0
#define HIGH_SCORE_EEPROMADDR 128
#define TICKETS_PER_SCORE_EEPROMADDR 129
#define PLAYS_PER_CREDIT_EEPROMADDR 130
#define PLAY_TIME_EEPROMADDR 131
#define ATTRACT_TIME_EEPROMADDR 132

enum class GameState {
  GS_START,
  GS_RUN,
  GS_LAST10,
  GS_END,
  GS_ATTRACT // attract
};

uint8_t highScore, ticketsPerScore, playsPerCredit, playTime, attractTime;
// uint16_t jackpotTickets;

IntervalTimer gameTimer, attractTimer;

volatile GameState curGameState = GameState::GS_ATTRACT;
volatile uint8_t lastGameSec;
volatile uint8_t remainingGameSec;
volatile bool doAttract;

volatile bool coin1in;
volatile unsigned long lastCoin1Millis;
uint16_t coinDelay = 2500; // time to wait before accepting another credit


void statusLedThread() {
  digitalWriteFast(STATUS_LED, LOW);
  while(1) {
    digitalWriteFast(STATUS_LED, HIGH);
    threads.delay(STATUS_BLINK_MS);
    digitalWriteFast(STATUS_LED, LOW);
    threads.delay(STATUS_BLINK_MS);
    digitalWriteFast(STATUS_LED, HIGH);
    threads.delay(STATUS_BLINK_MS);
    digitalWriteFast(STATUS_LED, LOW);
    threads.delay(STATUS_BLINK_DELAY_MS);
  }
}

void attractCallback() {
  if (curGameState == GameState::GS_ATTRACT) {
    doAttract = true;
  }
}

void dispenseTickets(int16_t tickets) {
  int16_t i;
  for (i = 0; i < tickets; i++) {
    digitalWriteFast(TICKET_NOTCH_OUT, LOW);
    digitalWriteFast(TICKET_COUNTER_OUT, HIGH);
    threads.delay(TICKET_PULSE_DELAY);
    digitalWriteFast(TICKET_NOTCH_OUT, HIGH);
    digitalWriteFast(TICKET_COUNTER_OUT, LOW);
    threads.delay(TICKET_PULSE_DELAY);
  }
}

volatile bool gameTick;
volatile bool delayNextGame;

void gameTimerCallback() {
  gameTick = true;
  remainingGameSec--;
}

void gameThread() {
  while(1) {

    if (delayNextGame && curGameState == GameState::GS_ATTRACT) {
      Serial.print("Starting next game in 10 seconds");
      for (int i = 0; i < 10; i++) {        
        Serial.print('.');
        threads.delay(1000);
      }
      Serial.println();
      curGameState = GameState::GS_START;
    }

    switch(curGameState) {
      case GameState::GS_START:
        curCredits--; // use 1 credit

        delayNextGame = (curCredits >= 1 && curGameState != GameState::GS_ATTRACT);

        Serial.print("Game started, new balance: ");
        Serial.println(curCredits);
        // play "Get ready" sound?
        threads.delay(2500); // wait for player to get ready
        digitalWriteFast(BALL_GATE_OUT, HIGH);
        // delay timer start for balls to come out?
        remainingGameSec = playTime;
        gameTimer.begin(gameTimerCallback, SEC_TO_MICROSEC(1));
        curGameState = GameState::GS_RUN; // move to next state
        break;
      case GameState::GS_RUN:
        // service opto interrupts and set scores
        if (remainingGameSec <= 10) {
          curScore = 5;
          curGameState = GameState::GS_LAST10;
        }
        break;
      case GameState::GS_LAST10:
        // continue opto-ISR, do lights and sound
        if (remainingGameSec <= 0) {
          curScore++;
          curGameState = GameState::GS_END;
        }
        break;
      case GameState::GS_END:
        gameTimer.end();
        digitalWriteFast(BALL_GATE_OUT, LOW); // close ball gate

        if (curScore > highScore) {
          Serial.println("Beat high score"); // do something??
        }

        dispenseTickets(curScore * ticketsPerScore); // dispense tickets
        lastScore = curScore;
        curScore = 0;

        Serial.print("Game ended, Final score: ");
        Serial.print(lastScore);
        Serial.print(", Tickets earned: ");
        Serial.println(lastScore * ticketsPerScore);

        curGameState = GameState::GS_ATTRACT;
        break;
      case GameState::GS_ATTRACT:
        if (doAttract) {
          // do something attractive ;)
          doAttract = false;
        }

        break;
    }     
  }  
}

void coin1ISR() {
  if (millis() - lastCoin1Millis > coinDelay) {
    curCredits++;
    coin1in = true;
    lastCoin1Millis = millis();
  }
}

void displayThread() {
  digitalWriteFast(DISPLAY_ENABLE_OUT, LOW); // active low
  digitalWriteFast(DISPLAY_CLOCK_OUT, LOW);
  digitalWriteFast(DISPLAY_STROBE_OUT, LOW);
  
  delay(5);
  Serial.println("Pushed display data");
  while(1) {
    digitalWrite(DISPLAY_STROBE_OUT, HIGH);
    delay(1);
    shiftOut_lsbFirst(DISPLAY_SDATA_OUT, DISPLAY_CLOCK_OUT, B11111100);
    delay(1);
    digitalWrite(DISPLAY_STROBE_OUT, LOW);
  }
}

void setupIO() {
  pinMode(UPPER_OPTO_IN, INPUT);
  pinMode(LOWER_OPTO_IN, INPUT);
  pinMode(COIN1_IN, INPUT_PULLUP);
  pinMode(AUX1_IN, INPUT_PULLUP);
  pinMode(AUX2_IN, INPUT_PULLUP);
  pinMode(RESET_IN, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(COIN1_IN), coin1ISR, RISING);

  pinMode(TICKET_COUNTER_OUT, OUTPUT);
  pinMode(CREDIT_COUNTER_OUT, OUTPUT);
  pinMode(TICKET_NOTCH_OUT, OUTPUT);
  pinMode(BALL_GATE_OUT, OUTPUT);

  // display
  pinMode(DISPLAY_ENABLE_OUT, OUTPUT);
  pinMode(DISPLAY_STROBE_OUT, OUTPUT);
  pinMode(DISPLAY_SDATA_OUT, OUTPUT);
  pinMode(DISPLAY_CLOCK_OUT, OUTPUT);

  pinMode(LED_BUILTIN, OUTPUT);

  digitalWriteFast(TICKET_NOTCH_OUT, HIGH); // active low 
  digitalWriteFast(LED_BUILTIN, HIGH); // goes low in status thread
}

void setupEEPROM() {
  /* Variables that must be stored.
   * ticketsPerScore, playsPerCredit,
   * playTime, attractTime
   * 
   */

  // if this board has never had the eeprom initialized
  if (EEPROM.read(EEPROM_INITIALIZED_EEPROMADDR) != 1 || true) {
    EEPROM.write(EEPROM_INITIALIZED_EEPROMADDR, 1);
    EEPROM.put(HIGH_SCORE_EEPROMADDR, HIGH_SCORE_DEFAULT);
    EEPROM.put(TICKETS_PER_SCORE_EEPROMADDR, TICKETS_PER_SCORE_DEFAULT);
    EEPROM.put(PLAYS_PER_CREDIT_EEPROMADDR, PLAYS_PER_CREDIT_DEFAULT);
    EEPROM.put(PLAY_TIME_EEPROMADDR, PLAY_TIME_DEFAULT);
    EEPROM.put(ATTRACT_TIME_EEPROMADDR, ATTRACT_TIME_DEFAULT);
  }

  EEPROM.get(HIGH_SCORE_EEPROMADDR, highScore);
  EEPROM.get(TICKETS_PER_SCORE_EEPROMADDR, ticketsPerScore);
  EEPROM.get(PLAYS_PER_CREDIT_EEPROMADDR, playsPerCredit);
  EEPROM.get(PLAY_TIME_EEPROMADDR, playTime);
  EEPROM.get(ATTRACT_TIME_EEPROMADDR, attractTime);

  Serial.println("EEPROM Initialized");
  Serial.print("Play Time: ");
  Serial.println(playTime);
}

void setupThreads() {
  threads.addThread(statusLedThread);
  threads.addThread(gameThread);
  threads.addThread(displayThread);
}

void setupTimers() {
  attractTimer.begin(attractCallback, SEC_TO_MICROSEC(attractTime));
}

void setup() {
  Serial.begin(true);
  delay(500);

  setupIO();
  setupEEPROM();
  setupTimers();
  setupThreads();

  Serial.println("Hot Shot Reloaded initialized");  
}

void handleCredit() {
  Serial.print("Got Credit, new balance: ");
  Serial.println(curCredits);
  if (curCredits >= 1 && curGameState == GameState::GS_ATTRACT) {
    curGameState = GameState::GS_START;
  } else if (curCredits >= 1 && curGameState != GameState::GS_ATTRACT) {
    delayNextGame = true;
    Serial.println("Delaying next game by 10sec");
  }
}

void loop() {
  if (coin1in) {   
    handleCredit();
    coin1in = false;    
  }

  if (gameTick) {
    Serial.print("Game time left: ");
    Serial.println(remainingGameSec);
    gameTick = false;  
  }
}