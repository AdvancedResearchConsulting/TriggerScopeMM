# TriggerScopeMM
V4 Firmware for Triggerscope using Micromanager Driver Developed by Nico Stuurman

*This branch is custom. It's based on vurrent 620. The only change is that using PDD you can now set delays for transitions on digital pins. 
*to enable this delay use transition state change == 2 for pin group to control. 
and example of this would be the following commands to generate a 10ms square wave at 50% duty cycle. 

'PDC0',         # clear prior settings

'PDO0-1-1',     # assign output bank 0 (pins 0-8) a value of 1, meaning pin 1.

'PDD-10000',    # use a delay of 10mS

'PDS0-1-2'      # begin transitions

-Austin
