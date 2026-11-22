#pragma once
#include "Arduino.h"
struct FakeEEPROM {
  uint8_t mem[4096]; long writes=0; long failAfter=-1; // failAfter: simulate power loss after N byte writes
  FakeEEPROM(){ memset(mem,0xFF,sizeof mem); }
  uint8_t read(int a){ return mem[a]; }
  void update(int a, uint8_t v){ if(mem[a]!=v){ if(failAfter>=0 && writes>=failAfter) throw 1; mem[a]=v; writes++; } }
  template<class T> void get(int a, T& t){ memcpy(&t, mem+a, sizeof(T)); }
  template<class T> void put(int a, const T& t){ const uint8_t* p=(const uint8_t*)&t; for(size_t i=0;i<sizeof(T);i++) update(a+i,p[i]); }
};
extern FakeEEPROM EEPROM;
