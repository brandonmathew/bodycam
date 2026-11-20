#pragma once
#include "../features.h"

namespace features
{
	class c_aim
	{
	public:
		// Basic aim: while Aim move is on and the menu is closed, point
		// the view straight at the best target's head by setting the
		// control rotation. No smoothing of any kind, no aim key.
		void process( SDK::UCanvas* canvas );
	};
	inline const std::unique_ptr< c_aim > aim = std::make_unique< c_aim >( );
}
