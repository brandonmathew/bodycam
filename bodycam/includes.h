#pragma once
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <thread>
#include <memory>
#include <algorithm>

#include "core/utils/logger/logger.h"
#include "core/bootstrap_cheat.h"

#include "core/thirdparty/minhook/minhook.h"

#include "game/sdk/SDK/Engine_classes.hpp"
#include "game/sdk/SDK/UMG_classes.hpp"
#include "game/sdk/SDK/ALS_Base_CharacterBP_classes.hpp"
#include "game/sdk/SDK/ALS_AnimMan_CharacterBP_classes.hpp"
#include "game/sdk/SDK/WW_SurvivorStatus_classes.hpp"
#include "game/sdk/SDK/AI_Humanoid_classes.hpp"
#include "game/sdk/SDK/PS_Bodycam_classes.hpp"
#include "game/sdk/SDK/WEP_classes.hpp"
#include "game/sdk/SDK/STR_WEP_structs.hpp"
#include "game/sdk/SDK/STR_RECOIL_structs.hpp"
#include "game/sdk/SDK/STR_WEP_ANIMATION_structs.hpp"
#include "game/sdk/SDK/EN_FireMode_structs.hpp"
#include "game/sdk/SDK/BP_Drone_classes.hpp"
#include "game/sdk/SDK/Bombe_classes.hpp"

#include "game/hooked/hooked.h"

#include "game/features/features.h"