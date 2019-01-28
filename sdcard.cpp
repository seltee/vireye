#include<stm32f10x.h>
#include<stm32f10x_rcc.h>
#include<stm32f10x_gpio.h>
#include<stm32f10x_spi.h>
#include<stm32f10x_dma.h>
#include<string.h>

#include "display_ili9341.h"
#include "sdcard.h"
#include "hardware.h"
#include "terminal.h"
#include "helpers.h"

#define GO_IDLE_STATE 0 											//Программная перезагрузка
#define SEND_IF_COND 8 												//Для SDC V2 - проверка диапазона напряжений
#define READ_SINGLE_BLOCK 17 									//Чтение указанного блока данных
#define WRITE_SINGLE_BLOCK 24 								//Запись указанного блока данных
#define SD_SEND_OP_COND 41 										//Начало процесса инициализации
#define APP_CMD 55 														//Главная команда из ACMD команд
#define READ_OCR 58 													//Чтение регистра OCR

extern Display_ILI9341 display;

unsigned char *buffer = 0;
uint8_t  SDHC;  
uint8_t SDError = 0;
unsigned int currentSector = 0xffffffff;

unsigned char sectorsPerCluster;
unsigned char fatCount;
unsigned int fatSize;
unsigned int fatStartAddr;
unsigned int cluster2Addr;
unsigned int rootClusterAddr;
unsigned int rootAddr;

uint8_t spiSend(uint8_t data){ 
  while (!(SPI2->SR & SPI_SR_TXE));
  SPI2->DR = data;
  while (!(SPI2->SR & SPI_SR_RXNE));
  return (SPI2->DR);
}

uint8_t spiRead (void){ 
  return spiSend(0xff);
}

uint8_t SDSendCommand(uint8_t cmd, uint32_t arg)
{
  uint8_t response, tmp;     
	uint16_t wait=0;
 
  // Usual SD has byte addressing, not by sector
  if(SDHC == 0 && (cmd == READ_SINGLE_BLOCK || cmd == WRITE_SINGLE_BLOCK)) arg = arg << 9;

  // Send command and arguments
  spiSend(cmd | 0x40);
  spiSend(arg>>24);
  spiSend(arg>>16);
  spiSend(arg>>8);
  spiSend(arg);
 
  // Send crc
	spiSend(cmd == SEND_IF_COND ? 0x87 : 0x95);   
 
  // waiting for response
  while((response = spiRead()) == 0xff) 
		if(wait++ > 0xfffe) break;
 
  // check OCR from READ_OCR
  if(response == 0x00 && cmd == 58)     
  {
		// Check if SDHC
    tmp = spiRead();   
    if(tmp & 0x40) 
			SDHC = 1;
    else           
			SDHC = 0;
		
    // Another 3 bytes
    spiRead(); 
    spiRead(); 
    spiRead(); 
  }
 
  spiRead();
  return response;
}

uint8_t SDInit(void)
{
  uint32_t i, response, SD_version = 2, retry = 0;        
  for(i=0;i<40;i++) spiRead();      //send > 74 bits  
 
  // Soft CD reset
  GPIOB->BSRR = GPIO_BSRR_BR12;
  while(SDSendCommand(GO_IDLE_STATE, 0) != 0x01)                                   
    if(retry++>0x40) return 1;  
	
  spiRead();
  spiRead();

  retry = 0;                                     
  while(SDSendCommand(SEND_IF_COND, 0x000001AA)!=0x01)
  { 
    if(retry++>0xfe) 
    { 
      SD_version = 1;
      break;
    } 
  }
 
	retry = 0;                                     
	do
	{
		response = SDSendCommand(APP_CMD,0); 
		response = SDSendCommand(SD_SEND_OP_COND,0x40000000);
		retry++;
		if(retry>0xfffe) return 1;                     
	}while(response != 0x00);                      
 
 
	// Detecting memory type
	retry = 0;
	SDHC = 0;
	if (SD_version == 2){ 
		while(SDSendCommand(READ_OCR,0) != 0x00)
		if(retry++>0xffe)  break;
	}
 
	return 0; 
}

bool SDReadSector(unsigned int sector){ 
	int retry = 0, i;
	if (buffer){
		if (sector == currentSector) return true;
		while(retry++<8){
			// Not working without this
			delayByLoop(200000);
			i=0;
		 
			// Read single block command
			if(SDSendCommand(READ_SINGLE_BLOCK, sector)) continue;
			
			// Waiting for data
			while(spiRead() != 0xfe)                
				if(i++ > 0xffffe) continue;
		 
			// Reading 512 bytes
			for(i=0; i<512; i++) 
				buffer[i] = spiRead();
		 
			spiRead(); 
			spiRead(); 
			spiRead(); 

			currentSector = sector;
			return true;
		}
	}
  return false;
}

bool SDWriteSector(unsigned int sector){
	if (buffer){
		int retry = 0, i;
		
		while(retry++<8){
			// Not working without this
			delayByLoop(200000);
			i=0;
			
			// Write single block command
			if(SDSendCommand(WRITE_SINGLE_BLOCK, sector)) continue;
			
			spiRead();
			spiSend(0xfe); // Mark of package sending
				
			for (i=0; i<512; i++) 
				spiSend(buffer[i]);
			
			spiSend(0x00); 
			spiSend(0x00); 
			
			i = 0;
			while(spiRead() != 0x05)
				if(i++ > 0xffffe) continue;  
					
			i = 0;
			while(spiRead() != 0x00) continue;

			return true;
		}
	}
	return false;
}

bool SDEnable(unsigned char *newBuffer){
	SDError = SDInit();
	if (SDError == 0){
		buffer = newBuffer;
		
		if (!SDReadSector(0)) return false;
		if (SDGetWord(0x1fe) != 0xaa55) return false;
		
		// A0 usually
		unsigned short bootSector = SDGetWord(0x1c6);
		if (!SDReadSector(bootSector)) return false;
		
		sectorsPerCluster = SDGetByte(0x0D);
		fatCount = SDGetByte(0x0C);
		fatSize = SDGetDWord(0x24);
		fatStartAddr = SDGetWord(0x0E) + bootSector;
		cluster2Addr = fatStartAddr+(fatSize*fatCount);
		rootClusterAddr = SDGetDWord(0x2c);
		rootAddr = cluster2Addr+((rootClusterAddr-2)*sectorsPerCluster);

		return true;
	}
	return false;
}

void SDDisable(){
	buffer = 0;
}

unsigned char SDGetByte(unsigned int address){
	return buffer[address];
}

unsigned short SDGetWord(unsigned int address){
	return buffer[address] + (buffer[address+1] << 8);
}

unsigned int SDGetDWord(unsigned int address){
	return buffer[address] + (buffer[address+1] << 8) + (buffer[address+2] << 16) + (buffer[address+3] << 24);
}

void SDSetByte(unsigned int address, char byte){
	buffer[address] = byte;
}

void SDSetWord(unsigned int address, short word){
	buffer[address+1] = word >> 8;
	buffer[address] = word; 
}

void SDSetDWord(unsigned int address, int dword){
	buffer[address+3] = dword >> 24;
	buffer[address+2] = dword >> 16; 
	buffer[address+1] = dword >> 8; 
	buffer[address] = dword;
}

bool SDIsHC(){
	return SDHC ? true : false;
}

unsigned char getSDError(){
	return SDError;
}

void FSGetNameFromFileStruct(unsigned char* fStruct, char *fileName){
	unsigned char nameCounter;
	for (int i = 0; i < 8; i++){
		fileName[i] = fStruct[i];
	}
	nameCounter=8;
			
	// Remove spaces
	while(nameCounter > 0){
		nameCounter--;
		if (fileName[nameCounter] != ' ') break;
	}
	nameCounter++;
	
	// Extention
	if (fStruct[8] != ' '){
		fileName[nameCounter] = '.';
		nameCounter++;

		for (int i = 0; i < 3; i++){
			fileName[nameCounter] = fStruct[i+8];
			nameCounter++;
		}
				
		// Remove spaces
		while(nameCounter > 0){
			nameCounter--;
			if (fileName[nameCounter] != ' ') break;
		}
		nameCounter++;
	}

	fileName[nameCounter] = 0;
}

int FSGetCluster(const char *filePath, FileInfo *fileInfo, int *targetSector, int *targetSectorShift){
	char nameBuffer[32];
	char cmpName[14];
	short int searcher = 0, counter;
	unsigned int sectorStart, sectorCounter, inSectorShift;
	int cluster = -1;
	
	if (buffer){
		if (filePath[0] == '/'){
			cluster = rootClusterAddr;
			searcher = 1;
			sectorStart = rootAddr;
		}
		
		while(filePath[searcher]){
			counter = 0;
			while(filePath[searcher] && filePath[searcher] != '/'){ 
				nameBuffer[counter] = filePath[searcher];
				searcher++;
				counter++;
			}
			nameBuffer[counter] = 0;

			// Searching the file
			if (strlen(nameBuffer)){
				sectorCounter = 0;
				inSectorShift = 0;
				
				while(1){
					SDReadSector(sectorStart+sectorCounter);
					
					if (buffer[inSectorShift] != 0xE5 && buffer[inSectorShift] != 0x05 && buffer[inSectorShift] != 0x2E && buffer[inSectorShift+0x0b] != 0x0f){
						// File not found
						// if (buffer[inSectorShift] == 0) return -1; 
						
						// Compare file names and get cluster if match
						FSGetNameFromFileStruct(buffer+inSectorShift, cmpName);
						if (cmpi(cmpName, nameBuffer)){
							cluster = SDGetWord(inSectorShift+0x1A) + (SDGetWord(inSectorShift+0x14) << 16);
							break;
						}
					}
					
					inSectorShift+=32;
					if (inSectorShift >= 512){
						inSectorShift = 0;
						sectorCounter++;
						if (sectorCounter >= sectorsPerCluster){
							sectorCounter = 0;
							cluster = FSGetClusterValue(cluster);
							if (cluster == 0x0fffffff){
								cluster = -1;
								break;
							}
						}
					}					
				}
				
				if (filePath[searcher] == '/'){
					// Todo - check if file is directory to continue deeper
					while(filePath[searcher] == '/') searcher++;
				}else{
					// Todo - check if directory
					if (filePath[searcher] == 0 && fileInfo){
						fileInfo->fileSize = SDGetDWord(inSectorShift+0x1C);
					}					
				}
				nameBuffer[0] = 0;
			} else {
				break;
			}
		}
	}
	if (targetSector) *targetSector = sectorStart + sectorCounter;
	if (targetSectorShift) *targetSectorShift = inSectorShift;
	return cluster;
}

unsigned int FSGetStartSector(int cluster){
	return rootAddr+((cluster-2)*sectorsPerCluster);
}

bool FSReadDir(char *path, DirectoryReader *dirReader){
	if (buffer){
		int cluster = FSGetCluster(path);
		if (cluster == -1) return false;
		dirReader->currentCluster = cluster;
		dirReader->sectorShift = 0;
		dirReader->inSectorShift = 0;
		return true;
	}
	return false;
}

bool FSReadNextFile(char *fileName, int fileNameLength, DirectoryReader *dirReader, FileInfo *fileInfo){
	char len;
	unsigned short shift;
	fileNameLength--;
	
	if (fileNameLength < 13){
		return false;
	}	
	
	if (buffer && dirReader->currentCluster != -1){
		while(1){
			shift = dirReader->inSectorShift;
			
			dirReader->inSectorShift += 32;
			if (dirReader->inSectorShift >= 512){
				dirReader->inSectorShift = 0;
				dirReader->sectorShift++;
				if (dirReader->sectorShift >= sectorsPerCluster){
					dirReader->currentCluster = -1;
					return false;
				}
			}
			
			SDReadSector(FSGetStartSector(dirReader->currentCluster) + dirReader->sectorShift);

			// Empty
			if (buffer[shift] == 0 || buffer[shift] == ' '|| buffer[shift] == 0xff) continue;
			
			// File deleted
			if (buffer[shift] == 0xE5) continue;
			
			// Long name
			if (buffer[shift+0x0b] == 0x0f){
				// TODO - do it
			} else {
				// File info
				FSGetNameFromFileStruct(buffer+shift, fileName);
				len = strlen(fileName);
				
				fileInfo->fileSize = SDGetDWord(shift+0x1C);
				fileInfo->permissions = SDGetWord(shift+0x14);
				fileInfo->flags = 0;
				
				if (SDGetByte(shift+0x0B) & 0x02){
					fileInfo->flags |= FILE_FLAGS_HIDDEN;
				}
				
				if (SDGetByte(shift+0x0B) & 0x04){
					fileInfo->flags |= (FILE_FLAGS_HIDDEN | FILE_FLAGS_SYSTEM);
				}
				
				if (cmpi(&fileName[len-3], "VEX")){
					fileInfo->fileType = FILE_TYPE_RUNNABLE;
				}else{
					if (SDGetByte(shift+0x0B) & 0x10){
						fileInfo->fileType = FILE_TYPE_DIRECTORY;
					} else {
						fileInfo->fileType = FILE_TYPE_UNKNOWN;
					}
				}
				return true;
			}
		}
	}
	return false;
}

bool FSReadFile(char *filePath, FileWorker *fileWorker){
	if (buffer){
		FileInfo fileInfo;
		int cluster = FSGetCluster(filePath, &fileInfo);
		if (cluster == -1) return false;
		fileWorker->currentCluster = cluster;
		fileWorker->inSectorShift = 0;
		fileWorker->sectorShift = 0;
		fileWorker->fileSize = fileInfo.fileSize;
		fileWorker->readed = 0;
		fileWorker->mode = 'r';
		return true;
	}	
	return false;
}

bool FSWriteFile(const char *filePath, FileWorker *fileWorker){
	const char *fileName;
	char realFileName[8];
	char realFileExt[3];
	
	char filePathFull[32];
	FileInfo fileInfo;
	int cluster;
	int fileSector, fileInSectorShift;
	char i;
	
	if (buffer){
		// Getting file name and path to folder
		fileName = &filePath[strlen(filePath) - 1];
		while(fileName != filePath && *fileName != '/' && *fileName) fileName--;
		if (*fileName == '/') fileName++;
		if (strlen(fileName) == 0) return false;
		if (fileName - filePath >= 64) return false; //???
		
		memcpy(filePathFull, filePath, fileName - filePath);
		filePathFull[fileName - filePath] = 0;
		
		
		// Getting real filename
		memset(realFileName, ' ', 8);
		memset(realFileExt, ' ', 3);
		for (i = 0; i < 8; i++){
			if (fileName[i] == 0 || fileName[i] == '.') break;
			realFileName[i] = fileName[i];
		}
		
		i = strlen(fileName);
		while(i > 0 && fileName[i] != '.') i--;
		
		if (fileName[i] == '.'){
			i++;
			char extSize = strlen(fileName) - i;
			if (extSize < 3){
				memcpy(realFileExt, &fileName[i], extSize);
			}else{
				memcpy(realFileExt, &fileName[i], 3);
			}
		}
		
		// Searching existing file
		cluster = FSGetCluster(filePath, &fileInfo, &fileSector, &fileInSectorShift);

		if (cluster == -1){
			// Adding file
			// First, let's find place in the folder
			cluster = FSGetCluster(filePathFull, &fileInfo);
			if (cluster == -1) return false;
			FSGetEmptyFilePlace(cluster, &fileSector, &fileInSectorShift);
			
			// New cluster for file
			cluster = FSMakeNewCluster();
		} else {
			// Using existing file
			FSRemoveClusterChain(cluster);
			FSSetClusterValue(cluster, 0x0fffffff);
		}
				
		// Writing file data
		SDReadSector(fileSector);
		
		memcpy(&buffer[fileInSectorShift], realFileName, 8);
		memcpy(&buffer[fileInSectorShift+8], realFileExt, 3);
			
		SDSetWord(fileInSectorShift+0x1A, cluster & 0xffff);
		SDSetWord(fileInSectorShift+0x14, cluster >> 16);
			
		
		if (!SDWriteSector(fileSector)) return false;
		
		fileWorker->startCluster = cluster;
		fileWorker->currentCluster = cluster;
		fileWorker->fileSector = fileSector;
		fileWorker->fileInSectorShift = fileInSectorShift;
		fileWorker->sectorShift = 0;
		fileWorker->inSectorShift = 0;
		fileWorker->fileSize = 0;

		
		fileWorker->mode = 'w';

		return true;
	}

	return false;
}

unsigned int FSRead(FileWorker *fileWorker, void *dst, unsigned int length){
	if (buffer && fileWorker->mode == 'r'){
		unsigned int startSectorInCluster = FSGetStartSector(fileWorker->currentCluster);
		char *c = (char*)dst;
		
		while(length--){
			if (fileWorker->readed >= fileWorker->fileSize) break;
			fileWorker->readed++;
			
			SDReadSector(startSectorInCluster + fileWorker->sectorShift);
			
			*c = buffer[fileWorker->inSectorShift];
			c++;
			fileWorker->inSectorShift++;
			
			if (fileWorker->inSectorShift >= 512){
				fileWorker->inSectorShift = 0;
				fileWorker->sectorShift++;
				if (fileWorker->sectorShift >= sectorsPerCluster){
					fileWorker->sectorShift = 0;
					fileWorker->currentCluster = FSGetClusterValue(fileWorker->currentCluster);
					if (fileWorker->currentCluster >= 0x0fffffff){
						fileWorker->fileSize = 0;
						break;
					}
					startSectorInCluster = FSGetStartSector(fileWorker->currentCluster);
				}
			}
		}
		
		return c - (char*)dst;
	}
	return 0;
}

unsigned int FSWrite(FileWorker *fileWorker, const void *src, int length){
	if (buffer && fileWorker->mode == 'w'){
		char *c = (char*)src;
		unsigned int startSectorInCluster = FSGetStartSector(fileWorker->currentCluster);
		
		while(length--){
			SDReadSector(startSectorInCluster + fileWorker->sectorShift);
			buffer[fileWorker->inSectorShift] = *c;
			c++;
			
			fileWorker->inSectorShift++;
			fileWorker->fileSize++;
			
			if (fileWorker->inSectorShift >= 512){
				SDWriteSector(startSectorInCluster + fileWorker->sectorShift);
				fileWorker->inSectorShift = 0;
				fileWorker->sectorShift++;
				if (fileWorker->sectorShift >= sectorsPerCluster){
					fileWorker->sectorShift = 0;
					int newCluster = FSMakeNewCluster();
					FSSetClusterValue(fileWorker->currentCluster, newCluster);
					fileWorker->currentCluster = newCluster;
					startSectorInCluster = FSGetStartSector(newCluster);
				}
			}
		}
		
		SDWriteSector(startSectorInCluster + fileWorker->sectorShift);
		return c - (char*)src;
	}
	return 0;
}

bool FSSeek(unsigned int shift, FileWorker *fileWorker){
	return false;
}

bool FSClose(FileWorker *fileWorker){
	if (fileWorker->mode == 'w'){
		// Saves File Size
		if (SDReadSector(fileWorker->fileSector)){
			SDSetDWord(fileWorker->fileInSectorShift+0x1C, fileWorker->fileSize);
			SDWriteSector(fileWorker->fileSector);
		}
	}
	fileWorker->mode = 'e';
	return true;
}

int FSMakeNewCluster(){
	int fatSector = fatStartAddr;
	unsigned int clusterNum = 0, inSectorShift = 0;

	while(1){
		SDReadSector(fatSector);
		if (SDGetDWord(inSectorShift) == 0){
			SDSetDWord(inSectorShift, 0x0fffffff);
			if (!SDWriteSector(fatSector)) return -1;
			return clusterNum;
		}
		clusterNum++;
		inSectorShift+=4;
		if (inSectorShift >= 512){
			fatSector++;
			inSectorShift = 0;
		}
	}
}

void FSRemoveClusterChain(int cluster){
	int nextCluster;
	while(1){
		nextCluster = FSGetClusterValue(cluster);
		FSSetClusterValue(cluster, 0);
		if (nextCluster == 0x0fffffff || nextCluster == 0) break;
		cluster = nextCluster;
	}
}

void FSSetClusterValue(int cluster, int value){
	int clusterPage = cluster / 128;
	int clusterPageShift = (cluster%128)*4;
	SDReadSector(fatStartAddr+clusterPage);
	SDSetDWord(clusterPageShift, value);
	SDWriteSector(fatStartAddr+clusterPage);
}

int FSGetClusterValue(int cluster){
	int clusterPage = cluster / 128;
	int clusterPageShift = (cluster%128)*4;
	SDReadSector(fatStartAddr+clusterPage);
	return SDGetDWord(clusterPageShift);
}

void FSClearCluster(int cluster){
	int sector = FSGetStartSector(cluster);
	memset(buffer, 0, 512);
	for (int i = 0; i < sectorsPerCluster; i++){
		SDWriteSector(sector+i);
	}
}

void FSGetEmptyFilePlace(int cluster, int *targetSector, int *targetSectorShift){
	int startSector = FSGetStartSector(cluster);
	int sectorShift = 0, inSectorShift = 0;
	int newCluster;
	
	while(1){
		SDReadSector(startSector + sectorShift);
		if (buffer[inSectorShift] == 0 || buffer[inSectorShift] == 0xE5) break;
		
		inSectorShift += 32;
		if (inSectorShift >= 512){
			sectorShift += 1;
			inSectorShift = 0;
			if (sectorShift >= sectorsPerCluster){
				// Making new block in cluster chain after per cluster limit
				sectorShift = 0;
				newCluster = FSMakeNewCluster();
				FSClearCluster(newCluster);
				FSSetClusterValue(cluster, newCluster);
				FSSetClusterValue(newCluster, 0x0fffffff);
			}
		}
	}
	*targetSector = startSector+sectorShift;
	*targetSectorShift = inSectorShift;
}




