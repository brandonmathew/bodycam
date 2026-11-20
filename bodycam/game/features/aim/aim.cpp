#include "aim.h"

void features::c_aim::process( SDK::UCanvas* canvas )
{
	if ( !features::menu->aim_move || features::menu->open )
		return;

	if ( ( GetAsyncKeyState( VK_RBUTTON ) & 0x8000 ) == 0 )
		return;

	// This frame's best target head, computed by esp's bestTarget.
	const SDK::FVector2D target = features::esp->current_target_head;
	if ( target.X < 0.f )
		return;

	const LONG move_x = static_cast< LONG >( target.X - canvas->ClipX * 0.5f );
	const LONG move_y = static_cast< LONG >( target.Y - canvas->ClipY * 0.5f );
	if ( move_x == 0 && move_y == 0 )
		return;

	INPUT input = { };
	input.type = INPUT_MOUSE;
	input.mi.dx = move_x;
	input.mi.dy = move_y;
	input.mi.dwFlags = MOUSEEVENTF_MOVE;
	SendInput( 1, &input, sizeof( input ) );
}
