#pragma once
#include "../features.h"

namespace features
{
	class c_aim
	{
	public:
		// Basic aim: while Aim move is on, the menu is closed and the aim
		// key (right mouse) is held, move the mouse straight onto the
		// best target's head. No smoothing of any kind.
		void process( SDK::UCanvas* canvas );
	};
	inline const std::unique_ptr< c_aim > aim = std::make_unique< c_aim >( );
}
