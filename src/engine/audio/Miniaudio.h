#pragma once
// miniaudio (single header, MIT-0) for the sound devices of the own audio engine: only the device part, no decoders,
// encoders or graph (FFmpeg reads the files). The implementation is compiled once in Miniaudio.cpp.

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include <miniaudio.h>
