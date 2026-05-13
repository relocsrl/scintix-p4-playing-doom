# DOOM for ESP32-P4 + ESP32-C6 Guilion JC4880P443 Screen

## Working port of DOOM with PS4 controller and Audio!
[>>>  YOUTUBE <<<](https://www.youtube.com/watch?v=7lf0nKiRfZQ)  


## Hardware  
- JC4880P443 Display  
- 8Ohm 2W speaker  
- PS4 controller (wired only, controller bluetooth sees not compatible with C6)
- P4 chip is very powerful, gameplay is very smooth

## Software  
- Web server available to change color and volume settings  
- Wifi works in AP mode + captive portal  
- WAD file is forked from https://github.com/Akbar30Bill/DOOM_wads  
- compiled in VSC + IDF v5.5.0  
- Code takes about 12Mb of space and it is all loaded to SPIFFS, so flashing takes some time

## Power up  
- First you will hear beep and see pattern to confirm speaker and screen are operational  
- Game will load in about minute after so be patient
- If colors are swapped or inverted, adjust it in web server  
  
## wifi portal  
- wifi advertises as JC4880P443_xxxxxx with no password (xxxxxx is last 6 characters of device MAC)
- Connect and enter your own SSID and Password  
- Use serial monitor or network scanner to get your IP address  
  
  
<p align="center">
If you like this project ---->-<a href="https://www.buymeacoffee.com/mazur888" target="_blank"><img src="https://cdn.buymeacoffee.com/buttons/default-orange.png" alt="Buy Me A Coffee" height="35" width="auto"></a>
</p>
  
## Game loaded:  
<img width="3072" height="3350" alt="doom" src="https://github.com/user-attachments/assets/9ae8dea2-f8eb-4172-adaa-764856c08f69" />
  
## PCB:  
<img width="3072" height="4080" alt="guilion" src="https://github.com/user-attachments/assets/166c3e6f-90d6-4aa8-9752-b9ea89f1b835" />
  
## Webserver settings page:  
  
<img width="742" height="810" alt="Screenshot From 2026-05-13 09-07-36" src="https://github.com/user-attachments/assets/50d1c964-82e7-4bca-8fe7-3f806de12ebc" />

## Compilation  
**Make sure esp32p4 is selected in iDF**  
  

## License
This project is released under the [GNU GPL v3.0](LICENSE).  
© 2026 Mazur888. No warranty; use at your own risk.  
  
  
  
  


