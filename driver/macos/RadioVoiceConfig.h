#pragma once
#include "Identity.h"
// RadioVoice adaptation of BlackHole; upstream source is preserved separately.
#define kDriver_Name RV_AUDIO_NAME
#define kPlugIn_BundleID RV_AUDIO_BUNDLE_ID
#define kPlugIn_Icon "RadioVoice.icns"
#define kHas_Driver_Name_Format 0
#define kDevice_Name RV_AUDIO_NAME
#define kDevice2_Name RV_AUDIO_SEND_NAME
#define kDevice_IsHidden 0
#define kDevice_HasInput 1
#define kDevice_HasOutput 0
#define kDevice2_IsHidden 1
#define kDevice2_HasInput 0
#define kDevice2_HasOutput 1
#define kNumber_Of_Channels 2
#define kSampleRates 48000
#define kManufacturer_Name RV_AUDIO_NAME
#define kCanBeDefaultSystemDevice 0
#define BlackHole_Create RadioVoice_Create
