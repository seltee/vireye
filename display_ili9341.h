#pragma once

#include "engine.h"
#include <stm32f10x.h>

#define PIN_LCD_LED GPIO_Pin_1
#define PIN_LCD_DC GPIO_Pin_2
#define PIN_LCD_RESET GPIO_Pin_3
#define PIN_LCD_CS GPIO_Pin_4

class Display_ILI9341 {
public:
	unsigned short getFPS();
	void setFPS(unsigned short limit);
	void init();
	void draw();

private:
	void sendCMD(uint8_t cmd);
	void writeData8(uint8_t data);
	void writeData16(uint16_t data);
	
	void displayClear(uint8_t color);

	void initDMA(uint16_t *cLine);
	void initTimer();
	void initFPSTimer(unsigned short limit);

	void setCol(uint16_t startCol, uint16_t endCol);
	void setPage(uint16_t startPage, uint16_t endPage);
	void setXY(uint16_t positionX, uint16_t positionY);
	void setPixel(uint16_t positionX, uint16_t positionY, uint8_t color);

	unsigned char fpsLimit;
};
