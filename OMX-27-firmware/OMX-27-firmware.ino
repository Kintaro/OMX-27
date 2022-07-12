// OMX-27 MIDI KEYBOARD / SEQUENCER
// v 1.6.0
//
// Steven Noreyko, Last update: July 2022
//
//
//	Big thanks to:
//	John Park and Gerald Stevens for initial testing and feature ideas
//	mzero for immense amounts of code coaching/assistance
//	drjohn for support
//  Additional code contributions: Matt Boone, Steven Zydek, Chris Atkins, Will Winder


#include <functional>
#include <Adafruit_NeoPixel.h>
#include <ResponsiveAnalogRead.h>

#include "consts.h"
#include "config.h"
#include "colors.h"
#include "MM.h"
#include "ClearUI.h"
#include "sequencer.h"
#include "noteoffs.h"
#include "storage.h"
#include "sysex.h"
#include "omx_keypad.h"
#include "omx_util.h"
#include "omx_disp.h"

const int potCount = 5;
ResponsiveAnalogRead *analog[potCount];

// storage of pot values; current is in the main loop; last value is for midi output
int volatile currentValue[potCount];
int lastMidiValue[potCount];
int potMin = 0;
int potMax = 8190;
int temp;

// Timers and such
elapsedMillis blink_msec = 0;
elapsedMillis slow_blink_msec = 0;
elapsedMillis pots_msec = 0;
elapsedMillis dirtyDisplayTimer = 0;
unsigned long displayRefreshRate = 60;
elapsedMicros clksTimer = 0;		// is this still in use?

//unsigned long clksDelay;
//elapsedMillis keyPressTime[27] = {0};

using Micros = unsigned long;
Micros lastProcessTime;
volatile unsigned long step_micros;
volatile unsigned long noteon_micros;
volatile unsigned long noteoff_micros;
volatile unsigned long ppqInterval;

elapsedMillis screenSaverCounter = 0;
bool screenSaverMode = false;
unsigned long screensaverInterval = 1000 * 60 * 3; // 3 minutes default? // 10000;  15000; //
int ssstep = 0;
int ssloop = 0;
volatile unsigned long nextStepTimeSS = 0;
bool ssreverse = false;
int sleepTick = 80;

// DEFAULT COLOR VARIABLES
uint32_t screensaverColor = 0xFF0000;
uint32_t stepColor = 0x000000;
uint32_t muteColor = 0x000000;
uint16_t midiBg_Hue = 0;
uint8_t midiBg_Sat = 255;
uint8_t midiBg_Brightness = 255;

int nspage = 0;
int pppage = 0;
int sqpage = 0;
int srpage = 0;
int mmpage = 0;

int miparam = 0;	// midi params item counter
int nsparam = 0;	// note select params
int ppparam = 0;	// pattern params
int sqparam = 0;	// seq params
int srparam = 0;	// step record params
int tmpmmode = 9;

// global sequencer shared state
SequencerState sequencer = defaultSequencer();

// VARIABLES / FLAGS
float step_delay;
bool dirtyPixels = false;

bool blinkState = false;
bool slowBlinkState = false;
bool noteSelect = false;
bool noteSelection = false;
bool patternParams = false;
bool seqPages = false;
int noteSelectPage = 0;
int selectedNote = 0;
int selectedStep = 0;
bool stepSelect = false;
bool stepRecord = false;
bool stepDirty = false;
bool dialogFlags[] = {false, false, false, false, false, false};
unsigned dialogDuration = 1000;

bool copiedFlag = false;
bool pastedFlag = false;
bool clearedFlag = false;

bool enc_edit = false;
bool midiAUX = false;

bool m8mutesolo[] = {false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false};

int pitchCV;
uint8_t RES;
uint16_t AMAX;
int V_scale;

// clock
float clockbpm = 120;
float newtempo = clockbpm;
unsigned long tempoStartTime, tempoEndTime;

unsigned long blinkInterval = clockbpm * 2;



//int midiChannel; // the MIDI channel number to send messages (MIDI/OM mode)

// ENCODER
Encoder myEncoder(12, 11); 	// encoder pins on hardware
Button encButton(0);		// encoder button pin on hardware
//long newPosition = 0;
//long oldPosition = -999;

// KEYPAD
//initialize an instance of custom Keypad class
unsigned long longPressInterval = 800;
unsigned long clickWindow = 200;
OMXKeypad keypad(longPressInterval, clickWindow, makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// Declare NeoPixel strip object
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// setup EEPROM/FRAM storage
Storage* storage;
SysEx* sysEx;

// ####### CLOCK/TIMING #######

void advanceClock(Micros advance) {
	static Micros timeToNextClock = 0;
	while (advance >= timeToNextClock) {
		advance -= timeToNextClock;

		MM::sendClock();
		timeToNextClock = ppqInterval * (PPQ / 24);
	}
	timeToNextClock -= advance;
}

void advanceSteps(Micros advance) {
	static Micros timeToNextStep = 0;
//	static Micros stepnow = micros();
	while (advance >= timeToNextStep) {
		advance -= timeToNextStep;
		timeToNextStep = ppqInterval;

		// turn off any expiring notes
		pendingNoteOffs.play(micros());

		// turn on any pending notes
		pendingNoteOns.play(micros());
	}
	timeToNextStep -= advance;
}

void resetClocks(){
	// BPM tempo to step_delay calculation
	ppqInterval = 60000000/(PPQ * clockbpm); 		// ppq interval is in microseconds
	step_micros = ppqInterval * (PPQ/4); 			// 16th note step in microseconds (quarter of quarter note)

	// 16th note step length in milliseconds
	step_delay = step_micros * 0.001; 	// ppqInterval * 0.006; // 60000 / clockbpm / 4;
}

void setGlobalSwing(int swng_amt){
	for(int z=0; z<NUM_PATTERNS; z++) {
		sequencer.getPattern(z)->swing = swng_amt;
	}
}

// ####### POTENTIOMETERS #######

// void sendPots(int val, int channel){
// 	MM::sendControlChange(pots[potbank][val], analogValues[val], channel);
// 	potCC = pots[potbank][val];
// 	potVal = analogValues[val];
// 	potValues[val] = potVal;
// }

void readPotentimeters(){
	for(int k=0; k<potCount; k++) {
		temp = analogRead(analogPins[k]);
		analog[k]->update(temp);

		// read from the smoother, constrain (to account for tolerances), and map it
		temp = analog[k]->getValue();
		temp = constrain(temp, potMin, potMax);
		temp = map(temp, potMin, potMax, 0, 16383);

		// map and update the value
		analogValues[k] = temp >> 7;

		if(analog[k]->hasChanged()) {
			 // do stuff
			if (screenSaverMode){
				screensaverColor = analog[4]->getValue() * 4;	// value is 0-32764 for strip.ColorHSV

				// reset screensaver
				if(analog[0]->hasChanged() || analog[1]->hasChanged() || analog[2]->hasChanged() || analog[3]->hasChanged()) { 
					screenSaverCounter = 0;
				}
			}
			
			switch(sysSettings.omxMode) {
				case MODE_OM:
						// fall through - same as MIDI
				case MODE_MIDI: // MIDI
					if (midiMacro && !screenSaverMode){
						omxutil.sendPots(k, midiMacroChan);
					} else if (screenSaverMode) {
						// don't send pots in screensaver
					} else {
						omxutil.sendPots(k, sysSettings.midiChannel);
					}
					omxidsp.setDispDirty();
					break;

				case MODE_S2: // SEQ2
						// fall through - same as SEQ1
				case MODE_S1: // SEQ1
					if (noteSelect && noteSelection){ // note selection - do P-Locks
						potNum = k;
						potCC = pots[potbank][k];
						potVal = analogValues[k];

						if (k < 4){ // only store p-lock value for first 4 knobs
							getSelectedStep()->params[k] = analogValues[k];
							omxutil.sendPots(k, sequencer.getPatternChannel(sequencer.playingPattern));
						}
						omxutil.sendPots(k, sequencer.getPatternChannel(sequencer.playingPattern));
						omxidsp.setDispDirty();
					} else if (stepRecord){
						potNum = k;
						potCC = pots[potbank][k];
						potVal = analogValues[k];

						if (k < 4){ // only store p-lock value for first 4 knobs
							sequencer.getCurrentPattern()->steps[sequencer.seqPos[sequencer.playingPattern]].params[k] = analogValues[k];
							omxutil.sendPots(k, sequencer.getPatternChannel(sequencer.playingPattern));
						} else if (k == 4){
							sequencer.getCurrentPattern()->steps[sequencer.seqPos[sequencer.playingPattern]].vel = analogValues[k]; // SET POT 5 to NOTE VELOCITY HERE
						}
						omxidsp.setDispDirty();
					} else if (!noteSelect || !stepRecord){
						omxutil.sendPots(k, sequencer.getPatternChannel(sequencer.playingPattern));
					}
					break;

				default:
					break;
				}
			}
	}
}


// ####### SETUP #######

void setup() {
	Serial.begin(115200);
//	while( !Serial );

	storage = Storage::initStorage();
	sysEx = new SysEx(storage, &sysSettings);

	// incoming usbMIDI callbacks
	usbMIDI.setHandleNoteOff(OnNoteOff);
	usbMIDI.setHandleNoteOn(OnNoteOn);
	usbMIDI.setHandleControlChange(OnControlChange);
	usbMIDI.setHandleSystemExclusive(OnSysEx);

	clksTimer = 0;
	screenSaverCounter = 0;
	ssstep = 0;
	
	lastProcessTime = micros();
	resetClocks();

	randomSeed(analogRead(13));
	srand(analogRead(13));

	// SET ANALOG READ resolution to teensy's 13 usable bits
	analogReadResolution(13);

	// initialize ResponsiveAnalogRead
	for (int i = 0; i < potCount; i++){
		analog[i] = new ResponsiveAnalogRead(0, true, .001);
		analog[i]->setAnalogResolution(1 << 13);

		// ResponsiveAnalogRead is designed for 10-bit ADCs
		// meanining its threshold defaults to 4. Let's bump that for
		// our 13-bit adc by setting it to 4 << (13-10)
		analog[i]->setActivityThreshold(32);

		currentValue[i] = 0;
		lastMidiValue[i] = 0;
	}

	// HW MIDI
	MM::begin();

	//CV gate pin
	pinMode(CVGATE_PIN, OUTPUT);

	// set DAC Resolution CV/GATE
	RES = 12;
	analogWriteResolution(RES); // set resolution for DAC
		AMAX = pow(2,RES);
		V_scale = 64; // pow(2,(RES-7)); 4095 max
	analogWrite(CVPITCH_PIN, 0);

	// Load from EEPROM
	bool bLoaded = loadFromStorage();
	if ( !bLoaded )
	{
		// Failed to load due to initialized EEPROM or version mismatch
		// defaults
		sysSettings.omxMode = DEFAULT_MODE;
		sequencer.playingPattern = 0;
		sysSettings.playingPattern = 0;
		sysSettings.midiChannel = 1;
		pots[0][0] = CC1;
		pots[0][1] = CC2;
		pots[0][2] = CC3;
		pots[0][3] = CC4;
		pots[0][4] = CC5;
		initPatterns();
		saveToStorage();
	}

	// Init Display
	omxidsp.setup();

	// Startup screen
	omxidsp.drawStartupScreen();

	// Keypad
//	customKeypad.begin();
	keypad.begin();
	
	//LEDs
	strip.begin();           // INITIALIZE NeoPixel strip object (REQUIRED)
	strip.show();            // Turn OFF all pixels ASAP
	strip.setBrightness(LED_BRIGHTNESS); // Set BRIGHTNESS to about 1/5 (max = 255)
	for(int i=0; i<LED_COUNT; i++) { // For each pixel...
		strip.setPixelColor(i, HALFWHITE);
		strip.show();   // Send the updated pixel colors to the hardware.
		delay(5); // Pause before next pass through loop
	}
	rainbow(5); // rainbow startup pattern
	delay(500);

	// clear LEDs
	strip.fill(0, 0, LED_COUNT);
	strip.show();

	delay(100);

	
}

// ####### END SETUP #######


// ####### MIDI LEDS #######

void midi_leds() {
	blinkInterval = step_delay*2;

	if (blink_msec >= blinkInterval){
		blinkState = !blinkState;
		blink_msec = 0;
	}

	if (midiAUX){
		// Blink left/right keys for octave select indicators.
		auto color1 = blinkState ? LIME : LEDOFF;
		auto color2 = blinkState ? MAGENTA : LEDOFF;
		auto color3 = blinkState ? ORANGE : LEDOFF;
		auto color4 = blinkState ? RBLUE : LEDOFF;

		for (int q = 1; q < LED_COUNT; q++){				
			if (midiKeyState[q] == -1){
				if (midiBg_Hue == 0){
					strip.setPixelColor(q, LEDOFF);
				} else if (midiBg_Hue == 32){
					strip.setPixelColor(q, LOWWHITE);
				} else {
					strip.setPixelColor(q, strip.ColorHSV(midiBg_Hue, midiBg_Sat, midiBg_Brightness));
				}
			}
		}
		strip.setPixelColor(0, RED);
		strip.setPixelColor(1, color1);
		strip.setPixelColor(2, color2);
		strip.setPixelColor(11, color3);
		strip.setPixelColor(12, color4);


	// Macros

	} else if (m8AUX){
		auto color5 = blinkState ? ORANGE : LEDOFF;
		auto color6 = blinkState ? RED : LEDOFF;

		strip.setPixelColor(0, BLUE);
		strip.setPixelColor(1, ORANGE); // all mute
		strip.setPixelColor(3, LIME); // MIXER
		strip.setPixelColor(4, CYAN); // snap load
		strip.setPixelColor(5, MAGENTA); // snap save

		for(int m = 11; m < LED_COUNT-8; m++){
			if (m8mutesolo[m-11]){
				strip.setPixelColor(m, color5);
			}else{
				strip.setPixelColor(m, ORANGE);
			}
		}
		
		strip.setPixelColor(6, RED); // all solo
		for(int m = 19; m < LED_COUNT; m++){
			if (m8mutesolo[m-11]){
				strip.setPixelColor(m, color6);
			}else{
				strip.setPixelColor(m, RED);
			}
		}
		strip.setPixelColor(2, LEDOFF);
		strip.setPixelColor(7, LEDOFF);
		strip.setPixelColor(8, LEDOFF);

		strip.setPixelColor(9, YELLOW); // WAVES
		strip.setPixelColor(10, BLUE); // PLAY


	} else {
		// AUX key
		strip.setPixelColor(0, LEDOFF);
		
		// Other keys
		if (!screenSaverMode){
			// clear not held leds
			for (int q = 1; q < LED_COUNT; q++){				
				if (midiKeyState[q] == -1){
					if (midiBg_Hue == 0){
						strip.setPixelColor(q, LEDOFF);
					} else if (midiBg_Hue == 32){
						strip.setPixelColor(q, LOWWHITE);
					} else {
						strip.setPixelColor(q, strip.ColorHSV(midiBg_Hue, midiBg_Sat, midiBg_Brightness));
					}
				}
			}
		}
	}
	dirtyPixels = true;
}

// ####### SEQUENCER LEDS #######

void show_current_step(int patternNum) {
	blinkInterval = step_delay*2;
	unsigned long slowBlinkInterval = blinkInterval * 2;

	if (blink_msec >= blinkInterval){
		blinkState = !blinkState;
		blink_msec = 0;
	}
	if (slow_blink_msec >= slowBlinkInterval){
		slowBlinkState = !slowBlinkState;
		slow_blink_msec = 0;
	}


	// AUX KEY

	if (sequencer.playing && blinkState) {
		strip.setPixelColor(0, WHITE);
	} else if (noteSelect && blinkState){
		strip.setPixelColor(0, NOTESEL);
	} else if (patternParams && blinkState){
		strip.setPixelColor(0, seqColors[patternNum]);
	} else if (stepRecord && blinkState){
		strip.setPixelColor(0, seqColors[patternNum]);
	} else {
		switch(sysSettings.omxMode){
			case MODE_S1:
				strip.setPixelColor(0, SEQ1C);
				break;
			case MODE_S2:
				strip.setPixelColor(0, SEQ2C);
				break;
			default:
				strip.setPixelColor(0, LEDOFF);
				break;
		}
	}

	if (sequencer.getPattern(patternNum)->mute) {
		stepColor = muteColors[patternNum];
	} else {
		stepColor = seqColors[patternNum];
		muteColor = muteColors[patternNum];
	}

	auto currentpage = sequencer.patternPage[patternNum];
	auto pagestepstart = (currentpage * NUM_STEPKEYS);

	if (noteSelect && noteSelection) {
		// 27 LEDS so use LED_COUNT
		for(int j = 1; j < LED_COUNT; j++){
			auto pixelpos = j;
			auto selectedStepPixel = (selectedStep % NUM_STEPKEYS) + 11;

			if (pixelpos == selectedNote){
				strip.setPixelColor(pixelpos, HALFWHITE);
			} else if (pixelpos == selectedStepPixel){
				strip.setPixelColor(pixelpos, SEQSTEP);
			} else{
				strip.setPixelColor(pixelpos, LEDOFF);
			}

			// Blink left/right keys for octave select indicators.
			auto color1 = blinkState ? ORANGE : WHITE;
			auto color2 = blinkState ? RBLUE : WHITE;
			strip.setPixelColor(11, color1);
			strip.setPixelColor(26, color2);
		}

	} else if (stepRecord) {		// STEP RECORD
//		int numPages = ceil(float(sequencer.getPatternLength(patternNum))/float(NUM_STEPKEYS));

		// BLANK THE TOP ROW
		for(int j = 1; j < LED_COUNT - NUM_STEPKEYS; j++){
			strip.setPixelColor(j, LEDOFF);
		}

		for(int j = pagestepstart; j < (pagestepstart + NUM_STEPKEYS); j++){
			auto pixelpos = j - pagestepstart + 11;
//			if (j < sequencer.getPatternLength(patternNum)){
				// ONLY DO LEDS FOR THE CURRENT PAGE
				if (j == sequencer.seqPos[sequencer.playingPattern]){
					strip.setPixelColor(pixelpos, SEQCHASE);
				} else if (pixelpos != selectedNote){
					strip.setPixelColor(pixelpos, LEDOFF);
				}
//			} else  {
//				strip.setPixelColor(pixelpos, LEDOFF);
//			}
		}

	} else if (sequencer.getCurrentPattern()->solo){			// MIDI SOLO

//		for(int i = 0; i < NUM_STEPKEYS; i++){
//			if (i == seqPos[patternNum]){
//				if (playing){
//					strip.setPixelColor(i+11, SEQCHASE); // step chase
//				} else {
//					strip.setPixelColor(i+11, LEDOFF);  // DO WE NEED TO MARK PLAYHEAD WHEN STOPPED?
//				}
//			} else {
//				strip.setPixelColor(i+11, LEDOFF);
//			}
//		}
	} else if (seqPages){
		// BLINK F1+F2
		auto color1 = blinkState ? FUNKONE : LEDOFF;
		auto color2 = blinkState ? FUNKTWO : LEDOFF;
		strip.setPixelColor(1, color1);
		strip.setPixelColor(2, color2);

		// TURN OFF LEDS
		// 27 LEDS so use LED_COUNT
		for(int j = 3; j < LED_COUNT; j++){  // START WITH LEDS AFTER F-KEYS
			strip.setPixelColor(j, LEDOFF);
		}
		// SHOW LEDS FOR WHAT PAGE OF SEQ PATTERN YOURE ON
		auto len = (sequencer.getPattern(patternNum)->len/NUM_STEPKEYS);
		for(int h = 0; h <= len; h++){
			auto currentpage = sequencer.patternPage[patternNum];
			auto color = sequencePageColors[h];
			if (h == currentpage){
				color = blinkState ? sequencePageColors[currentpage] : LEDOFF;
			}
			strip.setPixelColor(11 + h, color);
		}
	} else {
		for(int j = 1; j < LED_COUNT; j++){
			if (j < sequencer.getPatternLength(patternNum)+11){
				if (j == 1) {
															// NOTE SELECT / F1
					if (keyState[j] && blinkState){
						strip.setPixelColor(j, LEDOFF);
					} else {
						strip.setPixelColor(j, FUNKONE);
					}
				} else if (j == 2) {
															// PATTERN PARAMS / F2
					if (keyState[j] && blinkState){
						strip.setPixelColor(j, LEDOFF);
					} else {
						strip.setPixelColor(j, FUNKTWO);
					}

				} else if (j == patternNum+3){  			// PATTERN SELECT
					strip.setPixelColor(j, stepColor);
					if (patternParams && blinkState){
						strip.setPixelColor(j, LEDOFF);
					}
				} else {
					strip.setPixelColor(j, LEDOFF);
				}
			} else {
				strip.setPixelColor(j, LEDOFF);
			}
		}

		auto pattern = sequencer.getPattern(patternNum);
		auto steps = pattern->steps;
		auto currentpage = sequencer.patternPage[patternNum];
		auto pagestepstart = (currentpage * NUM_STEPKEYS);

		// WHAT TO DO HERE FOR MULTIPLE PAGES
		// NUM_STEPKEYS or NUM_STEPS INSTEAD?
		for(int i = pagestepstart; i < (pagestepstart + NUM_STEPKEYS); i++){
			if (i < sequencer.getPatternLength(patternNum)){

				// ONLY DO LEDS FOR THE CURRENT PAGE
				auto pixelpos = i - pagestepstart + 11;
//				if (patternParams){
// 					strip.setPixelColor(pixelpos, SEQMARKER);
// 				}

				if(i % 4 == 0){ 					// MARK GROUPS OF 4
					if(i == sequencer.seqPos[patternNum]){
						if (sequencer.playing){
							strip.setPixelColor(pixelpos, SEQCHASE); // step chase
						} else if (steps[i].trig == TRIGTYPE_PLAY){
							if (steps[i].stepType != STEPTYPE_NONE){
								if (slowBlinkState){
									strip.setPixelColor(pixelpos, stepColor); // STEP EVENT COLOR
								}else{
									strip.setPixelColor(pixelpos, muteColor); // STEP EVENT COLOR
								}
							} else {
								strip.setPixelColor(pixelpos, stepColor); // STEP ON COLOR
							}
						} else if (steps[i].trig == TRIGTYPE_MUTE){
							strip.setPixelColor(pixelpos, SEQMARKER);
						}
					} else if (steps[i].trig == TRIGTYPE_PLAY) {
						if (steps[i].stepType != STEPTYPE_NONE){
							if (slowBlinkState){
								strip.setPixelColor(pixelpos, stepColor); // STEP EVENT COLOR
							}else{
								strip.setPixelColor(pixelpos, muteColor); // STEP EVENT COLOR
							}
						} else {
							strip.setPixelColor(pixelpos, stepColor); // STEP ON COLOR
						}
					} else if (steps[i].trig == TRIGTYPE_MUTE){
						strip.setPixelColor(pixelpos, SEQMARKER);
					}

				} else if (i == sequencer.seqPos[patternNum]){ 		// STEP CHASE
					if (sequencer.playing){
						strip.setPixelColor(pixelpos, SEQCHASE);

					} else if (steps[i].trig == TRIGTYPE_PLAY){
						if (steps[i].stepType != STEPTYPE_NONE){
							if (slowBlinkState){
								strip.setPixelColor(pixelpos, stepColor); // STEP EVENT COLOR
							}else{
								strip.setPixelColor(pixelpos, muteColor); // STEP EVENT COLOR
							}
						} else {
							strip.setPixelColor(pixelpos, stepColor); // STEP ON COLOR
						}
					} else if (!patternParams && sequencer.patterns[patternNum].steps[i].trig == TRIGTYPE_MUTE){
						strip.setPixelColor(pixelpos, LEDOFF);  // DO WE NEED TO MARK PLAYHEAD WHEN STOPPED?
					} else if (patternParams){
						strip.setPixelColor(pixelpos, SEQMARKER);
					}
				}
				else if (steps[i].trig == TRIGTYPE_PLAY)
				{
					if (steps[i].stepType != STEPTYPE_NONE){
						if (slowBlinkState){
							strip.setPixelColor(pixelpos, stepColor); // STEP EVENT COLOR
						}else{
							strip.setPixelColor(pixelpos, muteColor); // STEP EVENT COLOR
						}
					} else {
						strip.setPixelColor(pixelpos, stepColor); // STEP ON COLOR
					}

				} else if (!patternParams && steps[i].trig == TRIGTYPE_MUTE){
					strip.setPixelColor(pixelpos, LEDOFF);
				} else if (patternParams){
					strip.setPixelColor(pixelpos, SEQMARKER);
				}
			}
		}
	}
	dirtyPixels = true;
//	strip.show();
}

// SLEEP MODE LEDS
void sleepModeOne(){
	unsigned long playstepmillis = millis();
	if (playstepmillis > nextStepTimeSS){ 
		ssstep = ssstep % 16;
		ssloop = ssloop % 16 ;
	
		int j = 26 - ssloop;
		int i = ssstep + 11;

		for (int z=1; z<11; z++){
			strip.setPixelColor(z, 0);
		}
		if (!ssreverse) {
			// turn off all leds
			for (int x=0; x<16; x++){
				if (i < j){
					strip.setPixelColor(x+11, 0);
				}
				if (x+11 > j){
					strip.setPixelColor(x+11, strip.gamma32(strip.ColorHSV(screensaverColor)));
				}
			}
			strip.setPixelColor(i+1, strip.gamma32(strip.ColorHSV(screensaverColor)));
		} else {
			for (int y=0; y<16; y++){
				if (i >= j){
					strip.setPixelColor(y+11, 0);
				}
				if (y+11 < j){
					strip.setPixelColor(y+11, strip.gamma32(strip.ColorHSV(screensaverColor)));
				}
			}
			strip.setPixelColor(i+1, strip.gamma32(strip.ColorHSV(screensaverColor)));
		}
		ssstep++;
		if (ssstep == 16){
			ssloop++;
		}
		if (ssloop == 16){
			ssreverse = !ssreverse;
		}
		nextStepTimeSS = nextStepTimeSS + sleepTick;
		dirtyPixels = true;
	}
}
// ####### END LEDS


// ####### DISPLAY FUNCTIONS #######


// ############## MAIN LOOP ##############

void loop() {
//	customKeypad.tick();
	keypad.tick();
	clksTimer = 0;

	Micros now = micros();
	Micros passed = now - lastProcessTime;
	lastProcessTime = now;

	if (passed > 0) {
		if (sequencer.playing){
			screenSaverCounter = 0;
			advanceClock(passed);
			advanceSteps(passed);
		}
	}
	doStep();

	// DISPLAY SETUP
	display.clearDisplay();

	// ############### SLEEP MODE ###############
	//
//	Serial.println(screenSaverCounter);
	if (screenSaverCounter > screensaverInterval ){
		screenSaverMode = true;
	} else if (screenSaverCounter < 10){
		ssstep = 0;
		ssloop = 0;
//		setAllLEDS(0,0,0);
		screenSaverMode = false;
		nextStepTimeSS = millis();
	} else {
		screenSaverMode = false;
		nextStepTimeSS = millis();
	}


	// ############### POTS ###############
	//
	readPotentimeters();


	// ############### EXTERNAL MODE CHANGE / SYSEX ###############
	if ((!enc_edit && (sysSettings.omxMode != sysSettings.newmode)) || sysSettings.refresh){
		sysSettings.newmode = sysSettings.omxMode;
		sequencer.playingPattern = sysSettings.playingPattern;
		dirtyDisplay = true;
		setAllLEDS(0,0,0);
		dirtyPixels = true;
		sysSettings.refresh = false;
	}


	// ############### ENCODER ###############
	//
	auto u = myEncoder.update();
	if (u.active()) {
		auto amt = u.accel(5); // where 5 is the acceleration factor if you want it, 0 if you don't)
		screenSaverCounter = 0;
//    	Serial.println(u.dir() < 0 ? "ccw " : "cw ");
//    	Serial.println(amt);

		// Change Mode
		if (enc_edit) {
			// set mode
//			int modesize = NUM_OMX_MODES;
			sysSettings.newmode = (OMXMode)constrain(sysSettings.newmode + amt, 0, NUM_OMX_MODES - 1);
			dispMode();
			dirtyDisplayTimer = displayRefreshRate+1;
			dirtyDisplay = true;

		} else if (!noteSelect && !patternParams && !stepRecord){
			switch(sysSettings.omxMode) {
				case MODE_OM: // Organelle Mother
					// CHANGE PAGE
					if (miparam == 0) {
						if(u.dir() < 0){									// if turn ccw
							MM::sendControlChange(CC_OM2, 0, sysSettings.midiChannel);
						} else if (u.dir() > 0){							// if turn cw
							MM::sendControlChange(CC_OM2, 127, sysSettings.midiChannel);
						}
					}
					dirtyDisplay = true;
//					break;
				case MODE_MIDI: // MIDI
					if (midiAUX){
						// change MIDI Background Color
						// midiBg_Hue = constrain(midiBg_Hue + (amt * 32), 0, 65534); // 65535 
						break;
					}
					// CHANGE PAGE
					if (miparam == 0 || miparam == 5 || miparam == 10) {
						mmpage = constrain(mmpage + amt, 0, 2);
						miparam = mmpage * NUM_DISP_PARAMS;
					}
					// PAGE ONE
					if (miparam == 2) {
						int newchan = constrain(sysSettings.midiChannel + amt, 1, 16);
						if (newchan != sysSettings.midiChannel){
							sysSettings.midiChannel = newchan;
						}
					} else if (miparam == 1){
						// set octave
						newoctave = constrain(octave + amt, -5, 4);
						if (newoctave != octave){
							octave = newoctave;
						}
					}
					// PAGE TWO
					if (miparam == 6) {
						int newrrchan = constrain(midiRRChannelCount + amt, 1, 16);
						if (newrrchan != midiRRChannelCount){
							midiRRChannelCount = newrrchan;
							if (midiRRChannelCount == 1){
								midiRoundRobin = false;
							}else{
								midiRoundRobin = true;
							}
						}
					} else if (miparam == 7){
						midiRRChannelOffset = constrain(midiRRChannelOffset + amt, 0, 15);
					} else if (miparam == 8){
						currpgm = constrain(currpgm + amt, 0, 127);

						if (midiRoundRobin){
							for (int q = midiRRChannelOffset+1 ; q < midiRRChannelOffset + midiRRChannelCount+1; q++){
								MM::sendProgramChange(currpgm, q);
							}
						} else {
							MM::sendProgramChange(currpgm, sysSettings.midiChannel);
						}

					} else if (miparam == 9){
						currbank = constrain(currbank + amt, 0, 127);
						// Bank Select is 2 mesages
						MM::sendControlChange(0, 0, sysSettings.midiChannel);
						MM::sendControlChange(32, currbank, sysSettings.midiChannel);
						MM::sendProgramChange(currpgm, sysSettings.midiChannel);
					}
					// PAGE THREE
					if (miparam == 11) {
						potbank = constrain(potbank + amt, 0, NUM_CC_BANKS-1);
					}
					if (miparam == 12) {
						midiSoftThru = constrain(midiSoftThru + amt, 0, 1);
					}
					if (miparam == 13) {
						midiMacro = constrain(midiMacro + amt, 0, nummacromodes);
					}
					if (miparam == 14) {
						midiMacroChan = constrain(midiMacroChan + amt, 1, 16);
					}


					dirtyDisplay = true;
					break;
				case MODE_S1: // SEQ 1
					// FALL THROUGH
				case MODE_S2: // SEQ 2
					// CHANGE PAGE
					if (sqparam == 0 || sqparam == 5 ) {
						sqpage = constrain(sqpage + amt, 0, 1);
						sqparam = sqpage * NUM_DISP_PARAMS;
					}

					// PAGE ONE
					if (sqparam == 1){
						sequencer.playingPattern = constrain(sequencer.playingPattern + amt, 0, 7);
						if (sequencer.getCurrentPattern()->solo) {
							setAllLEDS(0,0,0);
						}
					} else if (sqparam == 2){		// SET TRANSPOSE
						transposeSeq(sequencer.playingPattern, amt); //
						int newtransp = constrain(transpose + amt, -64, 63);
						transpose = newtransp;
					} else if (sqparam == 3){		// SET SWING
						int newswing = constrain(sequencer.getCurrentPattern()->swing + amt, 0, maxswing - 1); // -1 to deal with display values
						swing = newswing;
						sequencer.getCurrentPattern()->swing = newswing;
						//	setGlobalSwing(newswing);
					} else if (sqparam == 4){		// SET TEMPO
						newtempo = constrain(clockbpm + amt, 40, 300);
						if (newtempo != clockbpm){
							// SET TEMPO HERE
							clockbpm = newtempo;
							resetClocks();
						}
					}

					// PAGE TWO
					if (sqparam == 6){				//  MIDI SOLO
//						playingPattern = constrain(playingPattern + amt, 0, 7);
						sequencer.getCurrentPattern()->solo = constrain(sequencer.getCurrentPattern()->solo + amt, 0, 1);
						if (sequencer.getCurrentPattern()->solo)
						{
							setAllLEDS(0,0,0);
						}
					} else if (sqparam == 7){		// SET PATTERN LENGTH
						auto newPatternLen = constrain(sequencer.getPatternLength(sequencer.playingPattern) + amt, 1, NUM_STEPS);
						sequencer.setPatternLength( sequencer.playingPattern, newPatternLen);
						if (sequencer.seqPos[sequencer.playingPattern] >= newPatternLen){
							sequencer.seqPos[sequencer.playingPattern] = newPatternLen-1;
							sequencer.patternPage[sequencer.playingPattern] = getPatternPage(sequencer.seqPos[sequencer.playingPattern]);
						}
					} else if (sqparam == 8){		// SET CLOCK DIV/MULT
						sequencer.getCurrentPattern()->clockDivMultP = constrain(sequencer.getCurrentPattern()->clockDivMultP + amt, 0, NUM_MULTDIVS - 1);
					} else if (sqparam == 9){		// SET CV ON/OFF
						sequencer.getCurrentPattern()->sendCV = constrain(sequencer.getCurrentPattern()->sendCV + amt, 0, 1);
					}
					dirtyDisplay = true;
					break;
				default:
					break;
			}

		} else if (noteSelect || patternParams || stepRecord) {
			switch(sysSettings.omxMode) { // process encoder input depending on mode
				case MODE_MIDI: // MIDI
					break;
				case MODE_S1: // SEQ 1
						// FALL THROUGH

				case MODE_S2: // SEQ 2
					if (patternParams && !enc_edit){ 		// SEQUENCE PATTERN PARAMS SUB MODE
						//CHANGE PAGE
						if (ppparam == 0 || ppparam == 5 || ppparam == 10) {
							pppage = constrain(pppage + amt, 0, 2);		// HARDCODED - FIX WITH SIZE OF PAGES?
							ppparam = pppage * NUM_DISP_PARAMS;
						}

						// PAGE ONE
						if (ppparam == 1) { 					// SET PLAYING PATTERN
							sequencer.playingPattern = constrain(sequencer.playingPattern + amt, 0, 7);
						}
						if (ppparam == 2) { 					// SET LENGTH
							auto newPatternLen = constrain(sequencer.getPatternLength(sequencer.playingPattern) + amt, 1, NUM_STEPS);
							sequencer.setPatternLength(sequencer.playingPattern, newPatternLen);
							if (sequencer.seqPos[sequencer.playingPattern] >= newPatternLen){
								sequencer.seqPos[sequencer.playingPattern] = newPatternLen-1;
								sequencer.patternPage[sequencer.playingPattern] = getPatternPage(sequencer.seqPos[sequencer.playingPattern]);
							}
						}
						if (ppparam == 3) { 					// SET PATTERN ROTATION
							int rotator;
							(u.dir() < 0 ? rotator = -1 : rotator = 1);
//							int rotator = constrain(rotcc, (sequencer.PatternLength(sequencer.playingPattern))*-1, sequencer.PatternLength(sequencer.playingPattern));
							rotationAmt = rotationAmt + rotator;
							if (rotationAmt < 16 && rotationAmt > -16 ){ // NUM_STEPS??
								rotatePattern(sequencer.playingPattern, rotator);
							}
							rotationAmt = constrain(rotationAmt, (sequencer.getPatternLength(sequencer.playingPattern) - 1) * -1, sequencer.getPatternLength(sequencer.playingPattern) - 1);
						}

						if (ppparam == 4) { 					// SET PATTERN CHANNEL
							sequencer.getCurrentPattern()->channel = constrain(sequencer.getCurrentPattern()->channel + amt, 0, 15);
						}
						// PATTERN PARAMS PAGE 2
							//TODO: convert to case statement ??
						if (ppparam == 6) { 					// SET AUTO START STEP
							sequencer.getCurrentPattern()->startstep = constrain(sequencer.getCurrentPattern()->startstep + amt, 0, sequencer.getCurrentPattern()->len);
							//sequencer.getCurrentPattern()->startstep--;
						}
						if (ppparam == 7) { 					// SET AUTO RESET STEP
							int tempresetstep = sequencer.getCurrentPattern()->autoresetstep + amt;
							sequencer.getCurrentPattern()->autoresetstep = constrain(tempresetstep, 0, sequencer.getCurrentPattern()->len+1);
						}
						if (ppparam == 8) { 					// SET AUTO RESET FREQUENCY
							sequencer.getCurrentPattern()->autoresetfreq = constrain(sequencer.getCurrentPattern()->autoresetfreq + amt, 0, 15); // max every 16 times
						}
						if (ppparam == 9) { 					// SET AUTO RESET PROB
							sequencer.getCurrentPattern()->autoresetprob = constrain(sequencer.getCurrentPattern()->autoresetprob + amt, 0, 100); // never, 100% - 33%
						}

						// PAGE THREE
						if (ppparam == 11) { 					// SET CLOCK-DIV-MULT
							sequencer.getCurrentPattern()->clockDivMultP = constrain(sequencer.getCurrentPattern()->clockDivMultP + amt, 0, NUM_MULTDIVS - 1); // set clock div/mult
						}
						if (ppparam == 12) { 					// SET MIDI SOLO
							sequencer.getCurrentPattern()->solo = constrain(sequencer.getCurrentPattern()->solo + amt, 0, 1);
						}

					// STEP RECORD SUB MODE
					} else if (stepRecord && !enc_edit){
						// CHANGE PAGE
						if (srparam == 0 || srparam == 5) {
							srpage = constrain(srpage + amt, 0, 1);		// HARDCODED - FIX WITH SIZE OF PAGES?
							srparam = srpage * NUM_DISP_PARAMS;
						}

						// PAGE ONE
						if (srparam == 1) {		// OCTAVE SELECTION
							newoctave = constrain(octave + amt, -5, 4);
							if (newoctave != octave){
								octave = newoctave;
							}
						}
						if (srparam == 2) {		// STEP SELECTION
							if (u.dir() > 0){
								step_ahead();
							} else if (u.dir() < 0) {
								step_back();
							}
							selectedStep = sequencer.seqPos[sequencer.playingPattern];
						}
						if (srparam == 3) {		// SET NOTE NUM
							int tempNote = getSelectedStep()->note;
							getSelectedStep()->note = constrain(tempNote + amt, 0, 127);
						}
						if (srparam == 4) {
//							playingPattern = constrain(playingPattern + amt, 0, 7);
						}
						// PAGE TWO
						if (srparam == 6) {		// STEP TYPE
							changeStepType(amt);
						}
						if (srparam == 7) {		// STEP PROB
							int tempProb = getSelectedStep()->prob;
							getSelectedStep()->prob = constrain(tempProb + amt, 0, 100); // Note Len between 1-16
						}
						if (srparam == 8) {		// STEP CONDITION
							int tempCondition = getSelectedStep()->condition;
							getSelectedStep()->condition = constrain(tempCondition + amt, 0, 35); // 0-32
						}

					// NOTE SELECT MODE
					} else if (noteSelect && noteSelection && !enc_edit){
						// CHANGE PAGE
						if (nsparam == 0 || nsparam == 5 || nsparam == 10) {
							nspage = constrain(nspage + amt, 0, 2);		// HARDCODED - FIX WITH SIZE OF PAGES?
							nsparam = nspage * NUM_DISP_PARAMS;
						}

						// PAGE THREE
						if (nsparam > 10 && nsparam < 14){
							if(u.dir() < 0){			// RESET PLOCK IF TURN CCW
								int tempmode = nsparam - 11;
								getSelectedStep()->params[tempmode] = -1;
							}
						}
						// PAGE ONE
						if (nsparam == 1) {				// SET NOTE NUM
							int tempNote = getSelectedStep()->note;
							getSelectedStep()->note = constrain(tempNote + amt, 0, 127);
						}
						if (nsparam == 2) { 				// SET OCTAVE
							newoctave = constrain(octave + amt, -5, 4);
							if (newoctave != octave){
								octave = newoctave;
							}
						}
						if (nsparam == 3) { 				// SET VELOCITY
							int tempVel = getSelectedStep()->vel;
							getSelectedStep()->vel = constrain(tempVel + amt, 0, 127);
						}
						if (nsparam == 4) { 				// SET NOTE LENGTH
							int tempLen = getSelectedStep()->len;
							getSelectedStep()->len = constrain(tempLen + amt, 0, 15); // Note Len between 1-16
						}
						// PAGE TWO
						if (nsparam == 6) { 				// SET STEP TYPE
							changeStepType(amt);
						}
						if (nsparam == 7) { 				// SET STEP PROB
							int tempProb = getSelectedStep()->prob;
							getSelectedStep()->prob = constrain(tempProb + amt, 0, 100); // Note Len between 1-16
						}
						if (nsparam == 8) { 				// SET STEP TRIG CONDITION
							int tempCondition = getSelectedStep()->condition;
							getSelectedStep()->condition = constrain(tempCondition + amt, 0, 35); // 0-32
						}


					} else {
						newtempo = constrain(clockbpm + amt, 40, 300);
						if (newtempo != clockbpm){
							// SET TEMPO HERE
							clockbpm = newtempo;
							resetClocks();
						}
					}
					dirtyDisplay = true;
					break;

				case MODE_OM: // Organelle Mother
					break;

				default:
					break;
			}
		}
	}
	// END ENCODER

	// ############### ENCODER BUTTON ###############
	//
	auto s = encButton.update();
	switch (s) {
		// SHORT PRESS
		case Button::Down: //Serial.println("Button down");
			screenSaverCounter = 0;
			
			// what page are we on?
			if (sysSettings.newmode != sysSettings.omxMode && enc_edit) {
				sysSettings.omxMode = sysSettings.newmode;
				seqStop();
				setAllLEDS(0,0,0);
				enc_edit = false;
				dispMode();
			} else if (enc_edit){
				enc_edit = false;
			}

			if(sysSettings.omxMode == MODE_MIDI) {
				// switch midi oct/chan selection
				miparam = (miparam + 1 ) % 15;
				mmpage = miparam / NUM_DISP_PARAMS;
			}
			if(sysSettings.omxMode == MODE_OM) {
				miparam = (miparam + 1 ) % NUM_DISP_PARAMS;
//				MM::sendControlChange(CC_OM1,100,sysSettings.midiChannel);
			}
			if(sysSettings.omxMode == MODE_S1 || sysSettings.omxMode == MODE_S2) {
				if (noteSelect && noteSelection && !patternParams) {
					nsparam = (nsparam + 1 ) % 15;
					if (nsparam > 9){
						nspage = 2;
					}else if (nsparam > 4){
						nspage = 1;
					}else{
						nspage = 0;
					}
				} else if (patternParams) {
					ppparam = (ppparam + 1 ) % 15;
					if (ppparam > 9){
						pppage = 2;
					}else if (ppparam > 4){
						pppage = 1;
					}else{
						pppage = 0;
					}
				} else if (stepRecord) {
					srparam = (srparam + 1 ) % 10;
					if (srparam > 4){
						srpage = 1;
					}else{
						srpage = 0;
					}

				} else {
					sqparam = (sqparam + 1 ) % 10;
					if (sqparam > 4){
						sqpage = 1;
					}else{
						sqpage = 0;
					}
				}
			}
			dirtyDisplay = true;
			break;

		// LONG PRESS
		case Button::DownLong: //Serial.println("Button downlong");
			if (stepRecord) {
				resetPatternDefaults(sequencer.playingPattern);
				clearedFlag = true;
			} else {
				enc_edit = true;
				sysSettings.newmode = sysSettings.omxMode;
				dispMode();
			}
			dirtyDisplay = true;

			break;
		case Button::Up: //Serial.println("Button up");
			if(sysSettings.omxMode == MODE_OM) {
//				MM::sendControlChange(CC_OM1,0,sysSettings.midiChannel);
			}
			break;
		case Button::UpLong: //Serial.println("Button uplong");
			break;
		default:
			break;
	}
	// END ENCODER BUTTON

	// ############### KEY HANDLING ###############
	//
	while(keypad.available()) {
		auto e = keypad.next();
		int thisKey = e.key();
		int keyPos = thisKey - 11;
		int seqKey = keyPos + (sequencer.patternPage[sequencer.playingPattern] * NUM_STEPKEYS);

		if (e.down()){
			screenSaverCounter = 0;
			keyState[thisKey] = true;
		}
	
		if (e.down() && thisKey == 0 && enc_edit) {
			// temp - save whenever the 0 key is pressed in encoder edit mode
			saveToStorage();
			//	Serial.println("EEPROM saved");
		}

		switch(sysSettings.omxMode) {
			case MODE_OM: // Organelle
				// Fall Through

			case MODE_MIDI: // MIDI CONTROLLER

				// ### KEY PRESS EVENTS
				if (midiMacro){
					if (e.clicks()==2 && thisKey == 0) {
						m8AUX = !m8AUX;
						if (!m8AUX) {
							for(int m = 1; m < LED_COUNT; m++){
								strip.setPixelColor(m, LEDOFF);
							}
						}
					}
					if (m8AUX) {
						if (!e.held()){ 
							if (e.down() && (thisKey > 10 && thisKey < 27 )){
								// Mutes / Solos
								m8mutesolo[keyPos] = !m8mutesolo[keyPos];
								int mutePos = keyPos + 12;
								if (m8mutesolo[keyPos]){
									MM::sendNoteOn(mutePos, 1, midiMacroChan);
								} else {
									MM::sendNoteOff(mutePos, 0, midiMacroChan);
								}							
								break;
							} else if (e.down() && (thisKey == 1)){
								// release all mutes
								for(int z = 0; z < 8; z++){
									int mutePos = z + 12;
									if(m8mutesolo[z]){
										m8mutesolo[z] = false;
										MM::sendNoteOff(mutePos, 0, midiMacroChan);
									}
								}								
								break;
							} else if (e.down() && (thisKey == 2)){
								// ?
								break;
							} else if (e.down() && (thisKey == 3)){
								// return to mixer
								// hold shift 4 left 1 down, release shift
								MM::sendNoteOn(1, 1, midiMacroChan); // Shift								
								delay(40); 
								MM::sendNoteOn(6, 1, midiMacroChan); // Up	
								delay(20);							
								MM::sendNoteOff(6, 0, midiMacroChan);
								delay(40); 
								MM::sendNoteOn(4, 1, midiMacroChan); // Left 
								delay(20);
								MM::sendNoteOff(4, 0, midiMacroChan);
								delay(40);
								MM::sendNoteOn(4, 1, midiMacroChan); // Left 
								delay(20);
								MM::sendNoteOff(4, 0, midiMacroChan);
								delay(40);
								MM::sendNoteOn(4, 1, midiMacroChan); // Left 
								delay(20);
								MM::sendNoteOff(4, 0, midiMacroChan);
								delay(40);
								MM::sendNoteOn(4, 1, midiMacroChan); // Left 
								delay(20);
								MM::sendNoteOff(4, 0, midiMacroChan);
								delay(40);
								MM::sendNoteOn(7, 1, midiMacroChan); // Down
								delay(20);
								MM::sendNoteOff(7, 0, midiMacroChan);		
								MM::sendNoteOff(1, 0, midiMacroChan);
								
								break;
							} else if (e.down() && (thisKey == 4)){
								// snap save
								MM::sendNoteOn(1, 1, midiMacroChan); // Shift	
								delay(40);
								MM::sendNoteOn(3, 1, midiMacroChan); // Option
								delay(40);
								MM::sendNoteOff(3, 0, midiMacroChan); 
								MM::sendNoteOff(1, 0, midiMacroChan);
								
								break;
							} else if (e.down() && (thisKey == 5)){
								// snap load
								MM::sendNoteOn(1, 1, midiMacroChan); // Shift								
								delay(40); 
								MM::sendNoteOn(2, 1, midiMacroChan); // Edit
								delay(40); 
								MM::sendNoteOff(2, 0, midiMacroChan);
								MM::sendNoteOff(1, 0, midiMacroChan);								
								
								// then reset mutes/solos
								for(int z = 0; z < 16; z++){
									if(m8mutesolo[z]){
										m8mutesolo[z] = false;
									}
								}

								break;
							} else if (e.down() && (thisKey == 6)){
								// release all solos
								for(int z = 8; z < 16; z++){
									int mutePos = z + 12;
									if(m8mutesolo[z]){
										m8mutesolo[z] = false;
										MM::sendNoteOff(mutePos, 0, midiMacroChan);
									}
								}
								break;
							} else if (e.down() && (thisKey == 7)){
								// ??
								break;
							} else if (e.down() && (thisKey == 8)){
								// ??
								break;
							} else if (e.down() && (thisKey == 9)){
								// waveform
								MM::sendNoteOn(6, 1, midiMacroChan); // Up
								MM::sendNoteOn(7, 1, midiMacroChan); // Down
								MM::sendNoteOn(5, 1, midiMacroChan); // Right
								MM::sendNoteOn(4, 1, midiMacroChan); // Left
								delay(40);

								MM::sendNoteOff(6, 0, midiMacroChan); // Up
								MM::sendNoteOff(7, 0, midiMacroChan); // Down
								MM::sendNoteOff(5, 0, midiMacroChan); // Right
								MM::sendNoteOff(4, 0, midiMacroChan); // Left

								break;
							} else if (e.down() && (thisKey == 10)){
								// play
								MM::sendNoteOn(0, 1, midiMacroChan); // Play
								delay(40);
								MM::sendNoteOff(0, 0, midiMacroChan); // Play

//								MM::sendNoteOn(1, 1, midiMacroChan); // Shift
//								MM::sendNoteOn(3, 1, midiMacroChan); // Option
//								MM::sendNoteOn(2, 1, midiMacroChan); // Edit
//								MM::sendNoteOn(6, 1, midiMacroChan); // Up
//								MM::sendNoteOn(7, 1, midiMacroChan); // Down
//								MM::sendNoteOn(4, 1, midiMacroChan); // Left
//								MM::sendNoteOn(5, 1, midiMacroChan); // Right
								break;
							}
						}	
					}
				}

				// REGULAR KEY PRESSES
				if (!e.held()){ 		// IGNORE LONG PRESS EVENTS
					if (e.down() && thisKey != 0) {
						//Serial.println(" pressed");
						if (thisKey == 11 || thisKey == 12 || thisKey == 1 || thisKey == 2) {
							if (midiAUX){
								if (thisKey == 11 || thisKey == 12){
									int amt = thisKey == 11 ? -1 : 1;
									newoctave = constrain(octave + amt, -5, 4);
									if (newoctave != octave){
										octave = newoctave;
									}
								} else if (thisKey == 1 || thisKey == 2) {
									int chng = thisKey == 1 ? -1 : 1;
									miparam = constrain((miparam + chng ) % 15, 0, 14);
									mmpage = miparam / NUM_DISP_PARAMS;
								}
							} else {
								midiNoteOn(thisKey, defaultVelocity, sysSettings.midiChannel);
							}
						} else {
							midiNoteOn(thisKey, defaultVelocity, sysSettings.midiChannel);
						}
					} else if(!e.down() && thisKey != 0) {
						midiNoteOff(thisKey, sysSettings.midiChannel);
					}
				}
//				Serial.println(e.clicks());

				// AUX KEY
				if (e.down() && thisKey == 0) {
					// Hard coded Organelle stuff
//					MM::sendControlChange(CC_AUX, 100, sysSettings.midiChannel);

					if (!m8AUX){
						midiAUX = true;
					}
					
//					if (midiAUX) {
//						// STOP CLOCK
//						Serial.println("stop clock");
//					} else {
//						// START CLOCK
//						Serial.println("start clock");
//					}
//					midiAUX = !midiAUX;


				} else if (!e.down() && thisKey == 0) {
					// Hard coded Organelle stuff
//					MM::sendControlChange(CC_AUX, 0, sysSettings.midiChannel);
					if (midiAUX) {
						midiAUX = false;
					}
					// turn off leds
					strip.setPixelColor(0, LEDOFF);
					strip.setPixelColor(1, LEDOFF);
					strip.setPixelColor(2, LEDOFF);
					strip.setPixelColor(11, LEDOFF);
					strip.setPixelColor(12, LEDOFF);
				}
				break;

			case MODE_S1: // SEQUENCER 1
				// fall through

			case MODE_S2: // SEQUENCER 2
				// Sequencer row keys

				// ### KEY PRESS EVENTS

				if (e.down() && thisKey != 0) {
					// set key timer to zero
//					keyPressTime[thisKey] = 0;

					// NOTE SELECT
					if (noteSelect){
						if (noteSelection) {		// SET NOTE
							// left and right keys change the octave
							if (thisKey == 11 || thisKey == 26) {
								int amt = thisKey == 11 ? -1 : 1;
								newoctave = constrain(octave + amt, -5, 4);
								if (newoctave != octave){
									octave = newoctave;
								}
							// otherwise select the note
							} else {
								stepSelect = false;
								selectedNote = thisKey;
								int adjnote = notes[thisKey] + (octave * 12);
								getSelectedStep()->note = adjnote;
								if (!sequencer.playing){
									seqNoteOn(thisKey, defaultVelocity, sequencer.playingPattern);
								}
							}
							// see RELEASE events for more
							dirtyDisplay = true;

						} else if (thisKey == 1) {

						} else if (thisKey == 2) {

						} else if (thisKey > 2 && thisKey < 11) { // Pattern select keys
							sequencer.playingPattern = thisKey - 3;
							dirtyDisplay = true;

						} else if ( thisKey > 10 ) {
							selectedStep = seqKey; // was keyPos // set noteSelection to this step
							stepSelect = true;
							noteSelection = true;
							dirtyDisplay = true;
						}

					// PATTERN PARAMS
					} else if (patternParams) {
						if (thisKey == 1) { // F1

						} else if (thisKey == 2) {  // F2

						} else if (thisKey > 2 && thisKey < 11) { // Pattern select keys

							sequencer.playingPattern = thisKey - 3;

							// COPY / PASTE / CLEAR
							if (keyState[1] && !keyState[2]) {
								copyPattern(sequencer.playingPattern);
								displayMessagef("COPIED P-%d", sequencer.playingPattern + 1);
							} else if (!keyState[1] && keyState[2]) {
								pastePattern(sequencer.playingPattern);
								displayMessagef("PASTED P-%d", sequencer.playingPattern + 1);
							} else if (keyState[1] && keyState[2]) {
								clearPattern(sequencer.playingPattern);
								displayMessagef("CLEARED P-%d", sequencer.playingPattern + 1);
							}

							dirtyDisplay = true;
						} else if ( thisKey > 10 ) {
							// set pattern length with key
							auto newPatternLen = thisKey - 10;
							sequencer.setPatternLength(sequencer.playingPattern, newPatternLen );
							if (sequencer.seqPos[sequencer.playingPattern] >= newPatternLen){
								sequencer.seqPos[sequencer.playingPattern] = newPatternLen-1;
								sequencer.patternPage[sequencer.playingPattern] = getPatternPage(sequencer.seqPos[sequencer.playingPattern]);
							}
							dirtyDisplay = true;
						}

					// STEP RECORD
					} else if (stepRecord) {
						selectedNote = thisKey;
						selectedStep = sequencer.seqPos[sequencer.playingPattern];

						int adjnote = notes[thisKey] + (octave * 12);
						getSelectedStep()->note = adjnote;

						if (!sequencer.playing){
							seqNoteOn(thisKey, defaultVelocity, sequencer.playingPattern);
						} // see RELEASE events for more
						stepDirty = true;
						dirtyDisplay = true;

					// MIDI SOLO
					} else if (sequencer.getCurrentPattern()->solo) {
						midiNoteOn(thisKey, defaultVelocity, sequencer.getCurrentPattern()->channel+1);

					// REGULAR SEQ MODE
					} else {
						if (keyState[1] && keyState[2]) {
							seqPages = true;
						}
						if (thisKey == 1) {
//							seqResetFlag = true;					// RESET ALL SEQUENCES TO FIRST/LAST STEP
																	// MOVED DOWN TO AUX KEY

						} else if (thisKey == 2) { 					// CHANGE PATTERN DIRECTION
//							sequencer.getCurrentPattern()->reverse = !sequencer.getCurrentPattern()->reverse;

						// BLACK KEYS - PATTERNS
						} else if (thisKey > 2 && thisKey < 11) { // Pattern select

							// CHECK keyState[] FOR LONG PRESS THINGS

							// If ONLY KEY 1 is down + pattern is not playing = STEP RECORD
							if (keyState[1] && !keyState[2] && !sequencer.playing) {
								sequencer.playingPattern = thisKey-3;
								sequencer.seqPos[sequencer.playingPattern] = 0;
								sequencer.patternPage[sequencer.playingPattern] = 0;	// Step Record always starts from first page
								stepRecord = true;
								displayMessagef("STEP RECORD");
//								dirtyDisplay = true;

							// If KEY 2 is down + pattern = PATTERN MUTE
							} else if (keyState[2]) {
								if (sequencer.getPattern(thisKey - 3)->mute){
									displayMessagef("UNMUTE P-%d", (thisKey - 3)+1);
								}else {
									displayMessagef("MUTE P-%d", (thisKey - 3)+1);
								}
								sequencer.getPattern(thisKey - 3)->mute = !sequencer.getPattern(thisKey-3)->mute;
							} else {
								sequencer.playingPattern = thisKey - 3;
							}
							dirtyDisplay = true;

						// SEQUENCE 1-16 STEP KEYS
						} else if (thisKey > 10) {

							if (keyState[1] && keyState[2]) {		// F1+F2 HOLD
								if (!stepRecord){ // IGNORE LONG PRESSES IN STEP RECORD
									if (keyPos <= getPatternPage(sequencer.getCurrentPattern()->len) ){
										sequencer.patternPage[sequencer.playingPattern] = keyPos;
									}
									displayMessagef("PATT PAGE %d", keyPos + 1);
								}
							} else if (keyState[1]) {		// F1 HOLD
									if (!stepRecord && !patternParams){ 		// IGNORE LONG PRESSES IN STEP RECORD and Pattern Params
										selectedStep = thisKey - 11; // set noteSelection to this step
										noteSelect = true;
										stepSelect = true;
										noteSelection = true;
										dirtyDisplay = true;
										displayMessagef("NOTE SELECT");
										// re-toggle the key you just held
//										if (getSelectedStep()->trig == TRIGTYPE_PLAY || getSelectedStep()->trig == TRIGTYPE_MUTE ) {
//											getSelectedStep()->trig = (getSelectedStep()->trig == TRIGTYPE_PLAY ) ? TRIGTYPE_MUTE : TRIGTYPE_PLAY;
//										}
									}

							} else if (keyState[2]) {		// F2 HOLD

							} else {
								// TOGGLE STEP ON/OFF
								if (sequencer.getCurrentPattern()->steps[seqKey].trig == TRIGTYPE_PLAY || sequencer.getCurrentPattern()->steps[seqKey].trig == TRIGTYPE_MUTE ) {
									sequencer.getCurrentPattern()->steps[seqKey].trig = (sequencer.getCurrentPattern()->steps[seqKey].trig == TRIGTYPE_PLAY ) ? TRIGTYPE_MUTE : TRIGTYPE_PLAY;
								}
							}
						}
					}
				}

				// ### KEY RELEASE EVENTS

				if (!e.down() && thisKey != 0) {
					// MIDI SOLO
					if (sequencer.getCurrentPattern()->solo) {
						midiNoteOff(thisKey, sequencer.getCurrentPattern()->channel+1);
					}
				}

				if (!e.down() && thisKey != 0 && (noteSelection || stepRecord) && selectedNote > 0) {
					if (!sequencer.playing){
						seqNoteOff(thisKey, sequencer.playingPattern);
					}
					if (stepRecord && stepDirty) {
						step_ahead();
						stepDirty = false;
						// EXIT STEP RECORD AFTER THE LAST STEP IN PATTERN
						if (sequencer.seqPos[sequencer.playingPattern] == 0){
							stepRecord = false;
						}
					}
				}

				// AUX KEY PRESS EVENTS

				if (e.down() && thisKey == 0) {

					if (noteSelect){
						if (noteSelection){
							selectedStep = 0;
							selectedNote = 0;
						} else {

						}
						noteSelection = false;
						noteSelect = !noteSelect;
						dirtyDisplay = true;

					} else if (patternParams){
						patternParams = !patternParams;
						dirtyDisplay = true;

					} else if (stepRecord){
						stepRecord = !stepRecord;
						dirtyDisplay = true;
					} else if (seqPages){
						seqPages = false;
					} else {
						if (keyState[1] || keyState[2]) { 				// CHECK keyState[] FOR LONG PRESS OF FUNC KEYS
							if (keyState[1]) {
								sequencer.seqResetFlag = true;		// RESET ALL SEQUENCES TO FIRST/LAST STEP
								displayMessagef("RESET");

							} else if (keyState[2]) { 					// CHANGE PATTERN DIRECTION
								sequencer.getCurrentPattern()->reverse = !sequencer.getCurrentPattern()->reverse;
								if (sequencer.getCurrentPattern()->reverse) {
									displayMessagef("<< REV");
								} else{
									displayMessagef("FWD >>");
								}
							}
							dirtyDisplay = true;
						} else {
							if (sequencer.playing){
								// stop transport
								sequencer.playing = 0;
								allNotesOff();
	//							Serial.println("stop transport");
								seqStop();
							} else {
								// start transport
	//							Serial.println("start transport");
								seqStart();
							}
						}
					}

				// AUX KEY RELEASE EVENTS

				} else if (!e.down() && thisKey == 0) {

				}
				
//				strip.show();
				break;

			default:
				break;
		}
		// END MODE SWITCH

		if (!e.down()){
			keyState[thisKey] = false;
		}
		if (!keyState[1] && !keyState[2]) {
			seqPages = false;
		}

		
		// ### LONG KEY SWITCH PRESS
		if (e.held()) {
			// DO LONG PRESS THINGS
			switch (sysSettings.omxMode){
				case MODE_MIDI:
					break;
				case MODE_S1:
					// fall through
				case MODE_S2:
					if (!sequencer.getCurrentPattern()->solo){
						// TODO: access key state directly in omx_keypad.h
						if (keyState[1] && keyState[2]) {
							seqPages = true;
						} else if (!keyState[1] && !keyState[2]) { // SKIP LONG PRESS IF FUNC KEYS ARE ALREDY HELD
							if (thisKey > 2 && thisKey < 11){ // skip AUX key, get pattern keys
								patternParams = true;
								dirtyDisplay = true;
								displayMessagef("PATT PARAMS");
							} else if (thisKey > 10){
								if (!stepRecord && !patternParams){ 		// IGNORE LONG PRESSES IN STEP RECORD and Pattern Params
									selectedStep = (thisKey - 11) + (sequencer.patternPage[sequencer.playingPattern] * NUM_STEPKEYS); // set noteSelection to this step
									noteSelect = true;
									stepSelect = true;
									noteSelection = true;
									dirtyDisplay = true;
									displayMessagef("NOTE SELECT");
									// re-toggle the key you just held
//									if ( getSelectedStep()->trig == TRIGTYPE_PLAY || getSelectedStep()->trig == TRIGTYPE_MUTE ) {
//										getSelectedStep()->trig = ( getSelectedStep()->trig == TRIGTYPE_PLAY ) ? TRIGTYPE_MUTE : TRIGTYPE_PLAY;
//									}
								}
							}
						}
					}
					break;
				default:
					break;
			} // END MODE SWITCH
		} // END IF HELD

	}  // END KEYS WHILE

	if (!screenSaverMode){

		// ############### MODES DISPLAY  ##############
		//
		if(messageTextTimer > 0) {
			messageTextTimer -= passed;
			if(messageTextTimer <= 0) {
				dirtyDisplay = true;
				messageTextTimer = 0;
			}
		}
	
		switch(sysSettings.omxMode){
			case MODE_OM: 						// ############## ORGANELLE MODE
				// FALL THROUGH
			case MODE_MIDI:							// ############## MIDI KEYBOARD
				//playingPattern = 0; 		// DEFAULT MIDI MODE TO THE FIRST PATTERN SLOT
				midi_leds();				// SHOW LEDS
				if (dirtyDisplay){			// DISPLAY
					if (!enc_edit){
						int pselected = miparam % NUM_DISP_PARAMS;
						if (mmpage == 0){
							dispGenericMode(SUBMODE_MIDI, pselected);
						} else if (mmpage == 1){
							dispGenericMode(SUBMODE_MIDI2, pselected);
						} else if (mmpage == 2){
							dispGenericMode(SUBMODE_MIDI3, pselected);
						}
					}
				}
				break;
			case MODE_S1: 						// ############## SEQUENCER 1
				// FALL THROUGH
			case MODE_S2: 						// ############## SEQUENCER 2
				// MIDI SOLO
				if (sequencer.getCurrentPattern()->solo) {
					midi_leds();
				}
				if (dirtyDisplay){			// DISPLAY
					if (!enc_edit && messageTextTimer == 0){	// show only if not encoder edit or dialog display
						if (!noteSelect and !patternParams and !stepRecord){
							int pselected = sqparam % NUM_DISP_PARAMS;
							if (sqpage == 0){
								dispGenericMode(SUBMODE_SEQ, pselected);
							} else if (sqpage == 1){
								dispGenericMode(SUBMODE_SEQ2, pselected);
							}
						}
						if (noteSelect) {
							int pselected = nsparam % NUM_DISP_PARAMS;
							if (nspage == 0){
								dispGenericMode(SUBMODE_NOTESEL, pselected);
							} else if (nspage == 1){
								dispGenericMode(SUBMODE_NOTESEL2, pselected);
							} else if (nspage == 2){
								dispGenericMode(SUBMODE_NOTESEL3, pselected);
							}
						}
						if (patternParams) {
							int pselected = ppparam % NUM_DISP_PARAMS;
							if (pppage == 0){
								dispGenericMode(SUBMODE_PATTPARAMS, pselected);
							} else if (pppage == 1){
								dispGenericMode(SUBMODE_PATTPARAMS2, pselected);
							} else if (pppage == 2){
								dispGenericMode(SUBMODE_PATTPARAMS3, pselected);
							}

						}
						if (stepRecord) {
							int pselected = srparam % NUM_DISP_PARAMS;
							if (srpage == 0){
								dispGenericMode(SUBMODE_STEPREC, pselected);
							} else if (srpage == 1){
								dispGenericMode(SUBMODE_NOTESEL2, pselected);
							}
						}
					}
				}
				break;
			default:
				break;
		} // END SWITCH

	} else {	// if !screensaver
		switch(sysSettings.omxMode){
			case MODE_OM: 						// ############## ORGANELLE MODE
				// FALL THROUGH
			case MODE_MIDI:							// ############## MIDI KEYBOARD
					sleepModeOne();
				break;
			default:
				break;
		}
		// clear display
		display.clearDisplay();
		dirtyDisplay = true;
	}

	// DISPLAY at end of loop

	if (dirtyDisplay) {
		if (dirtyDisplayTimer > displayRefreshRate) {
			display.display();
			dirtyDisplay = false;
			dirtyDisplayTimer = 0;
		}
	}

	// are pixels dirty
	if (dirtyPixels) {
		strip.show();
		dirtyPixels = false;
	}

	while (MM::usbMidiRead()) {
		// incoming messages - see handlers
	}
	while (MM::midiRead()) {
		// ignore incoming messages
	}

} // ######## END MAIN LOOP ########


void cvNoteOn(int notenum){
	if (notenum>=midiLowestNote && notenum <midiHightestNote){
		pitchCV = static_cast<int>(roundf( (notenum - midiLowestNote) * stepsPerSemitone)); // map (adjnote, 36, 91, 0, 4080);
		digitalWrite(CVGATE_PIN, HIGH);
		analogWrite(CVPITCH_PIN, pitchCV);
	}
}
void cvNoteOff(){
	digitalWrite(CVGATE_PIN, LOW);
//	analogWrite(CVPITCH_PIN, 0);
}

// #### Inbound MIDI callbacks
void OnNoteOn(byte channel, byte note, byte velocity) {
	if (midiSoftThru) {
		MM::sendNoteOnHW(note, velocity, channel);
	}
	if (midiInToCV){
		cvNoteOn(note);
	}
	if (sysSettings.omxMode == MODE_MIDI){
		midiLastNote = note;
		int whatoct = (note / 12);
		int thisKey;
		uint32_t keyColor = MIDINOTEON;

		if ( (whatoct % 2) == 0) {
			thisKey = note - (12 * whatoct);
		} else {
			thisKey = note - (12 * whatoct) + 12;
		}
		if (whatoct == 0){ // ORANGE,YELLOW,GREEN,MAGENTA,CYAN,BLUE,LIME,LTPURPLE
		} else if(whatoct == 1){ keyColor = ORANGE;
		} else if(whatoct == 2){ keyColor = YELLOW;
		} else if(whatoct == 3){ keyColor = GREEN;
		} else if(whatoct == 4){ keyColor = MAGENTA;
		} else if(whatoct == 5){ keyColor = CYAN;
		} else if(whatoct == 6){ keyColor = LIME;
		} else if(whatoct == 7){ keyColor = CYAN;
		}
		strip.setPixelColor(midiKeyMap[thisKey], keyColor);         //  Set pixel's color (in RAM)
	//	dirtyPixels = true;
		strip.show();
		dirtyDisplay = true;
	}
}
void OnNoteOff(byte channel, byte note, byte velocity) {
	if (midiInToCV){
		cvNoteOff();
	}
	if (sysSettings.omxMode == MODE_MIDI){
		int whatoct = (note / 12);
		int thisKey;
		if ( (whatoct % 2) == 0) {
			thisKey = note - (12 * whatoct);
		} else {
			thisKey = note - (12 * whatoct) + 12;
		}
		strip.setPixelColor(midiKeyMap[thisKey], LEDOFF);         //  Set pixel's color (in RAM)
	//	dirtyPixels = true;
		strip.show();
		dirtyDisplay = true;
	}
	if (midiSoftThru) {
		MM::sendNoteOffHW(note, velocity, channel);
	}
}
void OnControlChange(byte channel, byte control,  byte value) {
	if (midiSoftThru) {
		MM::sendControlChangeHW(control, value, channel);
	}
}

void OnSysEx(const uint8_t *data, uint16_t length, bool complete) {
	sysEx->processIncomingSysex(data, length);
}

// #### Outbound MIDI Mode note on/off
void midiNoteOn(int notenum, int velocity, int channel) {
	int adjnote = notes[notenum] + (octave * 12); // adjust key for octave range
	rrChannel = (rrChannel % midiRRChannelCount) + 1;
	int adjchan = rrChannel;

	if (adjnote>=0 && adjnote <128){
		midiLastNote = adjnote;

		// keep track of adjusted note when pressed so that when key is released we send
		// the correct note off message
		midiKeyState[notenum] = adjnote;

		// RoundRobin Setting?
		if (midiRoundRobin) {
			adjchan = rrChannel + midiRRChannelOffset;
		} else {
			adjchan = channel;
		}
		midiChannelState[notenum] = adjchan;
		MM::sendNoteOn(adjnote, velocity, adjchan);
		// CV
		cvNoteOn(adjnote);
	}

	strip.setPixelColor(notenum, MIDINOTEON);         //  Set pixel's color (in RAM)
	dirtyPixels = true;
	omxidsp.setDispDirty();
}

void midiNoteOff(int notenum, int channel) {
	// we use the key state captured at the time we pressed the key to send the correct note off message
	int adjnote = midiKeyState[notenum];
	int adjchan = midiChannelState[notenum];
	if (adjnote>=0 && adjnote <128){
		MM::sendNoteOff(adjnote, 0, adjchan);
		// CV off
		cvNoteOff();
		midiKeyState[notenum] = -1;
	}
	
	strip.setPixelColor(notenum, LEDOFF);
	dirtyPixels = true;
	omxidsp.setDispDirty();
}




// #### LED STUFF
// Rainbow cycle along whole strip. Pass delay time (in ms) between frames.
void rainbow(int wait) {
	// Hue of first pixel runs 5 complete loops through the color wheel.
	// Color wheel has a range of 65536 but it's OK if we roll over, so
	// just count from 0 to 5*65536. Adding 256 to firstPixelHue each time
	// means we'll make 5*65536/256 = 1280 passes through this outer loop:
	for(long firstPixelHue = 0; firstPixelHue < 1*65536; firstPixelHue += 256) {
		for(int i=0; i<strip.numPixels(); i++) { // For each pixel in strip...
			// Offset pixel hue by an amount to make one full revolution of the
			// color wheel (range of 65536) along the length of the strip
			// (strip.numPixels() steps):
			int pixelHue = firstPixelHue + (i * 65536L / strip.numPixels());

			// strip.ColorHSV() can take 1 or 3 arguments: a hue (0 to 65535) or
			// optionally add saturation and value (brightness) (each 0 to 255).
			// Here we're using just the single-argument hue variant. The result
			// is passed through strip.gamma32() to provide 'truer' colors
			// before assigning to each pixel:
			strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(pixelHue)));
		}
		strip.show(); // Update strip with new contents
		delay(wait);  // Pause for a moment
	}
}
void setAllLEDS(int R, int G, int B) {
	for(int i=0; i<LED_COUNT; i++) { // For each pixel...
		strip.setPixelColor(i, strip.Color(R, G, B));
	}
	dirtyPixels = true;
}

// #### COLOR FUNCTIONS
// Input a value 0 to 255 to get a color value.
// The colours are a transition r - g - b - back to r.
uint32_t Wheel(byte WheelPos) {
	WheelPos = 255 - WheelPos;
	if(WheelPos < 85) {
		return strip.Color(255 - WheelPos * 3, 0, WheelPos * 3);
	}
	if(WheelPos < 170) {
		WheelPos -= 85;
		return strip.Color(0, WheelPos * 3, 255 - WheelPos * 3);
	}
	WheelPos -= 170;
	return strip.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
}
void colorWipe(byte red, byte green, byte blue, int SpeedDelay) {
  for(uint16_t i=0; i<strip.numPixels(); i++) {
      strip.setPixelColor(i, strip.Color(red, green, blue));
      strip.show();
      delay(SpeedDelay);
  }
}
void theaterChaseRainbow(uint8_t wait) {		//Theatre-style crawling lights with rainbow effect
	for (int j=0; j < 256; j++) {     // cycle all 256 colors in the wheel
		for (int q=0; q < 3; q++) {
			for (uint16_t i=0; i < strip.numPixels(); i=i+3) {
				strip.setPixelColor(i+q, Wheel( (i+j) % 255));    //turn every third pixel on
			}
			strip.show();
			delay(wait);
			for (uint16_t i=0; i < strip.numPixels(); i=i+3) {
				strip.setPixelColor(i+q, 0);        //turn every third pixel off
			}
		}
	}
}
void CylonBounce(byte red, byte green, byte blue, int EyeSize, int SpeedDelay, int ReturnDelay, int start, int end){
  for(int i = start; i < end-EyeSize-2; i++) {
    setAllLEDS(0,0,0);
    strip.setPixelColor(i, strip.Color(red/10, green/10, blue/10));
    for(int j = 1; j <= EyeSize; j++) {
      strip.setPixelColor(i+j, strip.Color(red, green, blue));
    }
    strip.setPixelColor(i+EyeSize+1, strip.Color(red/10, green/10, blue/10));
    strip.show();
    delay(SpeedDelay);
  }
  delay(ReturnDelay);
  for(int i = end-EyeSize-2; i > start; i--) {
    setAllLEDS(0,0,0);
    strip.setPixelColor(i, strip.Color(red/10, green/10, blue/10));
    for(int j = 1; j <= EyeSize; j++) {
      strip.setPixelColor(i+j, strip.Color(red, green, blue));
    }
    strip.setPixelColor(i+EyeSize+1, strip.Color(red/10, green/10, blue/10));
    strip.show();
    delay(SpeedDelay);
  }
  delay(ReturnDelay);
}
void RolandFill(byte red, byte green, byte blue, int start, int end, int SpeedDelay){
	for(uint16_t j=end; j>start; j--) {
		for(uint16_t i=start; i<end; i++) {
			strip.setPixelColor(i, strip.Color(red, green, blue));
			strip.show();
			if (i < j){
				strip.setPixelColor(i, 0);

			}
			if (!screenSaverMode){
				return;
			}
		}
		strip.setPixelColor(j, strip.Color(red, green, blue));
		strip.show();
	}
	for(uint16_t j=end; j>start-1; j--) {
		for(uint16_t i=start; i<end+1; i++) {
			strip.setPixelColor(i, strip.Color(red, green, blue));
			strip.show();
			if (i > j){
				strip.setPixelColor(i, 0);

			}
			if (!screenSaverMode){
				return;
			}
		}
		if (j != start){
			strip.setPixelColor(j, strip.Color(red, green, blue));
		}
		strip.show();
	}
}

// #### OLED STUFF




// TODO: move to sequencer.h
void initPatterns( void ) {
	// default to GM Drum Map for now -- GET THIS FROM patternDefaultNoteMap instead
//	uint8_t initNotes[NUM_PATTERNS] = {
//		36,
//		38,
//		37,
//		39,
//		42,
//		46,
//		49,
//		51 };

	StepNote stepNote = { 0, 100, 0, TRIGTYPE_MUTE, { -1, -1, -1, -1, -1 }, 100, 0, STEPTYPE_NONE };
					// {note, vel, len, TRIGTYPE, {params0, params1, params2, params3, params4}, prob, condition, STEPTYPE}

	for ( int i=0; i<NUM_PATTERNS; i++ ) {
		auto pattern = sequencer.getPattern(i);

		stepNote.note = sequencer.patternDefaultNoteMap[i]; // Defined in sequencer.h

		for ( int j=0; j<NUM_STEPS; j++ ) {
			memcpy( &pattern->steps[j], &stepNote, sizeof(StepNote) );
		}

		// TODO: move to sequencer.h
		pattern->len = 15;
		pattern->channel = i;		// 0 - 15 becomes 1 - 16
		pattern->startstep = 0;
		pattern->autoresetstep = 0;
		pattern->autoresetfreq = 0;
		pattern->current_cycle = 1;
		pattern->rndstep = 3;
		pattern->clockDivMultP = 2;
		pattern->autoresetprob = 0;
		pattern->swing = 0;
		pattern->reverse = false;
		pattern->mute = false;
		pattern->autoreset = false;
		pattern->solo = false;
		pattern->sendCV = false;
	}
}

void saveHeader( void ) {
	// 1 byte for EEPROM version
	storage->write( EEPROM_HEADER_ADDRESS + 0, EEPROM_VERSION );

	// 1 byte for mode
	storage->write( EEPROM_HEADER_ADDRESS + 1, (uint8_t)sysSettings.omxMode );

	// 1 byte for the active pattern
	storage->write(EEPROM_HEADER_ADDRESS + 2, (uint8_t)sequencer.playingPattern);

	// 1 byte for Midi channel
	uint8_t unMidiChannel = (uint8_t)(sysSettings.midiChannel - 1);
	storage->write( EEPROM_HEADER_ADDRESS + 3, unMidiChannel );

	for (int b=0; b< NUM_CC_BANKS; b++){
		for ( int i=0; i<NUM_CC_POTS; i++ ) {
			storage->write( EEPROM_HEADER_ADDRESS + 4 + i + (5*b), pots[b][i] );
		}
	}
}

// returns true if the header contained initialized data
// false means we shouldn't attempt to load any further information
bool loadHeader( void ) {
	uint8_t version = storage->read(EEPROM_HEADER_ADDRESS + 0);

//	char buf[64];
//	snprintf( buf, sizeof(buf), "EEPROM Header Version is %d\n", version );
//	Serial.print( buf );

	// Uninitalized EEPROM memory is filled with 0xFF
	if ( version == 0xFF ) {
		// EEPROM was uninitialized
//		Serial.println( "version was 0xFF" );
		return false;
	}

	if ( version != EEPROM_VERSION ) {
		// write an adapter if we ever need to increment the EEPROM version and also save the existing patterns
		// for now, return false will essentially reset the state
//		Serial.println( "version not matched" );
		return false;
	}

	sysSettings.omxMode = (OMXMode)storage->read( EEPROM_HEADER_ADDRESS + 1 );
	sequencer.playingPattern = storage->read(EEPROM_HEADER_ADDRESS + 2);
	sysSettings.playingPattern = sequencer.playingPattern;

	uint8_t unMidiChannel = storage->read( EEPROM_HEADER_ADDRESS + 3 );
	sysSettings.midiChannel = unMidiChannel + 1;

	for (int b=0; b < NUM_CC_BANKS; b++){
		for ( int i=0; i<NUM_CC_POTS; i++ ) {
			pots[b][i] = storage->read( EEPROM_HEADER_ADDRESS + 4 + i + (5*b));
		}
	}
	return true;
}

void savePatterns( void ) {
	int patternSize = serializedPatternSize(storage->isEeprom());
	int nLocalAddress = EEPROM_PATTERN_ADDRESS;

	for (int i=0; i<NUM_PATTERNS; i++) {
		auto pattern = (byte*) sequencer.getPattern(i);
		for (int j = 0; j < patternSize; j++) {
			storage->write(nLocalAddress + j, *pattern++);
		}

		nLocalAddress += patternSize;
	}
}

void loadPatterns( void ) {
	int patternSize = serializedPatternSize(storage->isEeprom());
	int nLocalAddress = EEPROM_PATTERN_ADDRESS;

	for (int i = 0; i < NUM_PATTERNS; i++) {
		auto pattern = Pattern{};
		auto current = (byte*)&pattern;
		for (int j = 0; j < patternSize; j++) {
			*current = storage->read(nLocalAddress + j);
			current++;
		}
		sequencer.patterns[i] = pattern;

		nLocalAddress += patternSize;
	}
}

// currently saves everything ( mode + patterns )
void saveToStorage( void ) {
//	Serial.println( "saving..." );
	saveHeader();
	savePatterns();
}

// currently loads everything ( mode + patterns )
bool loadFromStorage( void ) {
	// This load can happen soon after Serial.begin - enable this 'wait for Serial' if you need to Serial.print during loading
	// while( !Serial );

//	Serial.println( "read the header" );
	bool bContainedData = loadHeader();

	if ( bContainedData ) {
		// Serial.println( "loading patterns" );
		loadPatterns();
		return true;
	}

	return false;
}
