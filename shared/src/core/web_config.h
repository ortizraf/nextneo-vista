#pragma once
void webConfigBeginMdns();   // before Wi-Fi connects (see web_config.cpp)
void webConfigLoop();        // Arduino loop: start the server once online, serve requests
