#pragma once
#include "../features.h"

namespace features
{
	class c_esp
	{
	public:
		void process( SDK::UCanvas* canvas );

		// Screen-space head position of the enemy closest to the crosshair
		// within the menu FOV radius. Dead, teammates and enemies without
		// line of sight are always skipped. Returns { -1, -1 } when no
		// enemy qualifies; current_target holds the pawn (or nullptr).
		SDK::FVector2D bestTarget( SDK::UCanvas* canvas );
		SDK::AALS_Base_CharacterBP_C* current_target = nullptr;
		SDK::FVector2D current_target_head = { -1.f, -1.f };
		SDK::FVector current_target_head_world = { 0.f, 0.f, 0.f };
	};
	inline const std::unique_ptr< c_esp > esp = std::make_unique< c_esp >( );
}