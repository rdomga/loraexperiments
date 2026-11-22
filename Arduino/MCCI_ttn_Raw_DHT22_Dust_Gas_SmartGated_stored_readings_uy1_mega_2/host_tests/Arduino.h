#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define F(x) (x)
#define DEC 10
#define HEX 16
#define highByte(x) ((uint8_t)(((x)>>8)&0xFF))
#define lowByte(x)  ((uint8_t)((x)&0xFF))
struct FakeSerial { template<class T> void print(T v){ if(!quiet) std_print(v);} template<class T> void print(T v, int base){ if(!quiet) { if(base==HEX) printf("%X", v); else std_print(v); }} template<class T> void println(T v){ if(!quiet){std_print(v);puts("");}} void println(){puts("");}
  static void std_print(const char*s){fputs(s,stdout);} static void std_print(int v){printf("%d",v);} static void std_print(unsigned v){printf("%u",v);} static void std_print(long v){printf("%ld",v);} static void std_print(unsigned long v){printf("%lu",v);} static void std_print(unsigned char v){printf("%u",v);} static void std_print(unsigned short v){printf("%u",v);}
  bool quiet=true; };
extern FakeSerial Serial;
extern unsigned long g_fakeNowMs;
static inline unsigned long nowMs(){ return g_fakeNowMs; }
