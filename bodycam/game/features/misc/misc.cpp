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
	const bool any_weapon = features::menu->no_recoil || features::menu->unlimited_ammo || features::menu->rapid_fire
		|| features::menu->full_auto || features::menu->instant_kill || features::menu->insta_aim;
	const bool any_local = any_weapon || features::menu->no_shake || features::menu->no_flash || features::menu->god_mode
		|| features::menu->view_fov || features::menu->gravity || features::menu->no_aim_restrictions;
	if ( !any_local )
		return;

	SDK::APlayerController* const controller = get_local_controller( );
	if ( !controller || !controller->AcknowledgedPawn )
		return;

	SDK::AALS_Base_CharacterBP_C* const pawn = static_cast< SDK::AALS_Base_CharacterBP_C* >( controller->AcknowledgedPawn );
	// The playable pawn is the AnimMan subclass; it carries the camera,
	// the aim-block flag and the flashbang ear component.
	SDK::AALS_AnimMan_CharacterBP_C* const anim = static_cast< SDK::AALS_AnimMan_CharacterBP_C* >( controller->AcknowledgedPawn );
	SDK::AWEP_C* const weapon = pawn->WEP;

	if ( features::menu->no_shake && controller->PlayerCameraManager )
		controller->PlayerCameraManager->StopAllCameraShakes( true );

	if ( weapon )
	{
		// The full no-recoil set: current recoil state, aim/lean pose
		// vectors, the weapon shake state, spread growth and the camera
		// shake amount in the weapon's recoil info.
		if ( features::menu->no_recoil )
		{
			const SDK::FVector zero = { 0.0, 0.0, 0.0 };
			weapon->RecoilRot = zero;
			weapon->RecoilLoc = zero;
			weapon->RecoilRandomloc = zero;
			weapon->RecoilRandomRot = zero;
			weapon->Recoil_WepRotation = zero;
			weapon->Aim_Rot = zero;
			weapon->Aim_Loc = zero;
			weapon->LeanSwitchLoc = zero;
			weapon->LeanSwitchRot = zero;
			weapon->ShakeCurrent = zero;
			weapon->SmoothRotIdle = 0.0;
			weapon->Spread_Multiplier = 0.0;
			weapon->Wep_Info.RecoilInfo_14_2F550DD548C99F85FB2D1C911E29083C.CameraShake_13_B0DD31C04CD0D1983602ADB8F281516E = 0.0;
		}

		if ( features::menu->unlimited_ammo )
			weapon->BulletInChamber = true;

		if ( features::menu->rapid_fire )
			weapon->Wep_Info.RecoilInfo_14_2F550DD548C99F85FB2D1C911E29083C.FireRate_11_A93DD4B84C6696E6E0EC3BA044512B73 = features::menu->rapid_fire_value;

		if ( features::menu->full_auto )
			weapon->Wep_Info.FireMode_53_E7AEF4D24EEC5420D75555A92E651905 = SDK::EN_FireMode::NewEnumerator1;

		if ( features::menu->instant_kill )
			weapon->Wep_Info.Damage_17_7F60A88D435DBD7B36C96893C2118EAC = 999.0;

		if ( features::menu->insta_aim )
		{
			weapon->Wep_Info.Animation_23_574905EC43F77C89F6A965AFE119106B.ADSInTIme_39_E74D36F9478596C143C5C0A4B3A694F9 = 0.03;
			weapon->Wep_Info.Animation_23_574905EC43F77C89F6A965AFE119106B.ADSOutTime_41_63DF5BF04A00BD2A761C11861660F122 = 0.03;
		}
	}

	if ( features::menu->no_aim_restrictions && anim )
		anim->AimingBloked = false;

	if ( features::menu->no_flash && pawn->WW_SurvivorStatus )
	{
		pawn->WW_SurvivorStatus->FlashBangEffect = 0.0;
		if ( anim && anim->FlashBangEar )
			anim->FlashBangEar->SetVolumeMultiplier( 0.f );
	}

	if ( features::menu->god_mode && pawn->WW_SurvivorStatus )
		pawn->WW_SurvivorStatus->Health = 999.0;

	if ( features::menu->view_fov && anim && anim->Camera )
	{
		anim->Camera->FieldOfView = features::menu->view_fov_value;
		anim->Camera->PostProcessBlendWeight = 0.f;
	}

	if ( features::menu->gravity && pawn->CharacterMovement )
		pawn->CharacterMovement->GravityScale = features::menu->gravity_value;
}
