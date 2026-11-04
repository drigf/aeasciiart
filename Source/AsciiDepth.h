// AsciiDepth.h
//
// After Effects glue for the ASCII Depth effect.  The actual image processing
// lives in Core/AsciiCore.*; this file only describes the parameters and moves
// pixels between After Effects and the core.

#pragma once

#ifndef ASCIIDEPTH_H
#define ASCIIDEPTH_H

typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned short u_int16;
typedef unsigned long u_long;
typedef short int int16;
#define PF_TABLE_BITS 12
#define PF_TABLE_SZ_16 4096

#define PF_DEEP_COLOR_AWARE 1  // make sure we get 16bpc pixels; AE_Effect.h checks for this.

#include "AEConfig.h"

#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AE_EffectCBSuites.h"
#include "String_Utils.h"
#include "AE_GeneralPlug.h"
#include "AEFX_ChannelDepthTpl.h"
#include "AEGP_SuiteHandler.h"

// ---- Identity ----------------------------------------------------------------

#define ASCIIDEPTH_NAME "ASCII Depth"
#define ASCIIDEPTH_MATCH_NAME "NSDEV ASCII Depth"  // never change once projects exist
#define ASCIIDEPTH_CATEGORY "Stylize"
#define ASCIIDEPTH_DESCRIPTION "Turns the layer into ASCII text. Glyph brightness shows depth: near = white, far = dark grey."

#define MAJOR_VERSION 1
#define MINOR_VERSION 0
#define BUG_VERSION 0
#define STAGE_VERSION PF_Stage_DEVELOP
#define BUILD_VERSION 1

// ---- Parameters -----------------------------------------------------------------
//
// ParamIndex is the position in the params[] array passed to every command.
// Topic start/end markers occupy a slot each.  Keep this and the
// ParamsSetup() order in AsciiDepth.cpp in sync.

enum ParamIndex
{
    P_INPUT = 0,

    P_TOPIC_CHARS,
    P_CELL_SIZE,
    P_CELL_ASPECT,
    P_CHARSET,
    P_INVERT_CHARS,
    P_TOPIC_CHARS_END,

    P_TOPIC_TONE,
    P_BRIGHTNESS,
    P_CONTRAST,
    P_GAMMA,
    P_TOPIC_TONE_END,

    P_TOPIC_DEPTH,
    P_DEPTH_SOURCE,
    P_DEPTH_LAYER,
    P_INVERT_DEPTH,
    P_NORMALIZE_DEPTH,
    P_DEPTH_CONTRAST,
    P_DEPTH_SMOOTHING,
    P_NEAR_LEVEL,
    P_FAR_LEVEL,
    P_TOPIC_DEPTH_END,

    P_TOPIC_CUES,
    P_CUE_FOCUS,
    P_CUE_HEIGHT,
    P_CUE_BRIGHT,
    P_TOPIC_CUES_END,

    P_BACKGROUND,

    P_NUM_PARAMS
};

// Stable on-disk IDs.  Never renumber these once projects are saved; only append.
enum ParamDiskID
{
    ID_TOPIC_CHARS = 1,
    ID_CELL_SIZE,
    ID_CELL_ASPECT,
    ID_CHARSET,
    ID_INVERT_CHARS,
    ID_TOPIC_CHARS_END,

    ID_TOPIC_TONE,
    ID_BRIGHTNESS,
    ID_CONTRAST,
    ID_GAMMA,
    ID_TOPIC_TONE_END,

    ID_TOPIC_DEPTH,
    ID_DEPTH_SOURCE,
    ID_DEPTH_LAYER,
    ID_INVERT_DEPTH,
    ID_DEPTH_CONTRAST,
    ID_DEPTH_SMOOTHING,
    ID_NEAR_LEVEL,
    ID_FAR_LEVEL,
    ID_TOPIC_DEPTH_END,

    ID_TOPIC_CUES,
    ID_CUE_FOCUS,
    ID_CUE_HEIGHT,
    ID_CUE_BRIGHT,
    ID_TOPIC_CUES_END,

    ID_BACKGROUND,

    // New parameters are appended here so existing projects keep their values.
    ID_NORMALIZE_DEPTH
};

extern "C"
{
    DllExport PF_Err EffectMain(
        PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output, void* extra);
}

#endif  // ASCIIDEPTH_H
