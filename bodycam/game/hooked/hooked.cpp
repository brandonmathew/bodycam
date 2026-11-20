#include "hooked.h"

using PostRender_t = void( __thiscall* )( SDK::UObject*, SDK::UCanvas* );
PostRender_t o_PostRender;

// Set while the DLL is unloading so the detour stops drawing at once.
static bool g_unloading = false;

void hk_PostRender( SDK::UObject* ViewportClient, SDK::UCanvas* Canvas )
{
	if ( g_unloading )
		return;

	features::esp->process( Canvas );
	features::aim->process( Canvas );
	features::misc->process( );
	features::menu->process( Canvas );
}

void game::c_hooked::apply( )
{
	MH_Initialize( );

	const SDK::UGameViewportClient* viewport_client = SDK::UWorld::GetWorld( )->OwningGameInstance->LocalPlayers[ 0 ]->ViewportClient;
	void* const post_render = reinterpret_cast< void** >( viewport_client->VTable )[ 115 ];
	hook_target = post_render;
	MH_CreateHook( post_render, &hk_PostRender, reinterpret_cast< void** >( &o_PostRender ) );

	MH_EnableHook( MH_ALL_HOOKS );
}

void game::c_hooked::remove( )
{
	// Stop the detour drawing, then take the hook out before the DLL
	// unloads - if the hook outlived the DLL, the game would call into
	// unloaded memory on the next frame and crash.
	g_unloading = true;

	const MH_STATUS disable_status = MH_DisableHook( MH_ALL_HOOKS );
	L_INFO( "MH_DisableHook -> %d", disable_status );

	// Give a PostRender call that is already running time to finish.
	Sleep( 100 );

	if ( hook_target )
	{
		const MH_STATUS remove_status = MH_RemoveHook( hook_target );
		L_INFO( "MH_RemoveHook -> %d", remove_status );
	}

	const MH_STATUS uninit_status = MH_Uninitialize( );
	L_INFO( "MH_Uninitialize -> %d", uninit_status );
}