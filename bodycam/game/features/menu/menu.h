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

		// Aim tuning: written to the local player's ALS AnimBP every frame
		// while aim_tuning is on; the game's own values are restored when
		// it is switched off. Sliders seed from the game values on first use.
		bool aim_tuning = false;
		float ads_in = 0.25f;
		float ads_out = 0.25f;
		float interp_speed = 10.f;

	private:
		void handle_input( );
		void apply_aim_tuning( );
		void draw_watermark( SDK::UCanvas* canvas );
		void draw_menu( SDK::UCanvas* canvas );
		void free_mouse( );
		void capture_mouse( );

		int dragging_slider = -1;
		bool sliders_seeded = false;
		bool aim_applied = false;
		SDK::UALS_AnimBP_C* aim_instance = nullptr;
		double orig_ads_in = 0.0;
		double orig_ads_out = 0.0;
		double orig_interp = 0.0;

		bool mouse_freed = false;
		float mouse_x = 0.f;
		float mouse_y = 0.f;
		int hovered_item = -1;
		int fps = 0;
	};
	inline const std::unique_ptr< c_menu > menu = std::make_unique< c_menu >( );
}
