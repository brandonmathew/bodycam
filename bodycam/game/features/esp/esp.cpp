#include "esp.h"

namespace
{
  // Bots carry their team on the pawn itself; human players carry it in the
  // PCInfo struct inside the game's PlayerState. E_MAX means "couldn't tell"
  // and never counts as a teammate.
  SDK::E_Team get_actor_team( SDK::AActor* actor )
  {
    if ( !actor )
      return SDK::E_Team::E_MAX;

    if ( actor->IsA( SDK::AAI_Humanoid_C::StaticClass( ) ) )
      return static_cast< SDK::AAI_Humanoid_C* >( actor )->Team;

    SDK::APawn* const pawn = static_cast< SDK::APawn* >( actor );
    if ( pawn->PlayerState && pawn->PlayerState->IsA( SDK::APS_Bodycam_C::StaticClass( ) ) )
      return static_cast< SDK::APS_Bodycam_C* >( pawn->PlayerState )->PlayerInfo.Team_14_AF2247D6483E6C47508527A8BDC6C4C0;

    return SDK::E_Team::E_MAX;
  }

  // Standard UE mannequin bone chain - the rig the ALS characters are built
  // on. Bones a skeleton does not have are skipped, never guessed at.
  struct bone_def
  {
    const wchar_t* name;
    SDK::FName fname;
  };

  bone_def g_bones[] = {
    { L"pelvis" }, { L"spine_01" }, { L"spine_02" }, { L"spine_03" }, { L"neck_01" }, { L"head" },
    { L"clavicle_l" }, { L"upperarm_l" }, { L"lowerarm_l" }, { L"hand_l" },
    { L"clavicle_r" }, { L"upperarm_r" }, { L"lowerarm_r" }, { L"hand_r" },
    { L"thigh_l" }, { L"calf_l" }, { L"foot_l" },
    { L"thigh_r" }, { L"calf_r" }, { L"foot_r" },
  };

  // Segments as index pairs into g_bones: spine, both arms, both legs.
  const int g_bone_segments[][ 2 ] = {
    { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 4 }, { 4, 5 },
    { 3, 6 }, { 6, 7 }, { 7, 8 }, { 8, 9 },
    { 3, 10 }, { 10, 11 }, { 11, 12 }, { 12, 13 },
    { 0, 14 }, { 14, 15 }, { 15, 16 },
    { 0, 17 }, { 17, 18 }, { 18, 19 },
  };

  void draw_skeleton( SDK::UCanvas* Canvas, SDK::USkeletalMeshComponent* mesh, const SDK::FLinearColor& color )
  {
    if ( !mesh )
      return;

    const int bone_count = sizeof( g_bones ) / sizeof( g_bones[ 0 ] );
    SDK::FVector2D points[ 20 ];
    bool valid[ 20 ] = { };

    for ( int i = 0; i < bone_count; i++ )
    {
      bone_def& bone = g_bones[ i ];
      SDK::GetStaticName( bone.name, bone.fname );

      if ( !mesh->DoesSocketExist( bone.fname ) )
        continue;

      const SDK::FVector screen = Canvas->K2_Project( mesh->GetSocketLocation( bone.fname ) );
      if ( screen.Z <= 0.f )
        continue;

      points[ i ] = { screen.X, screen.Y };
      valid[ i ] = true;
    }

    for ( const auto& segment : g_bone_segments )
    {
      if ( valid[ segment[ 0 ] ] && valid[ segment[ 1 ] ] )
        Canvas->K2_DrawLine( points[ segment[ 0 ] ], points[ segment[ 1 ] ], 1.5f, color );
    }
  }
}

void features::c_esp::process( SDK::UCanvas* Canvas )
{
  if ( !features::menu->esp )
    return;

  SDK::UWorld* const world = SDK::UWorld::GetWorld( );
  if ( !world )
    return;

  SDK::UGameInstance* const game_instance = world->OwningGameInstance;
  if ( !game_instance || !game_instance->LocalPlayers[ 0 ] )
    return;

  SDK::APlayerController* const local_controller = game_instance->LocalPlayers[ 0 ]->PlayerController;
  if ( !local_controller )
    return;

  SDK::AALS_Base_CharacterBP_C* const local_player = static_cast<SDK::AALS_Base_CharacterBP_C*>(local_controller->AcknowledgedPawn);
  if (!local_player)
      return;

  const SDK::E_Team local_team = get_actor_team( local_player );
  
  for ( SDK::AActor* current_actor : world->PersistentLevel->Actors )
  {
    if ( !current_actor )
      continue;

    if ( !current_actor->IsA( SDK::AALS_Base_CharacterBP_C::StaticClass( ) ) )
      continue;

    SDK::AALS_Base_CharacterBP_C* const player = static_cast<SDK::AALS_Base_CharacterBP_C*>(current_actor);
    if (!player)
        continue;

    if ( player == local_controller->AcknowledgedPawn )
      continue;

    // Skip teammates.
    if ( features::menu->team_check && local_team != SDK::E_Team::E_MAX && get_actor_team( current_actor ) == local_team )
      continue;

    SDK::UWW_SurvivorStatus_C* const status = player->WW_SurvivorStatus;
    if (!status || status->Death_ || status->Health <= 0.0)
        continue;

    const SDK::FVector actor_location = current_actor->RootComponent->RelativeLocation;

    const SDK::FVector top_world = { actor_location.X, actor_location.Y, actor_location.Z + 85.f };
    const SDK::FVector bottom_world = { actor_location.X, actor_location.Y, actor_location.Z - 85.f };

    const SDK::FVector top_screen = Canvas->K2_Project( top_world );
    const SDK::FVector bottom_screen = Canvas->K2_Project( bottom_world );

    if ( top_screen.Z > 0.f && bottom_screen.Z > 0.f )
    {
      const float height = bottom_screen.Y - top_screen.Y;
      const float width = height / 1.8f;

      const SDK::FVector2D box_position = { top_screen.X - ( width / 2.0f ), top_screen.Y };
      const SDK::FVector2D box_size = { width, height };

      // Green when you have line of sight to them, red when they're behind cover.
      // With the visibility check off, everything draws white.
      const bool vis_check = features::menu->visibility_check;
      const bool is_visible = !vis_check || local_controller->LineOfSightTo( player, { 0.f, 0.f, 0.f }, false );
      const SDK::FLinearColor draw_color = !vis_check ? SDK::FLinearColor{ 1.f, 1.f, 1.f, 1.f }
        : is_visible ? SDK::FLinearColor{ 0.f, 1.f, 0.f, 1.f } : SDK::FLinearColor{ 1.f, 0.f, 0.f, 1.f };

      if ( features::menu->box )
        Canvas->K2_DrawBox( box_position, box_size, 1.5f, draw_color );

      if ( player->PlayerState )
      {
        const SDK::FString player_name = player->PlayerState->PlayerNamePrivate;

        const SDK::FVector2D text_postition = { top_screen.X, top_screen.Y - 15.f };
        const SDK::FLinearColor text_color = draw_color;

        Canvas->K2_DrawText( SDK::UEngine::GetEngine( )->SmallFont, player_name, text_postition, { 1.f, 1.f }, text_color, 0.f, { 0.f, 0.f, 0.f, 1.f }, { 1.f, 1.f }, true, false, true, { 0.f, 0.f, 0.f, 1.f } );
      }

      if (features::menu->health && status->HealthTresholdMax > 0.0)
      {
          const float current_hp = static_cast<float>(status->Health);
          const float max_hp = static_cast<float>(status->HealthTresholdMax);
          float hp_percentage = current_hp / max_hp;
          if (hp_percentage < 0.0f)
              hp_percentage = 0.0f;
          if (hp_percentage > 1.0f)
              hp_percentage = 1.0f;
          SDK::FLinearColor fill_color;
          if (hp_percentage > 0.5f)
              fill_color = { 1.0f - (hp_percentage - 0.5f) * 2.0f, 1.0f, 0.0f, 1.0f };
          else
              fill_color = { 1.0f, hp_percentage * 2.0f, 0.0f, 1.0f };
          Canvas->K2_DrawLine({ box_position.X - 4.0f, box_position.Y }, { box_position.X - 4.0f, box_position.Y + box_size.Y }, 2.0f, { 0.2f, 0.2f, 0.2f, 1.0f });
          const float fill_height = box_size.Y * hp_percentage;
          Canvas->K2_DrawLine({ box_position.X - 4.0f, (box_position.Y + box_size.Y) - fill_height }, { box_position.X - 4.0f, box_position.Y + box_size.Y }, 2.0f, fill_color);
      }

      if ( features::menu->skeleton )
        draw_skeleton( Canvas, player->Mesh, draw_color );
    }
  }
}
