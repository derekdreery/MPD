/* the Music Player Daemon (MPD)
 * (c)2003-2004 by Warren Dukes (shank@mercury.chem.pitt.edu)
 * This project's homepage is: http://www.musicpd.org
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "aac_decode.h"

#ifdef HAVE_FAAD

#define AAC_MAX_CHANNELS	6

#include "command.h"
#include "utils.h"
#include "audio.h"
#include "log.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <faad.h>

/* all code here is either based on or copied from FAAD2's frontend code */
typedef struct {
	long bytesIntoBuffer;
	long bytesConsumed;
	long fileOffset;
	unsigned char *buffer;
	int atEof;
	FILE *infile;
} AacBuffer;

void fillAacBuffer(AacBuffer *b) {
	if(b->bytesConsumed > 0) {
		int bread;

		if(b->bytesIntoBuffer) {
			memmove((void *)b->buffer,(void*)(b->buffer+
					b->bytesConsumed),b->bytesIntoBuffer);
		}

		if(!b->atEof) {
			bread = fread((void *)(b->buffer+b->bytesIntoBuffer),1,
					b->bytesConsumed,b->infile);
			if(bread!=b->bytesConsumed) b->atEof = 1;
			b->bytesIntoBuffer+=bread;
		}

		b->bytesConsumed = 0;

		if(b->bytesIntoBuffer > 3) {
			if(memcmp(b->buffer,"TAG",3)==0) b->bytesIntoBuffer = 0;
		}
		if(b->bytesIntoBuffer > 11) {
			if(memcmp(b->buffer,"LYRICSBEGIN",11)==0) {
				b->bytesIntoBuffer = 0;
			}
		}
		if(b->bytesIntoBuffer > 8) {
			if(memcmp(b->buffer,"APETAGEX",8)==0) {
				b->bytesIntoBuffer = 0;
			}
		}
	}
}

void advanceAacBuffer(AacBuffer * b, int bytes) {
	b->fileOffset+=bytes;
	b->bytesConsumed = bytes;
	b->bytesIntoBuffer-=bytes;
}

static int adtsSampleRates[] = {96000,88200,64000,48000,44100,32000,24000,22050,
				16000,12000,11025,8000,7350,0,0,0};

int adtsParse(AacBuffer * b, float * length) {
	int frames, frameLength;
	int tFrameLength = 0;
	int sampleRate = 0;
	float framesPerSec, bytesPerFrame;

	/* Read all frames to ensure correct time and bitrate */
	for(frames = 0; ;frames++) {
		fillAacBuffer(b);

		if(b->bytesIntoBuffer > 7) {
			/* check syncword */
			if (!((b->buffer[0] == 0xFF) && 
				((b->buffer[1] & 0xF6) == 0xF0)))
			{
				break;
			}

			if(frames==0) {
				sampleRate = adtsSampleRates[
						(b->buffer[2]&0x3c)>>2];
			}

			frameLength =  ((((unsigned int)b->buffer[3] & 0x3)) 
					<< 11) | (((unsigned int)b->buffer[4])  
                			<< 3) | (b->buffer[5] >> 5);

			tFrameLength+=frameLength;

			if(frameLength > b->bytesIntoBuffer) break;

			advanceAacBuffer(b,frameLength);
		}
		else break;
	}

	framesPerSec = (float)sampleRate/1024.0;
	if(frames!=0) {
		bytesPerFrame = (float)tFrameLength/(float)(frames*1000);
	}
	else bytesPerFrame = 0;
	if(framesPerSec!=0) *length = (float)frames/framesPerSec;

	return 1;
}

void initAacBuffer(FILE * fp, AacBuffer * b, float * length, 
		size_t * retFileread, size_t * retTagsize)
{
	size_t fileread;
	size_t bread;
	size_t tagsize;

	if(length) *length = -1;

	memset(b,0,sizeof(AacBuffer));

	b->infile = fp;

	fseek(b->infile,0,SEEK_END);
	fileread = ftell(b->infile);
	fseek(b->infile,0,SEEK_SET);

	b->buffer = malloc(FAAD_MIN_STREAMSIZE*AAC_MAX_CHANNELS);
	memset(b->buffer,0,FAAD_MIN_STREAMSIZE*AAC_MAX_CHANNELS);

	bread = fread(b->buffer,1,FAAD_MIN_STREAMSIZE*AAC_MAX_CHANNELS,
			b->infile);
	b->bytesIntoBuffer = bread;
	b->bytesConsumed = 0;
	b->fileOffset = 0;

	if(bread!=FAAD_MIN_STREAMSIZE*AAC_MAX_CHANNELS) b->atEof = 1;

	tagsize = 0;
	if(!memcmp(b->buffer,"ID3",3)) {
		tagsize = (b->buffer[6] << 21) | (b->buffer[7] << 14) |
				(b->buffer[8] << 7) | (b->buffer[9] << 0);

		tagsize+=10;
		advanceAacBuffer(b,tagsize);
		fillAacBuffer(b);
	}

	if(retFileread) *retFileread = fileread;
	if(retTagsize) *retTagsize = tagsize;

	if(!length) return;

	if((b->buffer[0] == 0xFF) && ((b->buffer[1] & 0xF6) == 0xF0)) {
		adtsParse(b, length);
		fseek(b->infile, tagsize, SEEK_SET);

		bread = fread(b->buffer, 1, 
				FAAD_MIN_STREAMSIZE*AAC_MAX_CHANNELS,
				b->infile);
		if(bread != FAAD_MIN_STREAMSIZE*AAC_MAX_CHANNELS) b->atEof = 1;
		else b->atEof = 0;
		b->bytesIntoBuffer = bread;
		b->bytesConsumed = 0;
		b->fileOffset = tagsize;
	}
	else if(memcmp(b->buffer,"ADIF",4) == 0) {
		int bitRate;
		int skipSize = (b->buffer[4] & 0x80) ? 9 : 0;
		bitRate = ((unsigned int)(b->buffer[4 + skipSize] & 0x0F)<<19) |
            			((unsigned int)b->buffer[5 + skipSize]<<11) |
            			((unsigned int)b->buffer[6 + skipSize]<<3) |
            			((unsigned int)b->buffer[7 + skipSize] & 0xE0);

		*length = fileread;
		if(*length!=0 && bitRate!=0) *length = *length*8.0/bitRate;
	}
}

float getAacFloatTotalTime(char * file) {
	AacBuffer b;
	float length;
	size_t fileread, tagsize;
	faacDecHandle decoder;
	faacDecConfigurationPtr config;
	unsigned long sampleRate;
	unsigned char channels;
	FILE * fp = fopen(file,"r");

	if(fp==NULL) return -1;

	initAacBuffer(fp,&b,&length,&fileread,&tagsize);

	if(length < 0) {
		decoder = faacDecOpen();

		config = faacDecGetCurrentConfiguration(decoder);
		config->outputFormat = FAAD_FMT_16BIT;
		faacDecSetConfiguration(decoder,config);

		fillAacBuffer(&b);
		if(faacDecInit(decoder,b.buffer,b.bytesIntoBuffer,
				&sampleRate,&channels) >= 0 &&
				sampleRate > 0 && channels > 0)
		{
			length = 0;
		}

		faacDecClose(decoder);
	}

	if(b.buffer) free(b.buffer);
	fclose(b.infile);

	return length;
}

int getAacTotalTime(char * file) {
	int time = -1;
	float length;

	if((length = getAacFloatTotalTime(file))>=0) time = length+0.5;

	return time;
}


int aac_decode(Buffer * cb, AudioFormat * af, DecoderControl * dc) {
	float time;
	float totalTime;
	faacDecHandle decoder;
	faacDecFrameInfo frameInfo;
	faacDecConfigurationPtr config;
	size_t bread;
	unsigned long sampleRate;
	unsigned char channels;
	int eof = 0;
	unsigned int sampleCount;
	char * sampleBuffer;
	size_t sampleBufferLen;
	int chunkLen = 0;
	/*float * seekTable;
	long seekTableEnd = -1;
	int seekPositionFound = 0;*/
	mpd_uint16 bitRate = 0;
	AacBuffer b;
	FILE * fp;

	if((totalTime = getAacFloatTotalTime(dc->file)) < 0) return -1;

	fp = fopen(dc->file,"r");

	if(fp==NULL) return -1;

	initAacBuffer(fp,&b,NULL,NULL,NULL);

	decoder = faacDecOpen();

	config = faacDecGetCurrentConfiguration(decoder);
	config->outputFormat = FAAD_FMT_16BIT;
#ifdef HAVE_FAACDECCONFIGURATION_DOWNMATRIX
	config->downMatrix = 1;
#endif
#ifdef HAVE_FAACDECCONFIGURATION_DONTUPSAMPLEIMPLICITSBR
	config->dontUpSampleImplicitSBR = 0;
#endif
	faacDecSetConfiguration(decoder,config);

	fillAacBuffer(&b);
	if((bread = faacDecInit(decoder,b.buffer,b.bytesIntoBuffer,
			&sampleRate,&channels)) < 0)
	{
		ERROR("Error not a AAC stream.\n");
		faacDecClose(decoder);
		fclose(b.infile);
		if(b.buffer) free(b.buffer);
		return -1;
	}

	af->sampleRate = sampleRate;
	af->channels = channels;
	af->bits = 16;

	cb->totalTime = totalTime;

	dc->state = DECODE_STATE_DECODE;
	dc->start = 0;
	time = 0.0;

	advanceAacBuffer(&b,bread);
	fillAacBuffer(&b);

	do {
		if(dc->seek) {
			dc->seekError = 1;
			dc->seek = 0;
		}
		
		sampleBuffer = faacDecDecode(decoder,&frameInfo,b.buffer,
						b.bytesIntoBuffer);
		advanceAacBuffer(&b,frameInfo.bytesconsumed);

		if(frameInfo.error > 0) {
			ERROR("error decoding AAC file: %s\n",dc->file);
			ERROR("faad2 error: %s\n",
				faacDecGetErrorMessage(frameInfo.error));
			eof = 1;
			break;
		}

		sampleCount = (unsigned long)(frameInfo.samples);

		if(sampleCount>0) {
			bitRate = frameInfo.bytesconsumed*8.0*
				frameInfo.channels*frameInfo.samplerate/
				frameInfo.samples/1024+0.5;
			time+= (float)(frameInfo.samples)/frameInfo.channels/
				frameInfo.samplerate;
		}
			
		sampleBufferLen = sampleCount*2;

		while(sampleBufferLen>0 && !dc->seek) {
			size_t size = sampleBufferLen>CHUNK_SIZE-chunkLen ? 
							CHUNK_SIZE-chunkLen:
							sampleBufferLen;
			while(cb->begin==cb->end && cb->wrap &&
					!dc->stop && !dc->seek)
			{
					usleep(10000);
			}
			if(dc->stop) {
				eof = 1;
				break;
			}
			else if(!dc->seek) {
				sampleBufferLen-=size;
				memcpy(cb->chunks+cb->end*CHUNK_SIZE+chunkLen,
						sampleBuffer,size);
				cb->times[cb->end] = time;
				cb->bitRate[cb->end] = bitRate;
				sampleBuffer+=size;
				chunkLen+=size;
				if(chunkLen>=CHUNK_SIZE) {
					cb->chunkSize[cb->end] = CHUNK_SIZE;
					++cb->end;
		
					if(cb->end>=buffered_chunks) {
						cb->end = 0;
						cb->wrap = 1;
					}
					chunkLen = 0;
				}
			}
		}

		fillAacBuffer(&b);

		if(b.bytesIntoBuffer==0) eof = 1;
	} while (!eof);

	if(!dc->stop && !dc->seek && chunkLen>0) {
		cb->chunkSize[cb->end] = chunkLen;
		++cb->end;
	
		if(cb->end>=buffered_chunks) {
			cb->end = 0;
			cb->wrap = 1;
		}
		chunkLen = 0;
	}

	faacDecClose(decoder);
	fclose(b.infile);
	if(b.buffer) free(b.buffer);

	if(dc->seek) dc->seek = 0;

	if(dc->stop) {
		dc->state = DECODE_STATE_STOP;
		dc->stop = 0;
	}
	else dc->state = DECODE_STATE_STOP;

	return 0;
}

#endif /* HAVE_FAAD */
