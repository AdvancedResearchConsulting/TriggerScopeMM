# Waveform modification from normal Firmware. 
Notes on using this!
- sending a "WAV1-32767-32767-180-0" command will do the following:
-- assign a waveform output (sine) to DAC1. 
-- set the midpoint of the output to 1/2 of the output voltage (16 bit)
-- set the amplitude of the output. (16 bit)
-- set the # of updates per cycle, so the bit depth of the waverform in effect. 
-- Last, sets a delay value in us per step. 

The steps are cycles inside of the clock loop, which blocks on execution as long as a ttl input is on/high.
The execution runs continually until the ttl output goes off/low. 

Using the delay value of ""BAD2-390" will delay a waveform output by the # of cteps specified at the last #. 
So in the case of the example, 390 steps will be delayed. 
If the waveform has an execution resolution of 360 steps per output, this results in one waveform + 30°. 
This can be advantageous for use in setting the phase of multiple waveforms. 

**Please note that when the execution runs for waveform output, no serial commands are processed. 

---original readme follows --


# TriggerScopeMM
V4 Firmware for Triggerscope using Micromanager Driver Developed by Nico Stuurman

***Be careful which firmware is used! 
# 1932-1970 should use Nico's Direct branch here -> https://github.com/micro-manager/TriggerScopeMM/tree/main/src/TriggerScope_V3
# 1971-2117 should use the -612 folder in this repo.
# 2117 + should use the -620 folder in this repo. 

Firmware for the Triggerscope 4 & 4-B. 
Please check the official Firmware repo maintained by Nico here for updates - https://github.com/micro-manager/TriggerScopeMM
-Austin
