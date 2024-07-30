# TIEM SYNC

this repo is for LiDAR, FA camera, IMU time synchrononization

current implementation is Livox AVIA + STM32F407IG(RoboMaster Type C board) + HkRobot MV-CA016-A0UC

The code will be refined soon!!!

# build

build livox first, go to `liv_syn/src/livox_ros_driver/livox_sdk`

```
mkdir build && cd build
cmake .. && cmake --build .
```

then go back to workspace root, execute `catkin_make`.

This repo is built based on ref from https://github.com/sheng00125/LIV_handhold.

