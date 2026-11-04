// AsciiDepth.cpp
//
// After Effects entry points, parameter definitions and the render hand-off.
// All the interesting pixel work is in Core/AsciiCore.cpp.

#include "AsciiDepth.h"

#include <exception>
#include <new>

#include "Core/AsciiCore.h"

// ---------------------------------------------------------------------------
// About / global setup
// ---------------------------------------------------------------------------

static PF_Err About(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    suites.ANSICallbacksSuite1()->sprintf(
        out_data->return_msg, "%s v%d.%d\r%s", ASCIIDEPTH_NAME, MAJOR_VERSION, MINOR_VERSION, ASCIIDEPTH_DESCRIPTION);

    return PF_Err_NONE;
}

static PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    out_data->my_version = PF_VERSION(MAJOR_VERSION, MINOR_VERSION, BUG_VERSION, STAGE_VERSION, BUILD_VERSION);

    // These MUST match AE_Effect_Global_OutFlags / _2 in AsciiDepthPiPL.r.
    out_data->out_flags = PF_OutFlag_DEEP_COLOR_AWARE;  // 8 and 16 bpc
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_THREADED_RENDERING;

    return PF_Err_NONE;
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

static PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    PF_ParamDef def;

    // --- Characters ---------------------------------------------------------
    PF_ADD_TOPICX("Characters", 0, ID_TOPIC_CHARS);

    PF_ADD_FLOAT_SLIDERX("Cell Size", 3, 128, 4, 48, 12, PF_Precision_TENTHS, 0, 0, ID_CELL_SIZE);
    PF_ADD_FLOAT_SLIDERX("Cell Aspect (H/W)", 0.5, 4, 1, 3, 1.7, PF_Precision_HUNDREDTHS, 0, 0, ID_CELL_ASPECT);
    PF_ADD_POPUPX("Character Set", 4, 1, "Classic|Simple|Binary|Blocks", 0, ID_CHARSET);
    PF_ADD_CHECKBOXX("Invert Characters", FALSE, 0, ID_INVERT_CHARS);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_TOPIC_CHARS_END);

    // --- Tone -----------------------------------------------------------------
    PF_ADD_TOPICX("Tone", 0, ID_TOPIC_TONE);

    PF_ADD_FLOAT_SLIDERX("Brightness", -100, 100, -100, 100, 0, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_BRIGHTNESS);
    PF_ADD_FLOAT_SLIDERX("Contrast", 0, 400, 0, 300, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_CONTRAST);
    PF_ADD_FLOAT_SLIDERX("Gamma", 0.1, 4, 0.2, 3, 1, PF_Precision_HUNDREDTHS, 0, 0, ID_GAMMA);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_TOPIC_TONE_END);

    // --- Depth --------------------------------------------------------------------
    PF_ADD_TOPICX("Depth", 0, ID_TOPIC_DEPTH);

    PF_ADD_POPUPX("Depth Source", 3, 1, "Auto Estimate|Depth Layer|Subject (Apple Vision)", 0, ID_DEPTH_SOURCE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Depth Layer (white = near)", PF_LayerDefault_NONE, ID_DEPTH_LAYER);

    PF_ADD_CHECKBOXX("Invert Depth", FALSE, 0, ID_INVERT_DEPTH);
    PF_ADD_CHECKBOXX("Normalize Depth Layer", TRUE, 0, ID_NORMALIZE_DEPTH);
    PF_ADD_FLOAT_SLIDERX("Depth Contrast", 0, 400, 0, 300, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_DEPTH_CONTRAST);
    PF_ADD_FLOAT_SLIDERX("Depth Smoothing", 0, 10, 0, 10, 2, PF_Precision_TENTHS, 0, 0, ID_DEPTH_SMOOTHING);
    PF_ADD_FLOAT_SLIDERX("Near Level (white)", 0, 100, 0, 100, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_NEAR_LEVEL);
    PF_ADD_FLOAT_SLIDERX("Far Level (dark grey)", 0, 100, 0, 100, 25, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_FAR_LEVEL);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_TOPIC_DEPTH_END);

    // --- Auto depth cues ---------------------------------------------------------------
    PF_ADD_TOPICX("Auto Depth Cues", PF_ParamFlag_START_COLLAPSED, ID_TOPIC_CUES);

    PF_ADD_FLOAT_SLIDERX("Focus (sharp = near)", 0, 100, 0, 100, 60, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_CUE_FOCUS);
    PF_ADD_FLOAT_SLIDERX("Height (low = near)", 0, 100, 0, 100, 30, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_CUE_HEIGHT);
    PF_ADD_FLOAT_SLIDERX("Brightness (bright = near)", 0, 100, 0, 100, 10, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_CUE_BRIGHT);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_TOPIC_CUES_END);

    // --- Output -------------------------------------------------------------------------
    PF_ADD_POPUPX("Background", 3, 1, "Black|Transparent|Dimmed Source", 0, ID_BACKGROUND);

    out_data->num_params = P_NUM_PARAMS;
    return PF_Err_NONE;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

static ascii::ImageView viewOf(const PF_LayerDef& w)
{
    ascii::ImageView v;
    v.data = reinterpret_cast<const uint8_t*>(w.data);
    v.width = w.width;
    v.height = w.height;
    v.rowBytes = static_cast<size_t>(w.rowbytes);
    v.format = PF_WORLD_IS_DEEP(&w) ? ascii::PixelFormat::ARGB16 : ascii::PixelFormat::ARGB8;
    return v;
}

static ascii::Settings readSettings(PF_ParamDef* params[], double resolutionScale)
{
    ascii::Settings s;

    // Cell size is authored in full-resolution pixels; scale it for half/quarter previews
    // so the look doesn't change when you drop resolution.
    s.cellWidth = static_cast<float>(params[P_CELL_SIZE]->u.fs_d.value * resolutionScale);
    s.cellAspect = static_cast<float>(params[P_CELL_ASPECT]->u.fs_d.value);
    s.charSet = static_cast<ascii::CharSet>(params[P_CHARSET]->u.pd.value - 1);
    s.invertChars = params[P_INVERT_CHARS]->u.bd.value != 0;

    s.brightness = static_cast<float>(params[P_BRIGHTNESS]->u.fs_d.value / 100.0);
    s.contrast = static_cast<float>(params[P_CONTRAST]->u.fs_d.value / 100.0);
    s.gamma = static_cast<float>(params[P_GAMMA]->u.fs_d.value);

    s.depthSource = static_cast<ascii::DepthSource>(params[P_DEPTH_SOURCE]->u.pd.value - 1);
    s.invertDepth = params[P_INVERT_DEPTH]->u.bd.value != 0;
    s.normalizeDepth = params[P_NORMALIZE_DEPTH]->u.bd.value != 0;
    s.depthContrast = static_cast<float>(params[P_DEPTH_CONTRAST]->u.fs_d.value / 100.0);
    s.depthSmoothing = static_cast<float>(params[P_DEPTH_SMOOTHING]->u.fs_d.value);
    s.nearLevel = static_cast<float>(params[P_NEAR_LEVEL]->u.fs_d.value / 100.0);
    s.farLevel = static_cast<float>(params[P_FAR_LEVEL]->u.fs_d.value / 100.0);

    s.focusWeight = static_cast<float>(params[P_CUE_FOCUS]->u.fs_d.value / 100.0);
    s.heightWeight = static_cast<float>(params[P_CUE_HEIGHT]->u.fs_d.value / 100.0);
    s.brightWeight = static_cast<float>(params[P_CUE_BRIGHT]->u.fs_d.value / 100.0);

    s.background = static_cast<ascii::Background>(params[P_BACKGROUND]->u.pd.value - 1);
    return s;
}

static PF_Err Render(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    PF_Err err = PF_Err_NONE;

    const double resolutionScale =
        in_data->downsample_x.den ? static_cast<double>(in_data->downsample_x.num) / in_data->downsample_x.den : 1.0;
    const ascii::Settings settings = readSettings(params, resolutionScale);

    const PF_LayerDef& srcWorld = params[P_INPUT]->u.ld;
    const ascii::ImageView source = viewOf(srcWorld);

    ascii::OutputView dest;
    dest.data = reinterpret_cast<uint8_t*>(output->data);
    dest.width = output->width;
    dest.height = output->height;
    dest.rowBytes = static_cast<size_t>(output->rowbytes);
    dest.format = PF_WORLD_IS_DEEP(output) ? ascii::PixelFormat::ARGB16 : ascii::PixelFormat::ARGB8;

    ascii::Inputs inputs;
    inputs.source = &source;

    int cellW = 0, cellH = 0;
    ascii::CellPixelSize(settings, cellW, cellH);
    inputs.atlas = ascii::GetAtlas(settings.charSet, cellW, cellH);

    // Depth layer: check it out at the current time (and check it back in below).
    PF_ParamDef depthCheckout;
    AEFX_CLR_STRUCT(depthCheckout);
    bool checkedOut = false;
    ascii::ImageView depthView;

    if (settings.depthSource == ascii::DepthSource::Layer)
    {
        err = PF_CHECKOUT_PARAM(in_data,
                                P_DEPTH_LAYER,
                                in_data->current_time,
                                in_data->time_step,
                                in_data->time_scale,
                                &depthCheckout);
        if (!err)
        {
            checkedOut = true;
            if (depthCheckout.u.ld.data)
            {
                depthView = viewOf(depthCheckout.u.ld);
                inputs.depthLayer = &depthView;
            }
            // else: no layer chosen -> core falls back to the auto estimate.
        }
    }

    ascii::FloatMap subjectMask;
    if (!err && settings.depthSource == ascii::DepthSource::Subject)
    {
        if (ascii::ComputeSubjectMask(source, subjectMask))
            inputs.subjectMask = &subjectMask;
        // else: nothing found / Vision unavailable -> auto estimate.
    }

    if (!err)
        ascii::Render(inputs, settings, dest);

    if (checkedOut)
    {
        PF_Err checkinErr = PF_CHECKIN_PARAM(in_data, &depthCheckout);
        if (!err)
            err = checkinErr;
    }

    return err;
}

// ---------------------------------------------------------------------------
// Registration and dispatch
// ---------------------------------------------------------------------------

extern "C" DllExport PF_Err PluginDataEntryFunction2(
    PF_PluginDataPtr inPtr,
    PF_PluginDataCB2 inPluginDataCallBackPtr,
    SPBasicSuite* inSPBasicSuitePtr,
    const char* inHostName,
    const char* inHostVersion)
{
    PF_Err result = PF_Err_INVALID_CALLBACK;

    result = PF_REGISTER_EFFECT_EXT2(inPtr,
                                     inPluginDataCallBackPtr,
                                     ASCIIDEPTH_NAME,        // Name
                                     ASCIIDEPTH_MATCH_NAME,  // Match Name
                                     ASCIIDEPTH_CATEGORY,    // Category
                                     AE_RESERVED_INFO,       // Reserved Info
                                     "EffectMain",           // Entry point
                                     "");                    // support URL

    return result;
}

PF_Err EffectMain(
    PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output, void* extra)
{
    PF_Err err = PF_Err_NONE;

    try
    {
        switch (cmd)
        {
        case PF_Cmd_ABOUT:
            err = About(in_data, out_data, params, output);
            break;

        case PF_Cmd_GLOBAL_SETUP:
            err = GlobalSetup(in_data, out_data, params, output);
            break;

        case PF_Cmd_PARAMS_SETUP:
            err = ParamsSetup(in_data, out_data, params, output);
            break;

        case PF_Cmd_RENDER:
            err = Render(in_data, out_data, params, output);
            break;
        }
    }
    catch (PF_Err& thrown_err)
    {
        err = thrown_err;
    }
    catch (const std::bad_alloc&)
    {
        err = PF_Err_OUT_OF_MEMORY;
    }
    catch (const std::exception&)
    {
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return err;
}
