/**************************************************************************
    Lightspark, a free flash player implementation

    Copyright (C) 2009,2010  Alessandro Pignotti (a.pignotti@sssup.it)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**************************************************************************/

#include "compat.h"
#include <assert.h>
#include <GL/glew.h>

#include "decoder.h"
#include "platforms/fastpaths.h"
#include "swf.h"
#include "graphics.h"
#include "backends/rendering.h"

using namespace lightspark;
using namespace std;

bool VideoDecoder::setSize(uint32_t w, uint32_t h)
{
	if(w!=frameWidth || h!=frameHeight)
	{
		frameWidth=w;
		frameHeight=h;
		LOG(LOG_NO_INFO,_("VIDEO DEC: Video frame size ") << frameWidth << 'x' << frameHeight);
		resizeGLBuffers=true;
		videoTexture=sys->getRenderThread()->allocateTexture(frameWidth, frameHeight, true);
		return true;
	}
	else
		return false;
}

bool VideoDecoder::resizeIfNeeded(TextureChunk& tex)
{
	if(!resizeGLBuffers)
		return false;

	//Chunks are at least aligned to 128, we need 16
	assert_and_throw(tex.width==frameWidth && tex.height==frameHeight);
	resizeGLBuffers=false;
	return true;
}

void VideoDecoder::sizeNeeded(uint32_t& w, uint32_t& h) const
{
	//Return the actual width aligned to 16, the SSE2 packer is advantaged by this
	//and it comes for free as the texture tiles are aligned to 128
	w=(frameWidth+15)&0xfffffff0;
	h=frameHeight;
}

const TextureChunk& VideoDecoder::getTexture()
{
	return videoTexture;
}

void VideoDecoder::uploadFence()
{
	assert(fenceCount);
	ATOMIC_DECREMENT(fenceCount);
}

void VideoDecoder::waitForFencing()
{
	ATOMIC_INCREMENT(fenceCount);
}

#ifdef ENABLE_LIBAVCODEC
bool FFMpegVideoDecoder::fillDataAndCheckValidity()
{
	if(frameRate==0 && codecContext->time_base.num!=0)
	{
		frameRate=codecContext->time_base.den;
		frameRate/=codecContext->time_base.num;
		if(videoCodec==H264) //H264 has half ticks (usually?)
			frameRate/=2;
	}
	else if(frameRate==0)
		return false;

	if(codecContext->width!=0 && codecContext->height!=0)
		setSize(codecContext->width, codecContext->height);
	else
		return false;

	return true;
}

FFMpegVideoDecoder::FFMpegVideoDecoder(LS_VIDEO_CODEC codecId, uint8_t* initdata, uint32_t datalen, double frameRateHint):curBuffer(0),curBufferOffset(0),
	codecContext(NULL),mutex("VideoDecoder")
{
	//The tag is the header, initialize decoding
	codecContext=avcodec_alloc_context();
	AVCodec* codec=NULL;
	videoCodec=codecId;
	if(codecId==H264)
	{
		//TODO: serialize access to avcodec_open
		const enum CodecID FFMPEGcodecId=CODEC_ID_H264;
		codec=avcodec_find_decoder(FFMPEGcodecId);
		assert(codec);

		//If this tag is the header, fill the extradata for the codec
		codecContext->extradata=initdata;
		codecContext->extradata_size=datalen;

		//Ignore the frameRateHint as the rate is gathered from the video data
	}
	else if(codecId==H263)
	{
		//TODO: serialize access to avcodec_open
		const enum CodecID FFMPEGcodecId=CODEC_ID_FLV1;
		codec=avcodec_find_decoder(FFMPEGcodecId);
		assert(codec);

		//Exploit the frame rate information
		assert(frameRateHint!=0.0);
		frameRate=frameRateHint;
	}

	if(avcodec_open(codecContext, codec)<0)
		throw RunTimeException("Cannot open decoder");

	if(fillDataAndCheckValidity())
		status=VALID;
	else
		status=INIT;

	frameIn=avcodec_alloc_frame();
}

FFMpegVideoDecoder::~FFMpegVideoDecoder()
{
	while(fenceCount);
	assert(codecContext);
	av_free(codecContext);
	av_free(frameIn);
}

//setSize is called from the routine that inserts new frames
void FFMpegVideoDecoder::setSize(uint32_t w, uint32_t h)
{
	if(VideoDecoder::setSize(w,h))
	{
		//Discard all the frames
		while(discardFrame());

		//As the size chaged, reset the buffer
		uint32_t bufferSize=frameWidth*frameHeight*4;
		buffers.regen(YUVBufferGenerator(bufferSize));
	}
}

void FFMpegVideoDecoder::skipUntil(uint32_t time)
{
	while(1)
	{
		if(buffers.isEmpty())
			break;
		if(buffers.front().time>=time)
			break;
		discardFrame();
	}
}
void FFMpegVideoDecoder::skipAll()
{
	while(!buffers.isEmpty())
		discardFrame();
}

bool FFMpegVideoDecoder::discardFrame()
{
	Locker locker(mutex);
	//We don't want ot block if no frame is available
	bool ret=buffers.nonBlockingPopFront();
	if(flushing && buffers.isEmpty()) //End of our work
	{
		status=FLUSHED;
		flushed.signal();
	}
	return ret;
}

bool FFMpegVideoDecoder::decodeData(uint8_t* data, uint32_t datalen, uint32_t time)
{
	if(datalen==0)
		return false;
	int frameOk=0;
#if HAVE_AVCODEC_DECODE_VIDEO2
	AVPacket pkt;
	av_init_packet(&pkt);
	pkt.data=data;
	pkt.size=datalen;
	int ret=avcodec_decode_video2(codecContext, frameIn, &frameOk, &pkt);
#else
	int ret=avcodec_decode_video(codecContext, frameIn, &frameOk, data, datalen);
#endif
	assert_and_throw(ret==(int)datalen);
	if(frameOk)
	{
		assert(codecContext->pix_fmt==PIX_FMT_YUV420P);

		if(status==INIT && fillDataAndCheckValidity())
			status=VALID;

		assert(frameIn->pts==AV_NOPTS_VALUE || frameIn->pts==0);

		copyFrameToBuffers(frameIn, time);
	}
	return true;
}

void FFMpegVideoDecoder::copyFrameToBuffers(const AVFrame* frameIn, uint32_t time)
{
	YUVBuffer& curTail=buffers.acquireLast();
	//Only one thread may access the tail
	int offset[3]={0,0,0};
	for(uint32_t y=0;y<frameHeight;y++)
	{
		memcpy(curTail.ch[0]+offset[0],frameIn->data[0]+(y*frameIn->linesize[0]),frameWidth);
		offset[0]+=frameWidth;
	}
	for(uint32_t y=0;y<frameHeight/2;y++)
	{
		memcpy(curTail.ch[1]+offset[1],frameIn->data[1]+(y*frameIn->linesize[1]),frameWidth/2);
		memcpy(curTail.ch[2]+offset[2],frameIn->data[2]+(y*frameIn->linesize[2]),frameWidth/2);
		offset[1]+=frameWidth/2;
		offset[2]+=frameWidth/2;
	}
	curTail.time=time;

	buffers.commitLast();
}

void FFMpegVideoDecoder::upload(uint8_t* data, uint32_t w, uint32_t h) const
{
	if(buffers.isEmpty())
		return;
	//Verify that the size are right
	assert_and_throw(w==((frameWidth+15)&0xfffffff0) && h==frameHeight);
	//At least a frame is available
	const YUVBuffer& cur=buffers.front();
	//If the width is compatible with full aligned accesses use the aligned version of the packer
	if(frameWidth%32==0)
		fastYUV420ChannelsToYUV0Buffer_SSE2Aligned(cur.ch[0],cur.ch[1],cur.ch[2],data,frameWidth,frameHeight);
	else
		fastYUV420ChannelsToYUV0Buffer_SSE2Unaligned(cur.ch[0],cur.ch[1],cur.ch[2],data,frameWidth,frameHeight);
}

void FFMpegVideoDecoder::YUVBufferGenerator::init(YUVBuffer& buf) const
{
	if(buf.ch[0])
	{
		free(buf.ch[0]);
		free(buf.ch[1]);
		free(buf.ch[2]);
	}
#ifdef WIN32
	//FIXME!!
#else
	int ret=posix_memalign((void**)&buf.ch[0], 16, bufferSize);
	assert(ret==0);
	ret=posix_memalign((void**)&buf.ch[1], 16, bufferSize/4);
	assert(ret==0);
	ret=posix_memalign((void**)&buf.ch[2], 16, bufferSize/4);
	assert(ret==0);
#endif
}
#endif //ENABLE_LIBAVCODEC

void* AudioDecoder::operator new(size_t s)
{
	void* retAddr=NULL;

	// fix warnings
#ifndef NDEBUG
	int ret =
#endif
	aligned_malloc(&retAddr, 16, s);
	assert(ret==0);

	assert(retAddr);
	return retAddr;
}
void AudioDecoder::operator delete(void* addr)
{
	aligned_free(addr);
}

bool AudioDecoder::discardFrame()
{
	//We don't want ot block if no frame is available
	bool ret=samplesBuffer.nonBlockingPopFront();
	if(flushing && samplesBuffer.isEmpty()) //End of our work
	{
		status=FLUSHED;
		flushed.signal();
	}
	return ret;
}

uint32_t AudioDecoder::copyFrame(int16_t* dest, uint32_t len)
{
	assert(dest);
	if(samplesBuffer.isEmpty())
		return 0;
	uint32_t frameSize=min(samplesBuffer.front().len,len);
	memcpy(dest,samplesBuffer.front().current,frameSize);
	samplesBuffer.front().len-=frameSize;
	assert(!(samplesBuffer.front().len&0x80000000));
	if(samplesBuffer.front().len==0)
	{
		samplesBuffer.nonBlockingPopFront();
		if(flushing && samplesBuffer.isEmpty()) //End of our work
		{
			status=FLUSHED;
			flushed.signal();
		}
	}
	else
	{
		samplesBuffer.front().current+=frameSize/2;
		samplesBuffer.front().time+=frameSize/getBytesPerMSec();
	}
	return frameSize;
}

uint32_t AudioDecoder::getFrontTime() const
{
	assert(!samplesBuffer.isEmpty());
	return samplesBuffer.front().time;
}

void AudioDecoder::skipUntil(uint32_t time, uint32_t usecs)
{
	assert(isValid());
//	while(1) //Should loop, but currently only usec adjustements are requested
	{
		if(samplesBuffer.isEmpty())
			return;
		FrameSamples& cur=samplesBuffer.front();
		assert(time==cur.time);
		if(usecs==0) //Nothing to skip
			return;
		//Check how many bytes are needed to fill the gap
		uint32_t bytesToDiscard=(time-cur.time)*getBytesPerMSec()+usecs*getBytesPerMSec()/1000;
		bytesToDiscard&=0xfffffffe;

		if(cur.len<=bytesToDiscard) //The whole frame is droppable
			discardFrame();
		else
		{
			assert((bytesToDiscard%2)==0);
			cur.len-=bytesToDiscard;
			assert(!(cur.len&0x80000000));
			cur.current+=(bytesToDiscard/2);
			cur.time=time;
			return;
		}
	}
}

void AudioDecoder::skipAll()
{
	while(!samplesBuffer.isEmpty())
		discardFrame();
}

#ifdef ENABLE_LIBAVCODEC
FFMpegAudioDecoder::FFMpegAudioDecoder(LS_AUDIO_CODEC audioCodec, uint8_t* initdata, uint32_t datalen)
{
	CodecID codecId;
	switch(audioCodec)
	{
		case AAC:
			codecId=CODEC_ID_AAC;
			break;
		case MP3:
			codecId=CODEC_ID_MP3;
			break;
		default:
			::abort();
	}
	AVCodec* codec=avcodec_find_decoder(codecId);
	assert(codec);

	codecContext=avcodec_alloc_context();

	if(initdata)
	{
		codecContext->extradata=initdata;
		codecContext->extradata_size=datalen;
	}

	if(avcodec_open(codecContext, codec)<0)
		throw RunTimeException("Cannot open decoder");

	if(fillDataAndCheckValidity())
		status=VALID;
	else
		status=INIT;
}

bool FFMpegAudioDecoder::fillDataAndCheckValidity()
{
	if(codecContext->sample_rate!=0)
	{
		LOG(LOG_NO_INFO,_("AUDIO DEC: Audio sample rate ") << codecContext->sample_rate);
		sampleRate=codecContext->sample_rate;
	}
	else
		return false;

	if(codecContext->channels!=0)
	{
		LOG(LOG_NO_INFO, _("AUDIO DEC: Audio channels ") << codecContext->channels);
		channelCount=codecContext->channels;
	}
	else
		return false;

	return true;
}

uint32_t FFMpegAudioDecoder::decodeData(uint8_t* data, uint32_t datalen, uint32_t time)
{
	FrameSamples& curTail=samplesBuffer.acquireLast();
	int maxLen=AVCODEC_MAX_AUDIO_FRAME_SIZE;
#if HAVE_AVCODEC_DECODE_AUDIO3
	AVPacket pkt;
	av_init_packet(&pkt);
	pkt.data=data;
	pkt.size=datalen;
	uint32_t ret=avcodec_decode_audio3(codecContext, curTail.samples, &maxLen, &pkt);
#else
	uint32_t ret=avcodec_decode_audio2(codecContext, curTail.samples, &maxLen, data, datalen);
#endif
	assert_and_throw(ret==datalen);

	if(status==INIT && fillDataAndCheckValidity())
		status=VALID;

	curTail.len=maxLen;
	assert(!(curTail.len&0x80000000));
	assert(maxLen%2==0);
	curTail.current=curTail.samples;
	curTail.time=time;
	samplesBuffer.commitLast();
	return maxLen;
}
#endif //ENABLE_LIBAVCODEC
