#pragma once
#include "../features.h"

namespace features
{
	class c_menu
	{
	public:
		void process( SDK::UCanvas* canvas );
		void shutdown( );

		bool open = false;
		bool esp = true;
		bool box = false;
		bool skeleton = true;
		bool health = true;
		bool visibility_check = true;
		bool team_check = true;

		// Search radius (pixels from the crosshair) for esp's bestTarget.
		float fov = 200.f;

		// Aim move: while on, the view is pointed straight at the best
		// target's head. No speed or smoothing.
		bool aim_move = false;

		bool no_recoil = false;
		bool no_shake = false;
		bool no_flash = false;
		bool god_mode = false;

		bool view_fov = false;
		float view_fov_value = 120.f;

		bool gravity = false;
		float gravity_value = 1.f;

		// Weapon (all written to the local player's WEP / its Wep_Info)
		bool unlimited_ammo = false;
		bool rapid_fire = false;
		float rapid_fire_value = 0.11f;
		bool full_auto = false;
		bool instant_kill = false;
		bool insta_aim = false;
		bool no_aim_restrictions = false;

		// Aim extras
		bool aim_line = false;
		bool aim_chest = false;

		// World ESP
		bool drone_esp = false;
		bool bomb_esp = false;

	private:
		void handle_input( );
		void draw_watermark( SDK::UCanvas* canvas );
		void draw_menu( SDK::UCanvas* canvas );
		void free_mouse( );
		void capture_mouse( );

		int dragging_row = -1;

		bool mouse_freed = false;
		float mouse_x = 0.f;
		float mouse_y = 0.f;
		int hovered_item = -1;
		int fps = 0;
	};
	inline const std::unique_ptr< c_menu > menu = std::make_unique< c_menu >( );
}
