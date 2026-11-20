#include "aim.h"

static SDK::APlayerController* get_local_controller( )
{
	SDK::UWorld* const world = SDK::UWorld::GetWorld( );
	if ( !world || !world->OwningGameInstance || world->OwningGameInstance->LocalPlayers.Num( ) == 0 )
		return nullptr;

	SDK::ULocalPlayer* const local_player = world->OwningGameInstance->LocalPlayers[ 0 ];
	if ( !local_player )
		return nullptr;

	return local_player->PlayerController;
}

void features::c_aim::process( SDK::UCanvas* canvas )
{
	if ( !features::menu->aim_move || features::menu->open )
		return;

	if ( !features::esp->current_target )
		return;

	SDK::APlayerController* const controller = get_local_controller( );
	if ( !controller || !controller->PlayerCameraManager )
		return;

	// Point the view straight at the head's world position. Setting the
	// control rotation is exact - unlike moving the mouse, it does not
	// depend on the game's sensitivity, so it lands on the head instead
	// of overshooting and oscillating around it.
	const SDK::FVector camera = controller->PlayerCameraManager->GetCameraLocation( );
	const SDK::FRotator rotation = SDK::UKismetMathLibrary::FindLookAtRotation( camera, features::esp->current_target_head_world );
	controller->SetControlRotation( rotation );
}
