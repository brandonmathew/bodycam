#pragma once
#include "../features.h"

namespace features
{
	class c_misc
	{
	public:
		// No recoil: keeps the local player's weapon recoil state zeroed
		// and its recoil timeline stopped. No shake: stops all camera
		// shakes on the local camera manager every frame.
		void process( );
	};
	inline const std::unique_ptr< c_misc > misc = std::make_unique< c_misc >( );
}
