/*
 *  timelapseplus.cpp
 *  Timelapse+
 *
 *  Created by Elijah Parker
 *  Copyright 2012 Timelapse+
 *  Licensed under GPLv3
 *
 */

#include <avr/pgmspace.h>
#include <avr/sleep.h>
#include <avr/io.h>
#include <avr/eeprom.h>
#include <avr/wdt.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <string.h>
#include <LUFA/Drivers/Peripheral/Serial.h>
#include "tldefs.h"
#include "5110LCD.h"
#include "AVRS_logo.h"
#include "clock.h"
#include "button.h"
#include "Menu.h"
#include "hardware.h"
#include "shutter.h"
#include "IR.h"
#include "timelapseplus.h"
#include "VirtualSerial.h"
#include "TWI_Master.h"
#include "LCD_Term.h"
#include "debug.h"
#include "bluetooth.h"
#include "settings.h"
#include "PTP_Driver.h"
#include "math.h"
#include "selftest.h"
#include "remote.h"
#include "tlp_menu_functions.h"
#include "notify.h"
#include "PTP.h"

char system_tested EEMEM;

extern volatile uint8_t showGap;
extern volatile uint8_t timerNotRunning;
extern volatile uint8_t modeHDR;
extern volatile uint8_t modeTimelapse;
extern volatile uint8_t modeStandard;
extern volatile uint8_t modeRamp;
extern volatile uint8_t modeNoRamp;
extern volatile uint8_t modeRampKeyAdd;
extern volatile uint8_t modeRampKeyDel;
extern volatile uint8_t modeBulb;
extern volatile uint8_t bulb1;
extern volatile uint8_t bulb2;
extern volatile uint8_t bulb3;
extern volatile uint8_t bulb4;
extern volatile uint8_t showRemoteStart;
extern volatile uint8_t showRemoteInfo;
extern volatile uint8_t brampKeyframe;
extern volatile uint8_t brampGuided;
extern volatile uint8_t brampAuto;

volatile uint8_t connectUSBcamera = 0;

uint8_t battery_percent, charge_status, USBmode = 0;

extern settings conf;

extern uint8_t Camera_Connected;

shutter timer = shutter();
LCD lcd = LCD();
MENU menu = MENU();
Clock clock = Clock();
Button button = Button();
BT bt = BT();
IR ir = IR();
Remote remote = Remote();
Notify notify = Notify();
PTP camera = PTP();

#include "Menu_Map.h"


/******************************************************************
 *
 *   setup
 *
 *
 ******************************************************************/

void setup()
{
	wdt_reset();
	wdt_disable();
	wdt_enable(WDTO_2S);

	setIn(BUTTON_FL_PIN);
	setHigh(BUTTON_FL_PIN);

#ifdef PRODUCTION
	if(battery_status() == 0 && getPin(BUTTON_FL_PIN)) // Looks like it's connected to the programmer
	{
		lcd.init();
		termInit();
		termPrintStr(TEXT("\n  Programming\n  Successful\n\n  Move to test\n  station\n"));
		wdt_disable();
		hardware_off();
	}
#endif

	hardware_init();
	settings_init();

	clock.init();

	lcd.init();

	menu.lcd = &lcd;
	menu.button = &button;

	set_sleep_mode(SLEEP_MODE_PWR_DOWN);
	sleep_enable();

	battery_percent = battery_read();

	menu.init((menu_item*)menu_main);

	lcd.update();

	VirtualSerial_Init();

	// Configure I2C //
	TWI_Master_Initialise();

	bt.init();
	if(!bt.present)
	{
		_delay_ms(100);
		bt.init(); // give it one more try
	}
	if(conf.btMode == BT_MODE_SLEEP) bt.sleep(); else bt.advertise();

#ifdef PRODUCTION
	// Check to see if system has passed self-test
	if(eeprom_read_byte((const uint8_t*)&system_tested) == 0xFF || eeprom_read_byte((const uint8_t*)&system_tested) == 0x00 || button.get() == RIGHT_KEY)
	{
		if(test()) // Run self-test (requires connection to test station)
		{
			// Test passed
			eeprom_write_byte((uint8_t*)&system_tested, 0x01);
		}
		// Reboot
		wdt_enable(WDTO_8S);
		for(;;);
	}
#endif
}

/******************************************************************
 *
 *   main
 *
 *
 ******************************************************************/

int main()
{
	/****************************
	   Initialization
	*****************************/

	setup();

    if(conf.firmwareVersion != VERSION)
    {
    	if(conf.firmwareVersion <= 20120212)
    	{
    		conf.interface = INTERFACE_AUTO;
		    conf.brampMode = BRAMP_MODE_ALL;
    	}
        conf.firmwareVersion = VERSION;
        settings_save();
        menu.spawn((void*)firmwareUpdated);
    }
    else
    {
		if(battery_status() > 0)
			hardware_off(); // If it was just plugged in, show the charging screen

    }

	uint16_t count = 0;

	notify.watch(NOTIFY_CHARGE, (void *)&charge_status, sizeof(charge_status), &message_notify);
	notify.watch(NOTIFY_BT, (void *)&remote.connected, sizeof(remote.connected), &message_notify);
	notify.watch(NOTIFY_CAMERA, (void *)&PTP_Ready, sizeof(PTP_Ready), &message_notify);

	/****************************
	   Main Loop
	*****************************/

	for(;;)
	{
		wdt_reset();

		if(VirtualSerial_CharWaiting()) // Process USB Commands from PC (needs to be moved to sub-module)
		{
			char c = VirtualSerial_GetChar();

			switch(c)
			{
			   case '$':
				   hardware_bootloader();
				   break;

			   case 'V': // prints version backward
				   {
					   uint32_t version = VERSION;
					   char c;
					   while(version)
					   {
						   c = (char)(version % 10);
						   debug((char)(c + '0'));
						   version -= (uint32_t)c;
						   version /= 10;
					   }
					   debug_nl();
					   break;
				   }

			   case 'v': // binary version (4 bytes)
				   {
					   uint32_t v = VERSION;
					   char *ptr;
					   ptr = (char*) &v;
					   debug(*ptr);
					   debug(*(ptr+1));
					   debug(*(ptr+2));
					   debug(*(ptr+3));
					   break;
				   }

			   case 'T':
				   debug('E');
				   break;

			   case 'C': // Capture
				   {
			   	       debug(STR("Taking picture...\r\n"));
			   	       timer.capture();
					   break;
				   }

			   case 'M':
			   	    debug(STR("REMOTE_MODEL: "));
				    debug(remote.model);
				    debug_nl();
			   	    debug(STR("REMOTE_CONNECTED: "));
				    debug(remote.connected);
				    debug_nl();
				    break;

			   case 'B':
				   bt.init();
				   break;

			   case 'I':
			   	   hardware_light_start();
			   	   hardware_light_set_range(0);
			   	   debug(STR("Light Sensor INIT\r\n"));
				   break;

			   case 'L':
			   	   debug(STR("Raw: "));
			   	   debug(hardware_light_read_raw());
				   debug_nl();
			   	   debug(STR("Lux: "));
			   	   debug(hardware_light_read_lux());
				   debug_nl();
			   	   debug(STR("Ev: "));
			   	   debug(hardware_light_read_ev());
				   debug_nl();
				   break;

			   case 'S': // Screen dump
				   for(uint8_t j = 0; j < (LCD_HEIGHT >> 3); j++)
				   {
				       for(uint8_t i = 0; i < LCD_WIDTH; i++)
				       {
				           char b = lcd.screen[i][j];
				           VirtualSerial_PutChar(b);
				       }
				    }
			}
		}


		/****************************
		   Tasks
		*****************************/

		updateConditions();
		menu.task();
		timer.task();
		clock.task();
		bt.task();
		notify.task();

		if(USBmode == 1)
			PTP_Task();
		else
			VirtualSerial_Task();


		/****************************
		   Events / Notifications
		*****************************/
		
		count++;
		if(count > 9)
		{
			count = 0;
			charge_status = battery_status();
			camera.checkEvent();
		}

		if(bt.event) remote.event();

		if(menu.unusedKey == FR_KEY)
			hardware_flashlight_toggle();


		static uint32_t startTime = 0;

		if(clock.Ms() > startTime + 60000)
		{
			startTime = clock.Ms();
			battery_percent = battery_read();
		}

		if((hardware_USB_HostConnected || connectUSBcamera) && (USBmode == 0))
		{
			USBmode = 1;
			USB_Detach();
			USB_Disable();
			hardware_USB_SetHostMode();
			PTP_Enable();
		}
		else if((!hardware_USB_HostConnected && !connectUSBcamera) && (USBmode == 1))
		{
			USBmode = 0;
			PTP_Disable();
			hardware_USB_SetDeviceMode();
			VirtualSerial_Init();
		}

	}
}

void message_notify(uint8_t id)
{
	switch(id)
	{
		case NOTIFY_CHARGE:
			switch(charge_status)
			{
				case 0:
					menu.message(STR("Unplugged"));
					break;
				case 1:
					menu.message(STR("Charging"));
					break;
				case 2:
					menu.message(STR("Charged"));
					break;
			}
			break;
			
		case NOTIFY_BT:
			if(remote.connected)
			{
				menu.message(STR("BT Connected"));
			}
			else
			{
				menu.message(STR("Disconnected"));
			}
			break;

		case NOTIFY_CAMERA:
			if(PTP_Ready)
			{
				menu.message(STR("USB Camera"));
				menu.refresh();
//				menu.message(PTP_CameraModel);
				camera.init();
			}
			else
			{
				if(PTP_Error)
				{
					timerStop(0, 1);
					menu.spawn((void*)usbPlug);
				}
				else
				{
					menu.message(STR("Disconnected"));
				}
				camera.close();
				menu.refresh();
			}
			break;
	}
}

/******************************************************************
 *
 *   ISR
 * 
 *   Timer2 interrupt routine - called every millisecond
 *   Configured in hardware.cpp hardware_init()
 *
 ******************************************************************/

ISR(TIMER2_COMPA_vect)
{
	clock.count();
	button.poll();
}

/******************************************************************
 *
 *   ISR
 * 
 *   INT6 external interrupt routine - lightening trigger
 *   Configured in hardware.cpp hardware_lightening_enable()
 *
 ******************************************************************/

ISR(INT6_vect) // Lightening Trigger
{ 
	shutter_capture();
} 
