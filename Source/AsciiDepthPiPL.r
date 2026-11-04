#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
	#include <AE_General.r>
#endif

// Keep in sync with AsciiDepth.h / GlobalSetup() in AsciiDepth.cpp:
//   Name / Category / Match Name
//   AE_Effect_Version = PF_VERSION(1,0,0,DEVELOP,1) = 524289
//   Global OutFlags   = PF_OutFlag_DEEP_COLOR_AWARE            (0x02000000)
//   Global OutFlags 2 = PF_OutFlag2_SUPPORTS_THREADED_RENDERING (0x08000000)

resource 'PiPL' (16000) {
	{
		Kind {
			AEEffect
		},
		Name {
			"ASCII Depth"
		},
		Category {
			"Stylize"
		},
#ifdef AE_OS_WIN
    #if defined(AE_PROC_INTELx64)
		CodeWin64X86 {"EffectMain"},
    #elif defined(AE_PROC_ARM64)
		CodeWinARM64 {"EffectMain"},
    #endif
#elif defined(AE_OS_MAC)
		CodeMacIntel64 {"EffectMain"},
		CodeMacARM64 {"EffectMain"},
#endif
		AE_PiPL_Version {
			2,
			0
		},
		AE_Effect_Spec_Version {
			PF_PLUG_IN_VERSION,
			PF_PLUG_IN_SUBVERS
		},
		AE_Effect_Version {
			524289	/* 1.0 */
		},
		AE_Effect_Info_Flags {
			0
		},
		AE_Effect_Global_OutFlags {
			0x02000000
		},
		AE_Effect_Global_OutFlags_2 {
			0x08000000
		},
		AE_Effect_Match_Name {
			"NSDEV ASCII Depth"
		},
		AE_Reserved_Info {
			0
		},
		AE_Effect_Support_URL {
			""
		}
	}
};
