// Shim for PPSSPP's Core/HW/SimpleAudioDec.h (rcsm audio extraction): just the AudioDecoder
// interface the in-tree ATRAC3/ATRAC3+ decoder implements (the original pulls in the MPEG/FFmpeg
// media engine). Derived from PPSSPP, Copyright (c) 2012- PPSSPP Project. GPL-2.0+.
#pragma once

#include <cstdint>
#include "Common/CommonTypes.h"
#include "Core/HLE/sceAudiocodec.h"

// Decodes packet by packet - does NOT demux.
class AudioDecoder {
public:
	virtual ~AudioDecoder() {}

	virtual PSPAudioType GetAudioType() const = 0;

	// inbytesConsumed can include skipping metadata.
	// outSamples is in stereo samples. So you have to multiply by 4 for 16-bit stereo audio to get bytes.
	// For Atrac3, if *outSamples != 0, it'll cap the number of samples to output.
	virtual bool Decode(const uint8_t *inbuf, int inbytes, int *inbytesConsumed, int outputChannels, int16_t *outbuf, int *outSamples) = 0;
	virtual bool IsOK() const = 0;

	virtual void SetChannels(int channels) = 0;
	virtual void FlushBuffers() {}

	void SetCtxPtr(uint32_t ptr) { ctxPtr = ptr; }
	uint32_t GetCtxPtr() const { return ctxPtr; }

private:
	uint32_t ctxPtr = 0xFFFFFFFF;
};
