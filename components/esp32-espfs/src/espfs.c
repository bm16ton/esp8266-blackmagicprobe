/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
This is a simple read-only implementation of a file system. It uses a block of data coming from the
mkespfsimg tool, and can use that block to do abstracted operations on the files that are in there.
It's written for use with httpd, but doesn't need to be used as such.
*/

//These routines can also be tested by comping them in with the espfstest tool. This
//simplifies debugging, but needs some slightly different headers. The #ifdef takes
//care of that.

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "espfsformat.h"
#include "espfs.h"
#include "sdkconfig.h"

#if CONFIG_ESPFS_USE_HEATSHRINK
#include "heatshrink_config_custom.h"
#include "heatshrink_decoder.h"
#endif

#include "esp_log.h"
const static char* TAG = "espfs";

// Define to enable more verbose output
#undef VERBOSE_OUTPUT

//ESP32, for now, stores memory locations here.
static char* espFsData = NULL;


struct EspFsFile {
	EspFsHeader *header;
	char decompressor;
	int32_t posDecomp;
	char *posStart;
	char *posComp;
	void *decompData;
};


#define readFlashUnaligned memcpy
#define readFlashAligned(a,b,c) memcpy(a, (uint32_t*)b, c)

EspFsInitResult espFsInit(const void *flashAddress) {
	// check if there is valid header at address
	EspFsHeader testHeader;
	readFlashUnaligned((char*)&testHeader, (char*)flashAddress, sizeof(EspFsHeader));
	if (testHeader.magic != ESPFS_MAGIC) {
		ESP_LOGE(TAG, "Esp magic: %x (should be %x)", testHeader.magic, ESPFS_MAGIC);
		return ESPFS_INIT_RESULT_NO_IMAGE;
	}

	espFsData = (char *)flashAddress;
	return ESPFS_INIT_RESULT_OK;
}

//Copies len bytes over from dst to src, but does it using *only*
//aligned 32-bit reads. Yes, it's no too optimized but it's short and sweet and it works.

// Returns flags of opened file.
int espFsFlags(EspFsFile *fh) {
	if (fh == NULL) {
		ESP_LOGE(TAG, "File handle not ready");
		return -1;
	}

	int8_t flags;
	readFlashUnaligned((char*)&flags, (char*)&fh->header->flags, 1);
	return (int)flags;
}

//Open a file and return a pointer to the file desc struct.
EspFsFile *espFsOpen(const char *fileName) {
	if (espFsData == NULL) {
		ESP_LOGE(TAG, "Call espFsInit first");
		return NULL;
	}
	char *p=espFsData;
	char *hpos;
	char namebuf[256];
	EspFsHeader h;
	EspFsFile *r;
	//Strip first initial slash
	//We should not strip any next slashes otherwise there is potential security risk when mapped authentication handler will not invoke (ex. ///security.html)
	if(fileName[0]=='/') fileName++;
	//Go find that file!
	while(1) {
		hpos=p;
		//Grab the next file header.
		readFlashAligned((uintptr_t*)&h, (uintptr_t)p, sizeof(EspFsHeader));

		if (h.magic!=ESPFS_MAGIC) {
			ESP_LOGE(TAG, "Magic mismatch. EspFS image broken.");
			return NULL;
		}
		if (h.flags&FLAG_LASTFILE) {
#ifdef VERBOSE_OUTPUT
			ESP_LOGD(TAG, "End of image");
#endif
			return NULL;
		}
		//Grab the name of the file.
		p+=sizeof(EspFsHeader);
		readFlashAligned((uint32_t*)&namebuf, (uintptr_t)p, sizeof(namebuf));
#ifdef VERBOSE_OUTPUT
		ESP_LOGD(TAG, "Found file '%s'. Namelen=%x fileLenComp=%x, compr=%d flags=%d",
				namebuf, (unsigned int)h.nameLen, (unsigned int)h.fileLenComp, h.compression, h.flags);
#endif
		if (strcmp(namebuf, fileName)==0) {
			//Yay, this is the file we need!
			p+=h.nameLen; //Skip to content.
			r=(EspFsFile *)malloc(sizeof(EspFsFile)); //Alloc file desc mem
#ifdef VERBOSE_OUTPUT
			ESP_LOGD(TAG, "Alloc %p", r);
#endif
			if (r==NULL) return NULL;
			r->header=(EspFsHeader *)hpos;
			r->decompressor=h.compression;
			r->posComp=p;
			r->posStart=p;
			r->posDecomp=0;
			if (h.compression==COMPRESS_NONE) {
				r->decompData=NULL;
#if CONFIG_ESPFS_USE_HEATSHRINK
			} else if (h.compression==COMPRESS_HEATSHRINK) {
				//File is compressed with Heatshrink.
				char parm;
				heatshrink_decoder *dec;
				//Decoder params are stored in 1st byte.
				readFlashUnaligned(&parm, r->posComp, 1);
				r->posComp++;
#ifdef VERBOSE_OOTPUT
				ESP_LOGD(TAG, "Heatshrink compressed file; decode parms = %x", parm);
#endif
				dec=heatshrink_decoder_alloc(16, (parm>>4)&0xf, parm&0xf);
				r->decompData=dec;
#endif
			} else {
				ESP_LOGE(TAG, "Invalid compression: %d", h.compression);
				free(r);
				return NULL;
			}
			return r;
		}
		//We don't need this file. Skip name and file
		p+=h.nameLen+h.fileLenComp;
		if ((uintptr_t)p&3) p+=4-((uintptr_t)p&3); //align to next 32bit val
	}
}

//Read len bytes from the given file into buff. Returns the actual amount of bytes read.
int espFsRead(EspFsFile *fh, char *buff, int len) {
	int flen;
#if CONFIG_ESPFS_USE_HEATSHRINK
	int fdlen;
#endif
	if (fh==NULL) return 0;

	readFlashUnaligned((char*)&flen, (char*)&fh->header->fileLenComp, 4);
	//Cache file length.
	//Do stuff depending on the way the file is compressed.
	if (fh->decompressor==COMPRESS_NONE) {
		int toRead;
		toRead=flen-(fh->posComp-fh->posStart);
		if (len>toRead) len=toRead;
#ifdef VERBOSE_OUTPUT
		ESP_LOGD(TAG, "Reading %d bytes from %x", len, (unsigned int)fh->posComp);
#endif
		readFlashUnaligned(buff, fh->posComp, len);
		fh->posDecomp+=len;
		fh->posComp+=len;
#ifdef VERBOSE_OUTPUT
		ESP_LOGD(TAG, "Done reading %d bytes, pos=%x", len, fh->posComp);
#endif
		return len;
#if CONFIG_ESPFS_USE_HEATSHRINK
	} else if (fh->decompressor==COMPRESS_HEATSHRINK) {
		readFlashUnaligned((char*)&fdlen, (char*)&fh->header->fileLenDecomp, 4);
		int decoded=0;
		size_t elen, rlen;
		char ebuff[16];
		heatshrink_decoder *dec=(heatshrink_decoder *)fh->decompData;
#ifdef VERBOSE_OUTPUT
		ESP_LOGD(TAG, "Alloc %p", dec);
#endif
		if (fh->posDecomp == fdlen) {
			return 0;
		}

		// We must ensure that whole file is decompressed and written to output buffer.
		// This means even when there is no input data (elen==0) try to poll decoder until
		// posDecomp equals decompressed file length

		while(decoded<len) {
			//Feed data into the decompressor
			//ToDo: Check ret val of heatshrink fns for errors
			elen=flen-(fh->posComp - fh->posStart);
			if (elen>0) {
				readFlashUnaligned(ebuff, fh->posComp, 16);
				heatshrink_decoder_sink(dec, (uint8_t *)ebuff, (elen>16)?16:elen, &rlen);
				fh->posComp+=rlen;
			}
			//Grab decompressed data and put into buff
			heatshrink_decoder_poll(dec, (uint8_t *)buff, len-decoded, &rlen);
			fh->posDecomp+=rlen;
			buff+=rlen;
			decoded+=rlen;

#ifdef VERBOSE_OUTPUT
			ESP_LOGD(TAG, "Elen %d rlen %d d %d pd %ld fdl %d\n",elen,rlen,decoded, fh->posDecomp, fdlen);
#endif

			if (elen == 0) {
				if (fh->posDecomp == fdlen) {
#ifdef VERBOSE_OUTPUT
					ESP_LOGD(TAG, "Decoder finish");
#endif
					heatshrink_decoder_finish(dec);
				}
				return decoded;
			}
		}
		return len;
#endif
	}
	return 0;
}

//Seek in the file.
int espFsSeek(EspFsFile *fh, long offset, int mode)
{
	if (fh==NULL) return -1;

	if (mode == SEEK_SET) {
		if (offset < 0) {
			return -1;
		} else if (offset == 0) {
			fh->posComp = fh->posStart;
			fh->posDecomp = 0;
		} else if (fh->decompressor == COMPRESS_NONE) {
			if (offset > fh->header->fileLenComp) {
				offset = fh->header->fileLenComp;
			}
			fh->posComp = fh->posStart + offset;
			fh->posDecomp = offset;
		} else {
			return -1;
		}
	} else if (mode == SEEK_CUR) {
		if (offset == 0) {
			return fh->posDecomp;
		} else if (fh->decompressor == COMPRESS_NONE) {
			if (offset < 0) {
				if (fh->posDecomp + offset < 0) {
					fh->posComp = fh->posStart;
					fh->posDecomp = 0;
				} else {
					fh->posComp = fh->posComp + offset;
					fh->posDecomp = fh->posDecomp + offset;
				}
			} else {
				if (fh->posDecomp + offset > fh->header->fileLenComp) {
					fh->posComp = fh->posStart + fh->header->fileLenComp;
					fh->posDecomp = fh->header->fileLenComp;
				} else {
					fh->posComp = fh->posComp + offset;
					fh->posDecomp = fh->posDecomp + offset;
				}
			}
		} else {
			return -1;
		}
	} else if (mode == SEEK_END && fh->decompressor == COMPRESS_NONE) {
		if (offset == 0) {
			fh->posComp = fh->posStart + fh->header->fileLenComp;
			fh->posDecomp = fh->header->fileLenComp;
		} else if (offset < 0) {
			if (fh->header->fileLenComp + offset < 0) {
				fh->posComp = fh->posStart;
				fh->posDecomp = 0;
			} else {
				fh->posComp = fh->posStart + fh->header->fileLenComp + offset;
				fh->posDecomp = fh->header->fileLenComp + offset;
			}
		} else {
			return -1;
		}
	} else {
		return -1;
	}
	return fh->posDecomp;
}

//Close the file.
void espFsClose(EspFsFile *fh) {
	if (fh==NULL) return;
#if CONFIG_ESPFS_USE_HEATSHRINK
	if (fh->decompressor==COMPRESS_HEATSHRINK) {
		heatshrink_decoder *dec=(heatshrink_decoder *)fh->decompData;
		heatshrink_decoder_free(dec);
#ifdef VERBOSE_OUTPUT
		ESP_LOGD(TAG, "Freed %p", dec);
#endif
	}
#endif

#ifdef VERBOSE_OUTPUT
	ESP_LOGD(TAG, "Freed %p", fh);
#endif

	free(fh);
}
