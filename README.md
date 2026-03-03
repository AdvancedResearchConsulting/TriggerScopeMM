# TriggerScope-MiniMM
This is a branch dedicated to the triggerscope-Mini. 
Find out more about the TG Mini here -> https://advancedresearch-consulting.com/product/triggerscope-mini/
To compile this firmware, please use the following steps. 

Drivers - If you don't see a COM port when the device is connected, and you have an older win10 or 7 install,
please use the driver included in this repo, as a zip in the root. (stsw*) This zip contains the needed serial installer for x64 and x32 older versions of windows with serial enumeration. 


ARDUINO SETUP:
1. Must have STM32CubeProgrammer installed
2. DL this library by adding to board manager:
	- https://github.com/stm32duino/BoardManagerFiles/raw/main/package_stmicroelectronics_index.json
3. Add STM32 to board types
4. set STM32 board, then set
	- Generic STM32F0 Series
	- tools - Board Part Number = F072CBT×
	- tools - Upload Method = STM32CubeProgrammer (DFU) 
	- tools - USB Support (if Available) "CDC (Generic Serial Supersede UART)"
	- tools - USART Support: "Enabled (generic serial)"

5. the above will at least configure the right settings. then set code with serial.begin and compile.

6. if the system won't upload on its own then:
	- use Sketch-> Export Compiled Binary
	- Sketch -> Show Sketch Folder
	- in STM Programmer app, load hex file, upload there. 


***Note, for most people who ened to compile, please contact ARC at:
suuport "at" advancedresearch-consulting.com


*The mini test.py file can be used to quickly validate proper performance. 



-Austin
