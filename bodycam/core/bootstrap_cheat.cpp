#include "bootstrap_cheat.h"

void core::c_cheat_bootstrap::run( HMODULE module )
{
#ifdef _DEBUG
	core::logger->alloc_console( "bodycam" );
	L_INFO( "attached" );
#endif
	game::hooked->apply( );

	// Stay alive until END is pressed, then unload the DLL only -
	// the game keeps running.
	while ( !( GetAsyncKeyState( VK_END ) & 0x8000 ) )
		Sleep( 100 );

	L_INFO( "END pressed - unloading" );

	features::menu->shutdown( );
	L_INFO( "menu shut down" );

	game::hooked->remove( );
	L_INFO( "hooks removed" );

	const HWND console_window = GetConsoleWindow( );
	if ( console_window )
		ShowWindow( console_window, SW_HIDE );

	FreeConsole( );
	FreeLibraryAndExitThread( module, 0 );
}