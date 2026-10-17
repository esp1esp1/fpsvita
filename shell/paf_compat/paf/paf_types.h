/*
	ScePaf-RE paf/paf_types.h with GCC fixes: __declspec as a no-op,
	SCE_CDECL_BEGIN/END, and an off_t guard against newlib's.
*/

#ifndef _VDSUITE_USER_PAF_PAF_TYPES_H
#define _VDSUITE_USER_PAF_PAF_TYPES_H

#include <stdint.h>
#include <stddef.h>

/* __declspec is MSVC-only */
#ifdef __GNUC__
# ifndef __declspec
#  define __declspec(x)
# endif
#endif

/* PSP2 SDK compat macros */
#ifdef __cplusplus
# ifndef SCE_CDECL_BEGIN
#  define SCE_CDECL_BEGIN extern "C" {
#  define SCE_CDECL_END   }
# endif
#else
# ifndef SCE_CDECL_BEGIN
#  define SCE_CDECL_BEGIN
#  define SCE_CDECL_END
# endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _OFF_T_DECLARED
typedef int64_t off_t;
# define _OFF_T_DECLARED
#endif

typedef enum ImageMode {
	ImageMode_NONE = -1,
	ImageMode_XBGR0565 = 0,
	ImageMode_ABGR1555 = 1,
	ImageMode_ABGR4444 = 2,
	ImageMode_ABGR8888 = 3,		//unsupported
	ImageMode_RGBA8888 = 4,
	ImageMode_ARGB8888 = 5,
	ImageMode_BGRA8888 = 6,
	ImageMode_WZYX16FP = 7,		//unsupported
	ImageMode_WZYX32FP = 8,		//unsupported
	ImageMode_XCRYCB8888 = 9,	//unsupported
	ImageMode_CRYCBX8888 = 10,	//unsupported
	ImageMode_XCRYCB16FP = 11,	//unsupported
	ImageMode_RGBX16INT = 12,	//unsupported
	ImageMode_CRYCBX16INT = 13,	//unsupported
	ImageMode_A8 = 14,
	ImageMode_L8 = 15,
	ImageMode_DXT1 = 16,
	ImageMode_DXT2 = 17,		//unsupported
	ImageMode_DXT3 = 18,
	ImageMode_DXT4 = 19,		//unsupported
	ImageMode_DXT5 = 20,
	ImageMode_index4 = 21,
	ImageMode_index8 = 22,
	ImageMode_index16 = 23,		//unsupported
	ImageMode_index32 = 24,		//unsupported
	ImageMode_BGR888 = 25,		//unsupported
	ImageMode_RGB888 = 26,		//unsupported
	ImageMode_YUV420P2 = 27,
	ImageMode_YUV420P3 = 28,
	ImageMode_PVRT2BPP = 29,
	ImageMode_PVRT4BPP = 30,
	ImageMode_PVRTII2BPP = 31,
	ImageMode_PVRTII4BPP = 32,
	ImageMode_depth16 = 100,
	ImageMode_depth32 = 101,
	ImageMode_A16FP = 102,
	ImageMode_A32FP = 103
} ImageMode;

typedef enum Locale {
	Locale_Default = -1,
	Locale_JP = 0,
	Locale_MIN = 0,
	Locale_EN = 1,
	Locale_FR = 2,
	Locale_ES = 3,
	Locale_DE = 4,
	Locale_IT = 5,
	Locale_NL = 6,
	Locale_PT = 7,
	Locale_RU = 8,
	Locale_KO = 9,
	Locale_ZH_T = 10,
	Locale_ZH_S = 11,
	Locale_FI = 12,
	Locale_SV = 13,
	Locale_DA = 14,
	Locale_NO = 15,
	Locale_PL = 16,
	Locale_TR = 17,
	Locale_MAX = 17,
} Locale;

typedef enum ResolutionId {
	ResolutionId_Landscape_Padkey_Headside = 0,
	ResolutionId_Landscape_Padkey_Footside = 1,
	ResolutionId_Portrait_Padkey_Leftside = 2,
	ResolutionId_Portrait_Padkey_Rightside = 3,
	ResolutionId_HD_Full = 4,
	ResolutionId_HD_Half = 5,
	ResolutionId_SD_NTSC = 6,
	ResolutionId_SD_PAL = 7,
	ResolutionId_PSP = 8,
	ResolutionId_END = 9
} ResolutionId;

typedef enum ResolutionType {
	ResolutionType_Landscape = 9,
	ResolutionType_Portrait = 10,
	ResolutionType_HD = 11,
	ResolutionType_SD = 12,
	ResolutionType_PSP = 13
} ResolutionType;

typedef enum OrientationType {
	OrientationType_Standard = 0,
	OrientationType_Upsidedown = 1,
	OrientationType_Rightsidedown = 2,
	OrientationType_Leftsidedown = 3,
	OrientationType_Auto = 4
} OrientationType;

typedef enum ImageOrder {
	ImageOrder_NONE = -1,
	ImageOrder_Linear = 1,
	ImageOrder_Normal = 1,
	ImageOrder_Swizzle = 2,
	ImageOrder_Tiled = 3
} ImageOrder;

typedef enum CharEncoding
{
	CharEncoding_AllowIllegalCode = 0x2,
	CharEncoding_AllowOutOfCodeRange = 0x4,
	CharEncoding_AllowInvalidEncode = 0x8,
	CharEncoding_AllowSrcBufferEnd = 0x10
} CharEncoding;

#define SCE_PAF_OK	(0)

#ifdef __cplusplus
}
#endif

#endif /* _VDSUITE_USER_PAF_PAF_TYPES_H */