// OMX-27 MIDI KEYBOARD / SEQUENCER

// v1.12.1alpha

//
// Steven Noreyko, Last update: July 2022
//
//
//	Big thanks to:
//	John Park and Gerald Stevens for initial testing and feature ideas
//	mzero for immense amounts of code coaching/assistance
//	drjohn for support
//  Additional code contributions: Matt Boone, Steven Zydek, Chris Atkins, Will Winder, Michael P Jones

#include <functional>
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
#include "omx_mode_midi_keyboard.h"
#include "omx_mode_sequencer.h"
#include "omx_mode_grids.h"
#include "omx_mode_euclidean.h"
#include "omx_mode_chords.h"
#include "omx_screensaver.h"
#include "omx_leds.h"
#include "music_scales.h"

#define RAM_MONITOR 1

#if RAM_MONITOR
#include "RamMonitor.h"
#endif

OmxModeMidiKeyboard omxModeMidi;
OmxModeSequencer omxModeSeq;
OmxModeGrids omxModeGrids;
OmxModeEuclidean omxModeEuclid;
OmxModeChords omxModeChords;

OmxModeInterface *activeOmxMode;

OmxScreensaver omxScreensaver;

MusicScales globalScale;

// storage of pot values; current is in the main loop; last value is for midi output
int volatile currentValue[NUM_CC_POTS];
int lastMidiValue[NUM_CC_POTS];
int potMin = 0;
int potMax = 8190;
int temp;

Micros lastProcessTime;

uint8_t RES;
uint16_t AMAX;
int V_scale;

// ENCODER
Encoder myEncoder(12, 11); // encoder pins on hardware
Button encButton(0);	   // encoder button pin on hardware
// long newPosition = 0;
// long oldPosition = -999;

// KEYPAD
// initialize an instance of custom Keypad class
unsigned long longPressInterval = 800;
unsigned long clickWindow = 200;
OMXKeypad keypad(longPressInterval, clickWindow, makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// setup EEPROM/FRAM storage
Storage *storage;
SysEx *sysEx;

#if RAM_MONITOR
RamMonitor ram;
uint32_t reporttime;

void report_ram_stat(const char *aname, uint32_t avalue)
{
	Serial.print(aname);
	Serial.print(": ");
	Serial.print((avalue + 512) / 1024);
	Serial.print(" Kb (");
	Serial.print((((float)avalue) / ram.total()) * 100, 1);
	Serial.println("%)");
};

void report_profile_time(const char *aname, uint32_t avalue)
{
	Serial.print(aname);
	Serial.print(": ");
	Serial.print(avalue);
	Serial.println("\n");
};

void report_ram()
{
	bool lowmem;
	bool crash;

	Serial.println("==== memory report ====");

	report_ram_stat("free", ram.adj_free());
	report_ram_stat("stack", ram.stack_total());
	report_ram_stat("heap", ram.heap_total());

	lowmem = ram.warning_lowmem();
	crash = ram.warning_crash();
	if (lowmem || crash)
	{
		Serial.println();

		if (crash)
			Serial.println("**warning: stack and heap crash possible");
		else if (lowmem)
			Serial.println("**warning: unallocated memory running low");
	};

	Serial.println();
};
#endif

// ####### SETUP #######

void setup()
{
	Serial.begin(115200);
	//	while( !Serial );
	storage = Storage::initStorage();
	sysEx = new SysEx(storage, &sysSettings);

#if RAM_MONITOR
	ram.initialize();
#endif

	// incoming usbMIDI callbacks
	usbMIDI.setHandleNoteOff(OnNoteOff);
	usbMIDI.setHandleNoteOn(OnNoteOn);
	usbMIDI.setHandleControlChange(OnControlChange);
	usbMIDI.setHandleSystemExclusive(OnSysEx);

	// clksTimer = 0; // TODO - didn't see this used anywhere
	omxScreensaver.resetCounter();
	// ssstep = 0;

	lastProcessTime = micros();
	omxUtil.resetClocks();

	randomSeed(analogRead(13));
	srand(analogRead(13));

	// SET ANALOG READ resolution to teensy's 13 usable bits
	analogReadResolution(13);

	// initialize ResponsiveAnalogRead
	for (int i = 0; i < potCount; i++)
	{
		potSettings.analog[i] = new ResponsiveAnalogRead(0, true, .001);
		potSettings.analog[i]->setAnalogResolution(1 << 13);

		// ResponsiveAnalogRead is designed for 10-bit ADCs
		// meanining its threshold defaults to 4. Let's bump that for
		// our 13-bit adc by setting it to 4 << (13-10)
		potSettings.analog[i]->setActivityThreshold(32);

		currentValue[i] = 0;
		lastMidiValue[i] = 0;
	}


	// HW MIDI
	MM::begin();

	// CV gate pin
	pinMode(CVGATE_PIN, OUTPUT);

	// set DAC Resolution CV/GATE
	RES = 12;
	analogWriteResolution(RES); // set resolution for DAC
	AMAX = pow(2, RES);
	V_scale = 64; // pow(2,(RES-7)); 4095 max
	analogWrite(CVPITCH_PIN, 0);

	globalScale.calculateScale(scaleConfig.scaleRoot, scaleConfig.scalePattern);
	omxModeMidi.SetScale(&globalScale);
	omxModeSeq.SetScale(&globalScale);
	omxModeGrids.SetScale(&globalScale);
	omxModeEuclid.SetScale(&globalScale);
	omxModeChords.SetScale(&globalScale);

	// Load from EEPROM
	bool bLoaded = loadFromStorage();
	if (!bLoaded)
	{
		// Failed to load due to initialized EEPROM or version mismatch
		// defaults
		// sysSettings.omxMode = DEFAULT_MODE;
		sequencer.playingPattern = 0;
		sysSettings.playingPattern = 0;
		sysSettings.midiChannel = 1;
		pots[0][0] = CC1;
		pots[0][1] = CC2;
		pots[0][2] = CC3;
		pots[0][3] = CC4;
		pots[0][4] = CC5;

		omxModeSeq.initPatterns();

		changeOmxMode(DEFAULT_MODE);
		// initPatterns();
		saveToStorage();
	}

	// changeOmxMode(MODE_EUCLID);

	// Init Display
	omxDisp.setup();

	// Startup screen
	omxDisp.drawStartupScreen();

	// Keypad
	//	customKeypad.begin();
	keypad.begin();

	// LEDs
	omxLeds.initSetup();

	omxScreensaver.InitSetup();

#if RAM_MONITOR
	reporttime = millis();
#endif
}

// ####### END SETUP #######

// ####### SEQUENCER LEDS #######

void changeOmxMode(OMXMode newOmxmode)
{
//	Serial.println((String)"NewMode: " + newOmxmode);
	sysSettings.omxMode = newOmxmode;
	sysSettings.newmode = newOmxmode;

	if(activeOmxMode != nullptr)
	{
		activeOmxMode->onModeDeactivated();
	}

	switch (newOmxmode)
	{
	case MODE_MIDI:
		omxModeMidi.setMidiMode();
		activeOmxMode = &omxModeMidi;
		break;
	case MODE_CHORDS:
		activeOmxMode = &omxModeChords;
		break;
	case MODE_S1:
		omxModeSeq.setSeq1Mode();
		activeOmxMode = &omxModeSeq;
		break;
	case MODE_S2:
		omxModeSeq.setSeq2Mode();
		activeOmxMode = &omxModeSeq;
		break;
	case MODE_OM:
		omxModeMidi.setOrganelleMode();
		activeOmxMode = &omxModeMidi;
		break;
	case MODE_GRIDS:
		activeOmxMode = &omxModeGrids;
		break;
	case MODE_EUCLID:
		activeOmxMode = &omxModeEuclid;
		break;
	default:
		omxModeMidi.setMidiMode();
		activeOmxMode = &omxModeMidi;
		break;
	}

	activeOmxMode->onModeActivated();
}

// ####### END LEDS

// ############## MAIN LOOP ##############

void loop()
{
	//	customKeypad.tick();
	keypad.tick();
	// clksTimer = 0; // TODO - didn't see this used anywhere

	Micros now = micros();
	Micros passed = now - lastProcessTime;
	lastProcessTime = now;

	sysSettings.timeElasped = passed;

	if (passed > 0)
	{
		if (sequencer.playing)
		{
			omxScreensaver.resetCounter(); // screenSaverCounter = 0;
			omxUtil.advanceClock(activeOmxMode, passed);
			omxUtil.advanceSteps(passed);
		}
	}

	// Micros timeStart = micros();
	activeOmxMode->loopUpdate(passed);


	// DISPLAY SETUP
	display.clearDisplay();

	// ############### SLEEP MODE ###############
	//
	//	Serial.println(screenSaverCounter);
	omxScreensaver.updateScreenSaverState();
	sysSettings.screenSaverMode = omxScreensaver.shouldShowScreenSaver();

	// ############### POTS ###############
	//
	readPotentimeters();

	bool omxModeChangedThisFrame = false;

	// ############### EXTERNAL MODE CHANGE / SYSEX ###############
	if ((!encoderConfig.enc_edit && (sysSettings.omxMode != sysSettings.newmode)) || sysSettings.refresh)
	{
		sysSettings.newmode = sysSettings.omxMode;
		changeOmxMode(sysSettings.omxMode);
		omxModeChangedThisFrame = true;

		sequencer.playingPattern = sysSettings.playingPattern;
		omxDisp.setDirty();
		omxLeds.setAllLEDS(0, 0, 0);
		omxLeds.setDirty();
		sysSettings.refresh = false;
	}

	// ############### ENCODER ###############
	//
	auto u = myEncoder.update();
	if (u.active())
	{
		auto amt = u.accel(1);		   // where 5 is the acceleration factor if you want it, 0 if you don't)
		omxScreensaver.resetCounter(); // screenSaverCounter = 0;
									   //    	Serial.println(u.dir() < 0 ? "ccw " : "cw ");
									   //    	Serial.println(amt);

		// Change Mode
		if (encoderConfig.enc_edit)
		{
			// set mode
			//			int modesize = NUM_OMX_MODES;
			sysSettings.newmode = (OMXMode)constrain(sysSettings.newmode + amt, 0, NUM_OMX_MODES - 1);
			omxDisp.dispMode();
			omxDisp.bumpDisplayTimer();
			omxDisp.setDirty();
		}
		else
		{
			activeOmxMode->onEncoderChanged(u);
		}
	}
	// END ENCODER

	// ############### ENCODER BUTTON ###############
	//
	auto s = encButton.update();
	switch (s)
	{
	// SHORT PRESS
	case Button::Down:				   // Serial.println("Button down");
		omxScreensaver.resetCounter(); // screenSaverCounter = 0;

		// what page are we on?
		if (sysSettings.newmode != sysSettings.omxMode && encoderConfig.enc_edit)
		{
			changeOmxMode(sysSettings.newmode);
			omxModeChangedThisFrame = true;
			seqStop();
			omxLeds.setAllLEDS(0, 0, 0);
			encoderConfig.enc_edit = false;
			omxDisp.dispMode();
		}
		else if (encoderConfig.enc_edit)
		{
			encoderConfig.enc_edit = false;
		}

		// Prevents toggling encoder select when entering mode
		if (!omxModeChangedThisFrame)
		{
			activeOmxMode->onEncoderButtonDown();
		}

		omxDisp.setDirty();
		break;

	// LONG PRESS
	case Button::DownLong: // Serial.println("Button downlong");
		if (activeOmxMode->shouldBlockEncEdit())
		{
			activeOmxMode->onEncoderButtonDown();
		}
		else
		{
			encoderConfig.enc_edit = true;
			sysSettings.newmode = sysSettings.omxMode;
			omxDisp.dispMode();
		}

		omxDisp.setDirty();
		break;
	case Button::Up: // Serial.println("Button up");
		activeOmxMode->onEncoderButtonUp();
		break;
	case Button::UpLong: // Serial.println("Button uplong");
		activeOmxMode->onEncoderButtonUpLong();
		break;
	default:
		break;
	}
	// END ENCODER BUTTON

	// ############### KEY HANDLING ###############
	//
	while (keypad.available())
	{
		auto e = keypad.next();
		int thisKey = e.key();
		bool keyConsumed = false;
		// int keyPos = thisKey - 11;
		// int seqKey = keyPos + (sequencer.patternPage[sequencer.playingPattern] * NUM_STEPKEYS);

		if (e.down())
		{
			omxScreensaver.resetCounter(); // screenSaverCounter = 0;
			midiSettings.keyState[thisKey] = true;
		}

		if (e.down() && thisKey == 0 && encoderConfig.enc_edit)
		{
			// temp - save whenever the 0 key is pressed in encoder edit mode
			saveToStorage();
			//	Serial.println("EEPROM saved");
			omxDisp.displayMessage("Saved State");
			encoderConfig.enc_edit = false;
			omxLeds.setAllLEDS(0,0,0);
			activeOmxMode->onModeActivated();
			omxDisp.isDirty();
			omxLeds.isDirty();
			keyConsumed = true;
		}

		if (!keyConsumed)
		{
			activeOmxMode->onKeyUpdate(e);
		}

		// END MODE SWITCH

		if (!e.down())
		{
			midiSettings.keyState[thisKey] = false;
		}

		// ### LONG KEY SWITCH PRESS
		if (e.held() && !keyConsumed)
		{
			// DO LONG PRESS THINGS
			activeOmxMode->onKeyHeldUpdate(e); // Only the sequencer uses this, could probably be handled in onKeyUpdate() but keyStates are modified before this stuff happens.
		}									   // END IF HELD

	} // END KEYS WHILE

	if (!sysSettings.screenSaverMode)
	{
		omxDisp.UpdateMessageTextTimer();
		activeOmxMode->onDisplayUpdate();
	}
	else
	{ // if screenSaverMode
		omxScreensaver.onDisplayUpdate();
	}

	// DISPLAY at end of loop
	omxDisp.showDisplay();

	omxLeds.showLeds();

	while (MM::usbMidiRead())
	{
		// incoming messages - see handlers
	}
	while (MM::midiRead())
	{
		// ignore incoming messages
	}

	// Micros elapsed = micros() - timeStart;
	// if ((timeStart - reporttime) > 2000)
	// {
	// 	report_profile_time("Elapsed", elapsed);
	// 	reporttime = timeStart;
	// 	// report_ram();
	// };

	

// #if RAM_MONITOR
// 	uint32_t time = millis();

// 	if ((time - reporttime) > 2000)
// 	{
// 		reporttime = time;
// 		report_ram();
// 	};

// 	ram.run();
// #endif

} // ######## END MAIN LOOP ########

// ####### POTENTIOMETERS #######
void readPotentimeters()
{
	for (int k = 0; k < potCount; k++)
	{
		int prevValue = potSettings.analogValues[k];
		int prevAnalog = potSettings.analog[k]->getValue();
		temp = analogRead(analogPins[k]);
		potSettings.analog[k]->update(temp);

		// read from the smoother, constrain (to account for tolerances), and map it
		temp = potSettings.analog[k]->getValue();
		temp = constrain(temp, potMin, potMax);
		temp = map(temp, potMin, potMax, 0, 16383);

		// map and update the value
		potSettings.analogValues[k] = temp >> 7;

		int newAnalog = potSettings.analog[k]->getValue();

		int analogDelta = abs(newAnalog - prevAnalog);

		// if (k == 1)
		// {
		// 	Serial.print(analogPins[k]);
		// 	Serial.print(" ");
		// 	Serial.print(temp);
		// 	Serial.print(" ");
		// 	Serial.print(potSettings.analogValues[k]);
		// 	Serial.print("\n");
		// }

		if (potSettings.analog[k]->hasChanged())
		{
			// do stuff
			if (sysSettings.screenSaverMode)
			{
				omxScreensaver.onPotChanged(k, prevValue, potSettings.analogValues[k], analogDelta);
			}
			// don't send pots in screensaver
			else
			{ 
				activeOmxMode->onPotChanged(k, prevValue, potSettings.analogValues[k], analogDelta);
			}
		}
	}
}

// ####### END POTENTIOMETERS #######

void handleNoteOn(byte channel, byte note, byte velocity)
{
	if (midiSettings.midiSoftThru)
	{
		MM::sendNoteOnHW(note, velocity, channel);
	}
	if (midiSettings.midiInToCV)
	{
		omxUtil.cvNoteOn(note);
	}

	activeOmxMode->inMidiNoteOn(channel, note, velocity);
}

void handleNoteOff(byte channel, byte note, byte velocity)
{
	if (midiSettings.midiSoftThru)
	{
		MM::sendNoteOffHW(note, velocity, channel);
	}

	if (midiSettings.midiInToCV)
	{
		omxUtil.cvNoteOff();
	}

	activeOmxMode->inMidiNoteOff(channel, note, velocity);
}

void handleControlChange(byte channel, byte control, byte value)
{
	if (midiSettings.midiSoftThru)
	{
		MM::sendControlChangeHW(control, value, channel);
	}

	activeOmxMode->inMidiControlChange(channel, control, value);
}

// #### Inbound MIDI callbacks
void OnNoteOn(byte channel, byte note, byte velocity)
{
	handleNoteOn(channel, note, velocity);
	
}
void OnNoteOff(byte channel, byte note, byte velocity)
{
	handleNoteOff(channel, note, velocity);
}
void OnControlChange(byte channel, byte control, byte value)
{
	handleControlChange(channel, control, value);
}

void OnSysEx(const uint8_t *data, uint16_t length, bool complete)
{
	sysEx->processIncomingSysex(data, length);
}

void saveHeader()
{
	// 1 byte for EEPROM version
	storage->write(EEPROM_HEADER_ADDRESS + 0, EEPROM_VERSION);

	// 1 byte for mode
	storage->write(EEPROM_HEADER_ADDRESS + 1, (uint8_t)sysSettings.omxMode);

	// 1 byte for the active pattern
	storage->write(EEPROM_HEADER_ADDRESS + 2, (uint8_t)sequencer.playingPattern);

	// 1 byte for Midi channel
	uint8_t unMidiChannel = (uint8_t)(sysSettings.midiChannel - 1);
	storage->write(EEPROM_HEADER_ADDRESS + 3, unMidiChannel);

	for (int b = 0; b < NUM_CC_BANKS; b++)
	{
		for (int i = 0; i < NUM_CC_POTS; i++)
		{
			storage->write(EEPROM_HEADER_ADDRESS + 4 + i + (5 * b), pots[b][i]);
		}
	}
	// Last is 28

	uint8_t midiMacroChan = (uint8_t)(midiMacroConfig.midiMacroChan - 1);
	storage->write(EEPROM_HEADER_ADDRESS + 29, midiMacroChan);

	uint8_t midiMacroId = (uint8_t)midiMacroConfig.midiMacro;
	storage->write(EEPROM_HEADER_ADDRESS + 30, midiMacroId);

	uint8_t scaleRoot = (uint8_t)scaleConfig.scaleRoot;
	storage->write(EEPROM_HEADER_ADDRESS + 31, scaleRoot);

	uint8_t scalePattern = (uint8_t)scaleConfig.scalePattern;
	storage->write(EEPROM_HEADER_ADDRESS + 32, scalePattern);

	uint8_t lockScale = (uint8_t)scaleConfig.lockScale;
	storage->write(EEPROM_HEADER_ADDRESS + 33, lockScale);

	uint8_t scaleGrp16 = (uint8_t)scaleConfig.group16 ;
	storage->write(EEPROM_HEADER_ADDRESS + 34, scaleGrp16);

	// 35 bytes
}

// returns true if the header contained initialized data
// false means we shouldn't attempt to load any further information
bool loadHeader(void)
{
	uint8_t version = storage->read(EEPROM_HEADER_ADDRESS + 0);

		char buf[64];
		snprintf( buf, sizeof(buf), "EEPROM Header Version is %d\n", version );
		Serial.print( buf );

	// Uninitalized EEPROM memory is filled with 0xFF
	if (version == 0xFF)
	{
		// EEPROM was uninitialized
				Serial.println( "version was 0xFF" );
		return false;
	}

	if (version != EEPROM_VERSION)
	{
		// write an adapter if we ever need to increment the EEPROM version and also save the existing patterns
		// for now, return false will essentially reset the state
				Serial.println( "version not matched" );
		return false;
	}

	sysSettings.omxMode = (OMXMode)storage->read(EEPROM_HEADER_ADDRESS + 1);

	sequencer.playingPattern = storage->read(EEPROM_HEADER_ADDRESS + 2);
	sysSettings.playingPattern = sequencer.playingPattern;

	uint8_t unMidiChannel = storage->read(EEPROM_HEADER_ADDRESS + 3);
	sysSettings.midiChannel = unMidiChannel + 1;

	Serial.println( "loading banks" );
	for (int b = 0; b < NUM_CC_BANKS; b++)
	{
		for (int i = 0; i < NUM_CC_POTS; i++)
		{
			pots[b][i] = storage->read(EEPROM_HEADER_ADDRESS + 4 + i + (5 * b));
		}
	}

	uint8_t midiMacroChannel = storage->read(EEPROM_HEADER_ADDRESS + 29);
	midiMacroConfig.midiMacroChan = midiMacroChannel + 1;

	uint8_t midiMacro = storage->read(EEPROM_HEADER_ADDRESS + 30);
	midiMacroConfig.midiMacro = midiMacro;

	uint8_t scaleRoot = storage->read(EEPROM_HEADER_ADDRESS + 31);
	scaleConfig.scaleRoot = scaleRoot;

	int8_t scalePattern = (int8_t)storage->read(EEPROM_HEADER_ADDRESS + 32);
	scaleConfig.scalePattern = scalePattern;

	bool lockScale = (bool)storage->read(EEPROM_HEADER_ADDRESS + 33);
	scaleConfig.lockScale = lockScale;

	bool scaleGrp16 = (bool)storage->read(EEPROM_HEADER_ADDRESS + 34);
	scaleConfig.group16 = scaleGrp16;

	globalScale.calculateScale(scaleConfig.scaleRoot, scaleConfig.scalePattern);

	return true;
}

void savePatterns(void)
{
	bool isEeprom = storage->isEeprom();

	int patternSize = serializedPatternSize(isEeprom);
	int nLocalAddress = EEPROM_PATTERN_ADDRESS;

	// Serial.println((String)"Seq patternSize: " + patternSize);
	int seqPatternNum = isEeprom ? NUM_SEQ_PATTERNS_EEPROM : NUM_SEQ_PATTERNS;

	for (int i = 0; i < seqPatternNum; i++)
	{
		auto pattern = (byte *)sequencer.getPattern(i);
		for (int j = 0; j < patternSize; j++)
		{
			storage->write(nLocalAddress + j, *pattern++);
		}

		nLocalAddress += patternSize;
	}

	if(isEeprom)
	{
		return;
	}

	Serial.println((String)"nLocalAddress: " + nLocalAddress);

	// Grids patterns
	patternSize = OmxModeGrids::serializedPatternSize(storage->isEeprom());
	int numPatterns = OmxModeGrids::getNumPatterns();

	// Serial.println((String)"OmxModeGrids patternSize: " + patternSize);
	// Serial.println((String)"numPatterns: " + numPatterns);

	for (int i = 0; i < numPatterns; i++)
	{
		auto pattern = (byte *)omxModeGrids.getPattern(i);
		for (int j = 0; j < patternSize; j++)
		{
			storage->write(nLocalAddress + j, *pattern++);
		}

		nLocalAddress += patternSize;
	}

	Serial.println((String)"nLocalAddress: " + nLocalAddress); // 5968

	Serial.println("Saving Euclidean");
	nLocalAddress = omxModeEuclid.saveToDisk(nLocalAddress, storage);
	Serial.println((String)"nLocalAddress: " + nLocalAddress); // 6321
	Serial.println("Saving Chords");
	nLocalAddress = omxModeChords.saveToDisk(nLocalAddress, storage);
	Serial.println((String)"nLocalAddress: " + nLocalAddress); // 6321

	Serial.println("Saving MidiFX");
	
	for(uint8_t i = 0; i < NUM_MIDIFX_GROUPS; i++)
	{
		nLocalAddress = subModeMidiFx[i].saveToDisk(nLocalAddress, storage);
		// Serial.println((String)"Saved: " + i);
		// Serial.println((String)"nLocalAddress: " + nLocalAddress);
	}

	Serial.println((String)"nLocalAddress: " + nLocalAddress); // 6321

	// Seq patternSize: 715
	// nLocalAddress: 5752
	// size of patterns: 5720
	// OmxModeGrids patternSize: 23
	// numPatterns: 8
	// nLocalAddress: 5936
	// size of grids: 184

}

void loadPatterns(void)
{
	bool isEeprom = storage->isEeprom();

	int patternSize = serializedPatternSize(isEeprom);
	int nLocalAddress = EEPROM_PATTERN_ADDRESS;

	Serial.println( "seq patterns nLocalAddress" );
	Serial.println( nLocalAddress );

	int seqPatternNum = isEeprom ? NUM_SEQ_PATTERNS_EEPROM : NUM_SEQ_PATTERNS;

	for (int i = 0; i < seqPatternNum; i++)
	{
		auto pattern = Pattern{};
		auto current = (byte *)&pattern;
		for (int j = 0; j < patternSize; j++)
		{
			*current = storage->read(nLocalAddress + j);
			current++;
		}
		sequencer.patterns[i] = pattern;

		nLocalAddress += patternSize;
	}

	if(isEeprom)
	{
		return;
	}

	Serial.println( "grids patterns nLocalAddress" );
	Serial.println( nLocalAddress );
	// 332 - eeprom size
	// 332 * 8 = 2656

	// Grids patterns
	patternSize = OmxModeGrids::serializedPatternSize(storage->isEeprom());
	int numPatterns = OmxModeGrids::getNumPatterns();

	for (int i = 0; i < numPatterns; i++)
	{
		auto pattern = grids::SnapShotSettings{};
		auto current = (byte *)&pattern;
		for (int j = 0; j < patternSize; j++)
		{
			*current = storage->read(nLocalAddress + j);
			current++;
		}
		omxModeGrids.setPattern(i, pattern);
		nLocalAddress += patternSize;
	}

	Serial.println( "Pattern size" );
	Serial.println( patternSize );
	Serial.println( "nLocalAddress" );
	Serial.println( nLocalAddress );

	Serial.println("Loading Euclidean");
	nLocalAddress = omxModeEuclid.loadFromDisk(nLocalAddress, storage);
	Serial.println((String)"nLocalAddress: " + nLocalAddress); // 5988
	Serial.println("Loading Chords");
	nLocalAddress = omxModeChords.loadFromDisk(nLocalAddress, storage);
	Serial.println((String)"nLocalAddress: " + nLocalAddress); // 5988

	// Serial.println((String)"nLocalAddress: " + nLocalAddress); // 5968

	Serial.println("Loading MidiFX");
	
	for(uint8_t i = 0; i < NUM_MIDIFX_GROUPS; i++)
	{
		nLocalAddress = subModeMidiFx[i].loadFromDisk(nLocalAddress, storage);
		// Serial.println((String)"Loaded: " + i);
		// Serial.println((String)"nLocalAddress: " + nLocalAddress);
	}

	Serial.println((String)"nLocalAddress: " + nLocalAddress); // 5988


	// Pattern size = 715
	// Pattern size eprom = 332
	// Total size of patterns = 5720
	// Total storage size = 5749
	// Fram = 32000 = 26251 available
	// Eeprom = 2048
	// Eeprom rom can save 6 patterns, plus 56 bytes

	// 2832 - size of 16 euclid patterns of 16 euclids
}

// currently saves everything ( mode + patterns )
void saveToStorage(void)
{
	// Serial.println( "saving..." );
	saveHeader();
	savePatterns();
}

// currently loads everything ( mode + patterns )
bool loadFromStorage(void)
{
	// This load can happen soon after Serial.begin - enable this 'wait for Serial' if you need to Serial.print during loading
	// while( !Serial );

	Serial.println( "read the header" );
	bool bContainedData = loadHeader();

	if (bContainedData)
	{
		Serial.println( "loading patterns" );
		loadPatterns();
		changeOmxMode(sysSettings.omxMode);

		omxDisp.isDirty();
		omxLeds.isDirty();
		return true;
	}

	Serial.println( "failed to load" );

	omxDisp.isDirty();
	omxLeds.isDirty();

	return false;
}
