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

		// Aim move: while on, the mouse is moved straight onto the best
		// target's head. No speed or smoothing.
		bool aim_move = false;

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
