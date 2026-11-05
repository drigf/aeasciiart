// AsciiDepth.cpp
//
// After Effects entry points, parameter definitions and the render hand-off.
// All the interesting pixel work is in Core/AsciiCore.cpp.

#include "AsciiDepth.h"

#include <exception>
#include <new>

#include <cstdio>
#include <cstring>
#include <string>

#include "Core/AsciiCore.h"
#include "TextPrompt.h"

// ---------------------------------------------------------------------------
// Custom characters: stored in a hidden arbitrary-data parameter
// ---------------------------------------------------------------------------

namespace
{

// Fixed-size and pointer-free, so flattening for the project file is a memcpy.
struct CustomText
{
    char utf8[ascii::kMaxCustomBytes + 1];
};

const char* const kDefaultCustomChars = " .:-=+*#%@";
const char* const kDefaultMessage = "ASCII DEPTH ";
constexpr A_long kCustomSetChoice = 5;  // 1-based popup index of "Custom"
constexpr A_long kMessageModeChoice = 2;  // 1-based popup index of "Message" in Text Mode

// Cuts `s` to at most `maxBytes` without splitting a UTF-8 sequence.
void clampUtf8(std::string& s, size_t maxBytes)
{
    if (s.size() <= maxBytes)
        return;
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
        --cut;
    s.resize(cut);
}

PF_Handle newArb(PF_InData* in_data, const char* text, size_t len)
{
    PF_Handle h = PF_NEW_HANDLE(sizeof(CustomText));
    if (!h)
        return nullptr;
    CustomText* t = static_cast<CustomText*>(PF_LOCK_HANDLE(h));
    std::memset(t, 0, sizeof(CustomText));
    len = len < ascii::kMaxCustomBytes ? len : ascii::kMaxCustomBytes;
    if (text)
        std::memcpy(t->utf8, text, len);
    PF_UNLOCK_HANDLE(h);
    return h;
}

std::string readArb(PF_InData* in_data, PF_ArbitraryH arb)
{
    if (!arb)
        return std::string();
    PF_Handle h = reinterpret_cast<PF_Handle>(arb);
    const CustomText* t = static_cast<const CustomText*>(PF_LOCK_HANDLE(h));
    std::string out;
    if (t)
    {
        const size_t n = strnlen(t->utf8, sizeof(t->utf8));
        out.assign(t->utf8, n);
    }
    PF_UNLOCK_HANDLE(h);
    return out;
}

}  // namespace

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
    out_data->out_flags = PF_OutFlag_DEEP_COLOR_AWARE |        // 8 and 16 bpc
                          PF_OutFlag_SEND_UPDATE_PARAMS_UI;    // to show the custom characters on the button
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
    PF_ADD_POPUPX("Character Set", 5, 2, "Classic|Simple|Binary|Blocks|Custom", 0, ID_CHARSET);

    // "Custom" character set: a button that opens a dialog, backed by a hidden arbitrary param.
    PF_ADD_BUTTON("Custom Characters", "Edit...", 0, PF_ParamFlag_SUPERVISE, ID_CUSTOM_BTN);

    AEFX_CLR_STRUCT(def);
    {
        const size_t n = std::strlen(kDefaultCustomChars);
        PF_ArbitraryH dflt = reinterpret_cast<PF_ArbitraryH>(newArb(in_data, kDefaultCustomChars, n));
        if (!dflt)
            return PF_Err_OUT_OF_MEMORY;
        PF_ADD_ARBITRARY2("Custom Characters Data", 0, 0, PF_ParamFlag_CANNOT_TIME_VARY, PF_PUI_NO_ECW_UI | PF_PUI_INVISIBLE, dflt, ID_CUSTOM_TEXT, nullptr);
    }

    PF_ADD_POPUPX("Text Mode", 2, 1, "Density Ramp|Message", 0, ID_TEXT_MODE);
    PF_ADD_BUTTON("Message", "Edit...", 0, PF_ParamFlag_SUPERVISE, ID_MESSAGE_BTN);

    AEFX_CLR_STRUCT(def);
    {
        PF_ArbitraryH dflt = reinterpret_cast<PF_ArbitraryH>(newArb(in_data, kDefaultMessage, std::strlen(kDefaultMessage)));
        if (!dflt)
            return PF_Err_OUT_OF_MEMORY;
        PF_ADD_ARBITRARY2("Message Data", 0, 0, PF_ParamFlag_CANNOT_TIME_VARY, PF_PUI_NO_ECW_UI | PF_PUI_INVISIBLE, dflt, ID_MESSAGE_TEXT, nullptr);
    }

    PF_ADD_POPUPX("Message: Solid Fill", 4, 2, "None|Bright Areas|Dark Areas|Near (Depth)", 0, ID_FILL);
    PF_ADD_FLOAT_SLIDERX("Fill Threshold", 0, 100, 0, 100, 50, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_FILL_THRESH);
    PF_ADD_SLIDER("Message Scroll", -10000, 10000, -200, 200, 0, ID_SCROLL);

    PF_ADD_POPUPX("Font",
                  ascii::kFontCount,
                  1,
                  "Menlo|Menlo Bold|Monaco|Courier New|Courier New Bold|Andale Mono|Helvetica Neue Bold",
                  0,
                  ID_FONT);
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

static ascii::Settings readSettings(PF_InData* in_data, PF_ParamDef* params[], double resolutionScale)
{
    ascii::Settings s;

    // Cell size is authored in full-resolution pixels; scale it for half/quarter previews
    // so the look doesn't change when you drop resolution.
    s.cellWidth = static_cast<float>(params[P_CELL_SIZE]->u.fs_d.value * resolutionScale);
    s.cellAspect = static_cast<float>(params[P_CELL_ASPECT]->u.fs_d.value);
    s.charSet = static_cast<ascii::CharSet>(params[P_CHARSET]->u.pd.value - 1);
    s.invertChars = params[P_INVERT_CHARS]->u.bd.value != 0;
    s.font = params[P_FONT]->u.pd.value - 1;
    if (s.charSet == ascii::CharSet::Custom)
        s.customChars = readArb(in_data, params[P_CUSTOM_TEXT]->u.arb_d.value);

    s.messageMode = params[P_TEXT_MODE]->u.pd.value == kMessageModeChoice;
    if (s.messageMode)
        s.message = readArb(in_data, params[P_MESSAGE_TEXT]->u.arb_d.value);
    s.fillMode = params[P_FILL]->u.pd.value - 1;
    s.fillThreshold = static_cast<float>(params[P_FILL_THRESH]->u.fs_d.value / 100.0);
    s.scroll = params[P_SCROLL]->u.sd.value;

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
    const ascii::Settings settings = readSettings(in_data, params, resolutionScale);

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
    inputs.atlas = settings.messageMode ? ascii::GetMessageAtlas(settings.font, settings.message, cellW, cellH)
                                        : ascii::GetAtlas(settings.charSet, settings.font, settings.customChars, cellW, cellH);

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
// Custom characters: button, label, and arbitrary-data callbacks
// ---------------------------------------------------------------------------

// Opens the text dialog for one of the two button params, stores the result in its arbitrary
// param, and switches the matching mode popup so the new text is actually used.
static PF_Err EditText(PF_InData* in_data,
                       PF_OutData* out_data,
                       PF_ParamDef* params[],
                       int dataIndex,
                       int modeIndex,
                       A_long modeChoice,
                       bool isMessage)
{
    const std::string current = readArb(in_data, params[dataIndex]->u.arb_d.value);
    std::string typed;
    const bool ok =
        isMessage ? PromptForText("Message",
                                  "The message repeats in reading order across the whole frame, like lines of text. "
                                  "Include a space at the end if you want a gap before it repeats. Solid fill blocks "
                                  "(see \"Message: Solid Fill\") cut the subject out of the text.",
                                  current,
                                  typed)
                  : PromptForCharacters(current, typed);
    if (!ok)
        return PF_Err_NONE;  // cancelled

    clampUtf8(typed, ascii::kMaxCustomBytes);

    PF_Handle h = reinterpret_cast<PF_Handle>(params[dataIndex]->u.arb_d.value);
    if (!h)
        return PF_Err_NONE;
    CustomText* t = static_cast<CustomText*>(PF_LOCK_HANDLE(h));
    std::memset(t, 0, sizeof(CustomText));
    std::memcpy(t->utf8, typed.data(), typed.size());
    PF_UNLOCK_HANDLE(h);

    params[dataIndex]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;

    params[modeIndex]->u.pd.value = modeChoice;
    params[modeIndex]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
    if (isMessage)
    {
        // Message mode ignores the character set; nothing else to switch.
    }

    out_data->out_flags |= PF_OutFlag_FORCE_RERENDER;
    return PF_Err_NONE;
}

static PF_Err UserChangedParam(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], void* extra)
{
    const PF_UserChangedParamExtra* changed = static_cast<const PF_UserChangedParamExtra*>(extra);
    if (!changed)
        return PF_Err_NONE;
    if (FILE* log = fopen("/tmp/asciidepth.log", "a"))
    {
        fprintf(log, "USER_CHANGED_PARAM index=%d (custom btn=%d, message btn=%d)\n", int(changed->param_index), int(P_CUSTOM_BTN), int(P_MESSAGE_BTN));
        fclose(log);
    }

    if (changed->param_index == P_CUSTOM_BTN)
    {
        PF_Err err = EditText(in_data, out_data, params, P_CUSTOM_TEXT, P_CHARSET, kCustomSetChoice, false);
        if (!err)
        {
            // Using custom characters means ramp mode.
            params[P_TEXT_MODE]->u.pd.value = 1;
            params[P_TEXT_MODE]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
        }
        return err;
    }
    if (changed->param_index == P_MESSAGE_BTN)
        return EditText(in_data, out_data, params, P_MESSAGE_TEXT, P_TEXT_MODE, kMessageModeChoice, true);
    return PF_Err_NONE;
}

// Puts the current text in each button's label so it's visible in the panel.
static PF_Err UpdateParamsUI(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[])
{
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    PF_Err err = PF_Err_NONE;

    struct Item { int button; int data; const char* prefix; };
    const Item items[] = {{P_CUSTOM_BTN, P_CUSTOM_TEXT, "Chars: "}, {P_MESSAGE_BTN, P_MESSAGE_TEXT, "Msg: "}};
    for (const Item& item : items)
    {
        std::string label = item.prefix + readArb(in_data, params[item.data]->u.arb_d.value);
        clampUtf8(label, PF_MAX_EFFECT_PARAM_NAME_LEN);

        PF_ParamDef def = *params[item.button];
        std::memset(def.PF_DEF_NAME, 0, sizeof(def.PF_DEF_NAME));
        std::memcpy(def.PF_DEF_NAME, label.data(), label.size());
        if (!err)
            err = suites.ParamUtilsSuite3()->PF_UpdateParamUI(in_data->effect_ref, item.button, &def);
    }
    return err;
}

static PF_Err ArbitraryCallback(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], void* extra)
{
    PF_ArbParamsExtra* arb = static_cast<PF_ArbParamsExtra*>(extra);
    if (!arb)
        return PF_Err_NONE;

    switch (arb->which_function)
    {
    case PF_Arbitrary_NEW_FUNC:
    {
        const char* dflt = arb->id == ID_MESSAGE_TEXT ? kDefaultMessage : kDefaultCustomChars;
        PF_Handle h = newArb(in_data, dflt, std::strlen(dflt));
        if (!h)
            return PF_Err_OUT_OF_MEMORY;
        *arb->u.new_func_params.arbPH = reinterpret_cast<PF_ArbitraryH>(h);
        break;
    }
    case PF_Arbitrary_DISPOSE_FUNC:
        if (arb->u.dispose_func_params.arbH)
            PF_DISPOSE_HANDLE(arb->u.dispose_func_params.arbH);
        break;

    case PF_Arbitrary_COPY_FUNC:
    {
        const std::string text = readArb(in_data, arb->u.copy_func_params.src_arbH);
        PF_Handle h = newArb(in_data, text.data(), text.size());
        if (!h)
            return PF_Err_OUT_OF_MEMORY;
        *arb->u.copy_func_params.dst_arbPH = reinterpret_cast<PF_ArbitraryH>(h);
        break;
    }
    case PF_Arbitrary_FLAT_SIZE_FUNC:
        *arb->u.flat_size_func_params.flat_data_sizePLu = sizeof(CustomText);
        break;

    case PF_Arbitrary_FLATTEN_FUNC:
    {
        const std::string text = readArb(in_data, arb->u.flatten_func_params.arbH);
        if (arb->u.flatten_func_params.buf_sizeLu < sizeof(CustomText))
            return PF_Err_INTERNAL_STRUCT_DAMAGED;
        CustomText* flat = static_cast<CustomText*>(arb->u.flatten_func_params.flat_dataPV);
        std::memset(flat, 0, sizeof(CustomText));
        std::memcpy(flat->utf8, text.data(), text.size());
        break;
    }
    case PF_Arbitrary_UNFLATTEN_FUNC:
    {
        // Treat the buffer defensively: it comes from a file.
        const size_t avail = arb->u.unflatten_func_params.buf_sizeLu < sizeof(CustomText)
                                 ? arb->u.unflatten_func_params.buf_sizeLu
                                 : sizeof(CustomText);
        const char* src = static_cast<const char*>(arb->u.unflatten_func_params.flat_dataPV);
        PF_Handle h = newArb(in_data, src, strnlen(src, avail));
        if (!h)
            return PF_Err_OUT_OF_MEMORY;
        *arb->u.unflatten_func_params.arbPH = reinterpret_cast<PF_ArbitraryH>(h);
        break;
    }
    case PF_Arbitrary_INTERP_FUNC:
    {
        // Text can't be tweened; hold the left value (the param can't be keyframed anyway).
        const std::string text = readArb(in_data, arb->u.interp_func_params.left_arbH);
        PF_Handle h = newArb(in_data, text.data(), text.size());
        if (!h)
            return PF_Err_OUT_OF_MEMORY;
        *arb->u.interp_func_params.interpPH = reinterpret_cast<PF_ArbitraryH>(h);
        break;
    }
    case PF_Arbitrary_COMPARE_FUNC:
        *arb->u.compare_func_params.compareP =
            readArb(in_data, arb->u.compare_func_params.a_arbH) == readArb(in_data, arb->u.compare_func_params.b_arbH)
                ? PF_ArbCompare_EQUAL
                : PF_ArbCompare_NOT_EQUAL;
        break;

    case PF_Arbitrary_PRINT_SIZE_FUNC:
        *arb->u.print_size_func_params.print_sizePLu =
            static_cast<A_u_long>(readArb(in_data, arb->u.print_size_func_params.arbH).size() + 1);
        break;

    case PF_Arbitrary_PRINT_FUNC:
    {
        const std::string text = readArb(in_data, arb->u.print_func_params.arbH);
        const A_u_long cap = arb->u.print_func_params.print_sizeLu;
        if (cap > 0 && arb->u.print_func_params.print_bufferPC)
        {
            const size_t n = text.size() < cap - 1 ? text.size() : cap - 1;
            std::memcpy(arb->u.print_func_params.print_bufferPC, text.data(), n);
            arb->u.print_func_params.print_bufferPC[n] = '\0';
        }
        break;
    }
    case PF_Arbitrary_SCAN_FUNC:
    {
        PF_Handle h = newArb(in_data, arb->u.scan_func_params.bufPC, arb->u.scan_func_params.bytes_to_scanLu);
        if (!h)
            return PF_Err_OUT_OF_MEMORY;
        *arb->u.scan_func_params.arbPH = reinterpret_cast<PF_ArbitraryH>(h);
        break;
    }
    default:
        break;
    }
    return PF_Err_NONE;
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

        case PF_Cmd_USER_CHANGED_PARAM:
            err = UserChangedParam(in_data, out_data, params, extra);
            break;

        case PF_Cmd_UPDATE_PARAMS_UI:
            err = UpdateParamsUI(in_data, out_data, params);
            break;

        case PF_Cmd_ARBITRARY_CALLBACK:
            err = ArbitraryCallback(in_data, out_data, params, extra);
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
