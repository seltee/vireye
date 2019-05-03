#pragma once
#include<stm32f10x.h>

#define _bitset(bits)\
  ((unsigned int)(\
  (bits%010)|\
  (bits/010%010)<<1|\
  (bits/0100%010)<<2|\
  (bits/01000%010)<<3|\
  (bits/010000%010)<<4|\
  (bits/0100000%010)<<5|\
  (bits/01000000%010)<<6|\
  (bits/010000000%010)<<7|\
  (bits/0100000000%010)<<8|\
  (bits/01000000000%010)<<9|\
  (bits/010000000000%010)<<10|\
  (bits/0100000000000%010)<<11|\
  (bits/01000000000000%010)<<12|\
  (bits/010000000000000%010)<<13|\
  (bits/0100000000000000%010)<<14|\
  (bits/01000000000000000%010)<<15))
#define BINARY( bits ) _bitset(0##bits)

void delayByLoop(uint32_t nCount);
void reverse(char s[]);
void itoa(int n, char s[]);
char toUpper(char a);
bool cmp(char *a, char *b);
bool cmpi(char *a, char *b);
unsigned int __umodsi3(unsigned int a, unsigned int b);
int __modsi3(int a, int b);
unsigned int __udivsi3(unsigned int n, unsigned int d);
int __divsi3(int n, int d);



unsigned int rand();
