#pragma once
#include "../features.h"

namespace features
{
	class c_misc
	{
	public:
		// Local-player features: no recoil / weapon mods written to the
		// pawn's WEP and its Wep_Info every frame, no shake, no flash,
		// god mode, view FOV and gravity.
		void process( );
	};
	inline const std::unique_ptr< c_misc > misc = std::make_unique< c_misc >( );
}
