/**************************************************************************
    Lightspark, a free flash player implementation

    Copyright (C) 2009  Alessandro Pignotti (a.pignotti@sssup.it)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**************************************************************************/

#ifndef FLV_H
#define FLV_H

#include <istream>
#include "swftypes.h"

namespace lightspark
{

class FLV_HEADER
{
private:
	unsigned int dataOffset;
	unsigned int version;
	bool valid;
	bool _hasAudio;
	bool _hasVideo;
public:
	FLV_HEADER(std::istream& in);
	unsigned int skipAmount() const {return dataOffset;}
	bool isValid() const { return valid; }
	bool hasAudio() const { return _hasAudio; }
	bool hasVideo() const { return _hasVideo; }
};

class VideoTag
{
protected:
	uint32_t dataSize;
	uint32_t timestamp;
public:
	VideoTag(std::istream& s);
	uint32_t getDataSize() const { return dataSize; }
};

class ScriptDataTag: public VideoTag
{
private:
	uint32_t totalLen;
public:
	ScriptDataTag(std::istream& s);
	uint32_t getTotalLen() const { return totalLen; }
};

class ScriptDataString
{
private:
	uint32_t size;
	tiny_string val;
public:
	ScriptDataString(std::istream& s);
	const tiny_string& getString() const { return val; }
	uint32_t getSize() const { return size; }
};

class ScriptECMAArray
{
public:
	ScriptECMAArray(std::istream& s);
};

};

#endif