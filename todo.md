I have an M5Stack RoverC Pro robot with an M5StickC Plus controller.
Each downloaded library has a claude.md file with hardware description and API.

## Your tasks:
1. Design subagent chain for this work

2. Find and read ALL claude.md files in the project (check lib/, .pio/libdeps/, 
   and any subdirectories). These contain the exact API for each library.

3. Based on claude.md files, write PlatformIO firmware for M5StickC Plus 
   that controls RoverC Pro.

4. Review subagent chain after your read all the docs.

## Demo sequence (triggered by Button A):
- FORWARD 50% speed → 1 second
- STOP → 0.5 seconds  
- BACKWARD 50% speed → 1 second
- STOP → 0.5 seconds
- GRIPPER OPEN → 0.5 seconds
- GRIPPER CLOSE → 0.5 seconds
- IDLE

## M5StickC Plus screen:
- Display current action name
- Display battery level

## Controls:
- Button A → start sequence
- Button B → emergency stop (motors off immediately)

## Build and flash:
1. `pio run` — check compilation
2. Fix any errors
3. `pio run --target upload` — flash to device
4. `pio device monitor` — show serial output

Use ONLY the APIs described in claude.md files. Do not guess register 
addresses or I2C commands — everything must come from the documentation.