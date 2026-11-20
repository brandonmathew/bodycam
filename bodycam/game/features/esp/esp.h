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

	private:
		// Steers the mouse toward the target point while Aim move is on,
		// the menu is closed and the aim key (right mouse) is held.
		// Velocity-based smoothing: the mouse accelerates toward the
		// speed that would arrive in aim_smooth seconds and eases out
		// as the remaining distance shrinks; all math runs on real
		// frame time, so it feels the same at any frame rate.
		void apply_aim( SDK::UCanvas* canvas, const SDK::FVector2D& target );
		float aim_vel_x = 0.f;
		float aim_vel_y = 0.f;
		float aim_remainder_x = 0.f;
		float aim_remainder_y = 0.f;
		ULONGLONG aim_last_tick = 0;
		SDK::AALS_Base_CharacterBP_C* aim_target = nullptr;
	};
	inline const std::unique_ptr< c_esp > esp = std::make_unique< c_esp >( );
}