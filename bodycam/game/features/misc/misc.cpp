#include "misc.h"

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

void features::c_misc::process( )
{
	if ( !features::menu->no_recoil && !features::menu->no_shake )
		return;

	SDK::APlayerController* const controller = get_local_controller( );
	if ( !controller )
		return;

	if ( features::menu->no_shake && controller->PlayerCameraManager )
		controller->PlayerCameraManager->StopAllCameraShakes( true );

	if ( !features::menu->no_recoil || !controller->AcknowledgedPawn )
		return;

	SDK::UWorld* const world = SDK::UWorld::GetWorld( );
	if ( !world || !world->PersistentLevel )
		return;

	// Every weapon the local player carries (all guns derive from AWEP_C;
	// the weapon points back at its character, and its owner matches too).
	for ( SDK::AActor* actor : world->PersistentLevel->Actors )
	{
		if ( !actor || !actor->IsA( SDK::AWEP_C::StaticClass( ) ) )
			continue;

		SDK::AWEP_C* const weapon = static_cast< SDK::AWEP_C* >( actor );
		if ( weapon->As_ALS_Base_Character_BP != controller->AcknowledgedPawn && weapon->GetOwner( ) != controller->AcknowledgedPawn )
			continue;

		weapon->RecoilRot = { 0.0, 0.0, 0.0 };
		weapon->RecoilLoc = { 0.0, 0.0, 0.0 };
		weapon->RecoilRandomloc = { 0.0, 0.0, 0.0 };
		weapon->RecoilRandomRot = { 0.0, 0.0, 0.0 };
		weapon->Recoil_WepRotation = { 0.0, 0.0, 0.0 };

		if ( weapon->Recoil_Timeline )
			weapon->Recoil_Timeline->Stop( );
	}
}
