Overdrive modifications per development w/ S. Mehta & I. Ivanov. 

INSTALL:
- Drop compiled driver into x64 OS enabled micromanager 2.0 Gamma install directory. 
- Install firmware appropriate for your triggerscope via the serial # information below. 
- Add updated driver and verify new functions are accessable via device/property browser. 

Modifications to original firmware for this version include:
 - POV command. "Program Over Voltage". 
	This command outputs a higher than default command for a brief period of time.
	The time this value will be output is specified by "POD"
 - POD Command. "Program OverVoltage Delay".
	This command specifies the duration time the over voltage value specified by POV will be held, in us. 
	After this time expires, the voltage on the specified pin will set to the state specified in the normal PAS list. 

 - See command info using Triggerscope "?\n" command for details on use. 

*Be careful which firmware is used*
Serial #'s are matched to firmware as specified below. 

SN# 1932-1970 should use Nico's Direct branch here -> https://github.com/micro-manager/TriggerScopeMM/tree/main/src/TriggerScope_V3
SN# 1971-2117 should use the -V4 folder in this repo.

SN# 2117 + should use the -V4B folder in this repo. 

For more information on serial # or if you have errors please contact ARC at advancedreseach-consulting.com
Special thanks to the Mehta Lab @ Chan Zuckerburg Bio Hub for supporting these extensions! 
