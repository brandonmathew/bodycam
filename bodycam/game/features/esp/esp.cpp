#include "esp.h"

#include <cmath>

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

  // Per-point visibility: a real trace from the camera to the exact world
  // point (e.g. a bone), instead of LineOfSightTo, which traces to the
  // actor's centre. Visible when nothing blocks the trace, or the first
  // thing it blocks on is the target itself.
  bool point_visible( SDK::UWorld* world, SDK::APlayerController* controller, SDK::AActor* self_pawn, SDK::AActor* target, const SDK::FVector& point )
  {
    if ( !controller->PlayerCameraManager )
      return false;

    const SDK::FVector start = controller->PlayerCameraManager->GetCameraLocation( );

    SDK::TArray< SDK::AActor* > ignore;
    ignore.Add( self_pawn );

    SDK::FHitResult hit = { };
    const bool blocked = SDK::UKismetSystemLibrary::LineTraceSingle( world, start, point,
      SDK::ETraceTypeQuery::TraceTypeQuery1, false, ignore, SDK::EDrawDebugTrace::None,
      &hit, false, { 0.f, 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f, 0.f }, 0.f );

    if ( !blocked )
      return true;

    SDK::UPrimitiveComponent* const comp = hit.Component.Get( );
    return comp && comp->GetOwner( ) == target;
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

  // One bone of a character, evaluated once per frame and shared by the
  // skeleton, the box colour and (for the head) bestTarget.
  struct bone_state
  {
    SDK::FVector world;
    bool exists = false;
    bool visible = false;
  };

  bool bone_world( SDK::AALS_Base_CharacterBP_C* player, int index, SDK::FVector& out )
  {
    bone_def& bone = g_bones[ index ];
    SDK::GetStaticName( bone.name, bone.fname );

    if ( !player->Mesh || !player->Mesh->DoesSocketExist( bone.fname ) )
      return false;

    out = player->Mesh->GetSocketLocation( bone.fname );
    return true;
  }

  // World position and traced visibility for every bone. With trace=false
  // no traces run and every existing bone reads visible (the flags are
  // only used when the visibility check is on).
  void evaluate_bones( SDK::UWorld* world, SDK::APlayerController* controller, SDK::AActor* self_pawn,
    SDK::AALS_Base_CharacterBP_C* player, bool trace, bone_state out[ 20 ] )
  {
    const int bone_count = sizeof( g_bones ) / sizeof( g_bones[ 0 ] );
    for ( int i = 0; i < bone_count; i++ )
    {
      if ( !bone_world( player, i, out[ i ].world ) )
        continue;

      out[ i ].exists = true;
      out[ i ].visible = !trace || point_visible( world, controller, self_pawn, player, out[ i ].world );
    }
  }

  // Segment colour from its two bones: green both visible, yellow one,
  // red neither.
  SDK::FLinearColor bone_pair_color( bool a, bool b )
  {
    if ( a && b )
      return { 0.f, 1.f, 0.f, 1.f };
    if ( a || b )
      return { 1.f, 1.f, 0.f, 1.f };
    return { 1.f, 0.f, 0.f, 1.f };
  }

  // Box colour from the share of bones visible: red at none, through
  // yellow at half, to green at all.
  SDK::FLinearColor total_visibility_color( float fraction )
  {
    if ( fraction <= 0.f )
      return { 1.f, 0.f, 0.f, 1.f };
    if ( fraction >= 1.f )
      return { 0.f, 1.f, 0.f, 1.f };
    if ( fraction < 0.5f )
      return { 1.f, fraction * 2.f, 0.f, 1.f };
    return { 1.f - ( fraction - 0.5f ) * 2.f, 1.f, 0.f, 1.f };
  }

  void draw_skeleton( SDK::UCanvas* Canvas, const bone_state bones[ 20 ], bool use_vis_colors )
  {
    const int bone_count = sizeof( g_bones ) / sizeof( g_bones[ 0 ] );
    SDK::FVector2D points[ 20 ];
    bool valid[ 20 ] = { };

    for ( int i = 0; i < bone_count; i++ )
    {
      if ( !bones[ i ].exists )
        continue;

      const SDK::FVector screen = Canvas->K2_Project( bones[ i ].world );
      if ( screen.Z <= 0.f )
        continue;

      points[ i ] = { screen.X, screen.Y };
      valid[ i ] = true;
    }

    for ( const auto& segment : g_bone_segments )
    {
      if ( !valid[ segment[ 0 ] ] || !valid[ segment[ 1 ] ] )
        continue;

      const SDK::FLinearColor color = use_vis_colors
        ? bone_pair_color( bones[ segment[ 0 ] ].visible, bones[ segment[ 1 ] ].visible )
        : SDK::FLinearColor{ 1.f, 1.f, 1.f, 1.f };
      Canvas->K2_DrawLine( points[ segment[ 0 ] ], points[ segment[ 1 ] ], 1.5f, color );
    }
  }
}

SDK::FVector2D features::c_esp::bestTarget( SDK::UCanvas* Canvas )
{
  current_target = nullptr;

  SDK::UWorld* const world = SDK::UWorld::GetWorld( );
  if ( !world || !world->OwningGameInstance || !world->OwningGameInstance->LocalPlayers[ 0 ] )
    return { -1.f, -1.f };

  SDK::APlayerController* const local_controller = world->OwningGameInstance->LocalPlayers[ 0 ]->PlayerController;
  if ( !local_controller || !local_controller->AcknowledgedPawn )
    return { -1.f, -1.f };

  const SDK::E_Team local_team = get_actor_team( local_controller->AcknowledgedPawn );

  const float center_x = Canvas->ClipX * 0.5f;
  const float center_y = Canvas->ClipY * 0.5f;
  const float fov = features::menu->fov;
  float best_dist_sq = fov * fov;

  SDK::FVector2D best_point = { -1.f, -1.f };

  for ( SDK::AActor* current_actor : world->PersistentLevel->Actors )
  {
    if ( !current_actor || !current_actor->IsA( SDK::AALS_Base_CharacterBP_C::StaticClass( ) ) )
      continue;

    SDK::AALS_Base_CharacterBP_C* const player = static_cast< SDK::AALS_Base_CharacterBP_C* >( current_actor );
    if ( player == local_controller->AcknowledgedPawn )
      continue;

    // Dead check.
    SDK::UWW_SurvivorStatus_C* const status = player->WW_SurvivorStatus;
    if ( !status || status->Death_ || status->Health <= 0.0 )
      continue;

    // Teammate check (an unreadable team never counts as a teammate).
    if ( local_team != SDK::E_Team::E_MAX && get_actor_team( current_actor ) == local_team )
      continue;

    // Head position: the head socket when the mesh has one, else the top
    // of the capsule (same +85 the ESP box uses).
    SDK::FVector head_world;
    if ( !bone_world( player, 5, head_world ) )
    {
      const SDK::FVector loc = current_actor->RootComponent->RelativeLocation;
      head_world = { loc.X, loc.Y, loc.Z + 85.f };
    }

    // Visibility check: trace to the head itself, so a head peeking over
    // cover counts as visible even when the body's centre is hidden.
    if ( !point_visible( world, local_controller, local_controller->AcknowledgedPawn, player, head_world ) )
      continue;

    const SDK::FVector screen = Canvas->K2_Project( head_world );
    if ( screen.Z <= 0.f )
      continue;

    const float dx = screen.X - center_x;
    const float dy = screen.Y - center_y;
    const float dist_sq = dx * dx + dy * dy;
    if ( dist_sq < best_dist_sq )
    {
      best_dist_sq = dist_sq;
      best_point = { screen.X, screen.Y };
      current_target = player;
    }
  }

  return best_point;
}

void features::c_esp::apply_aim( SDK::UCanvas* Canvas, const SDK::FVector2D& target )
{
  const ULONGLONG now = GetTickCount64( );

  // Any reason not to aim resets the motion state, so stale velocity
  // never leaks into the next engagement.
  if ( !features::menu->aim_move || features::menu->open || target.X < 0.f
    || ( GetAsyncKeyState( VK_RBUTTON ) & 0x8000 ) == 0 )
  {
    aim_vel_x = 0.f;
    aim_vel_y = 0.f;
    aim_remainder_x = 0.f;
    aim_remainder_y = 0.f;
    aim_last_tick = 0;
    aim_target = nullptr;
    return;
  }

  // Real frame time, clamped so a hitch can't throw the mouse.
  float dt = aim_last_tick ? static_cast< float >( now - aim_last_tick ) / 1000.f : ( 1.f / 60.f );
  aim_last_tick = now;
  if ( dt < 0.0005f )
    dt = 0.0005f;
  if ( dt > 0.05f )
    dt = 0.05f;

  // A new target starts from rest; carrying the old target's velocity
  // into a different direction swings past it.
  if ( current_target != aim_target )
  {
    aim_target = current_target;
    aim_vel_x = 0.f;
    aim_vel_y = 0.f;
  }

  const float dx = target.X - Canvas->ClipX * 0.5f;
  const float dy = target.Y - Canvas->ClipY * 0.5f;

  // On target: inside a 2px deadzone, hold still instead of jittering.
  if ( fabsf( dx ) < 2.f && fabsf( dy ) < 2.f )
  {
    aim_vel_x = 0.f;
    aim_vel_y = 0.f;
    aim_remainder_x = 0.f;
    aim_remainder_y = 0.f;
    return;
  }

  const float smooth = features::menu->aim_smooth < 0.01f ? 0.01f : features::menu->aim_smooth;

  // Desired velocity: the speed that would cover the remaining distance
  // in one smoothing time, capped by the Aim speed slider (500 px/s at
  // the bottom of the slider, 6000 px/s at the top).
  float want_x = dx / smooth;
  float want_y = dy / smooth;
  const float max_speed = 500.f + features::menu->aim_speed * 5500.f;
  const float want_len = sqrtf( want_x * want_x + want_y * want_y );
  if ( want_len > max_speed )
  {
    want_x *= max_speed / want_len;
    want_y *= max_speed / want_len;
  }

  // Accelerate the actual velocity toward the desired one. The approach
  // runs on frame time (1 - e^(-dt/tau)), so the feel is identical at
  // 30, 60 or 240 fps; tau under the smoothing time keeps it responsive
  // while still ramping instead of snapping.
  const float alpha = 1.f - expf( -dt / ( smooth * 0.6f ) );
  aim_vel_x += ( want_x - aim_vel_x ) * alpha;
  aim_vel_y += ( want_y - aim_vel_y ) * alpha;

  // Integrate, carrying the sub-pixel fraction so slow, smooth motion
  // still accumulates into real mouse steps.
  aim_remainder_x += aim_vel_x * dt;
  aim_remainder_y += aim_vel_y * dt;

  const LONG move_x = static_cast< LONG >( aim_remainder_x );
  const LONG move_y = static_cast< LONG >( aim_remainder_y );
  aim_remainder_x -= static_cast< float >( move_x );
  aim_remainder_y -= static_cast< float >( move_y );

  if ( move_x == 0 && move_y == 0 )
    return;

  INPUT input = { };
  input.type = INPUT_MOUSE;
  input.mi.dx = move_x;
  input.mi.dy = move_y;
  input.mi.dwFlags = MOUSEEVENTF_MOVE;
  SendInput( 1, &input, sizeof( input ) );
}

void features::c_esp::process( SDK::UCanvas* Canvas )
{
  // Closest-to-crosshair target: marker on its head, and the FOV search
  // circle while the menu is open so the slider can be set against it.
  const SDK::FVector2D target_head = bestTarget( Canvas );

  apply_aim( Canvas, target_head );

  if ( !features::menu->esp )
    return;

  if ( target_head.X >= 0.f )
    Canvas->K2_DrawBox( { target_head.X - 5.f, target_head.Y - 5.f }, { 10.f, 10.f }, 1.5f, { 0.16f, 0.55f, 0.85f, 1.f } );

  if ( features::menu->open && features::menu->fov > 0.f )
  {
    const float cx = Canvas->ClipX * 0.5f;
    const float cy = Canvas->ClipY * 0.5f;
    const float radius = features::menu->fov;
    const SDK::FLinearColor circle_color = { 1.f, 1.f, 1.f, 0.25f };
    SDK::FVector2D prev = { cx + radius, cy };
    for ( int i = 1; i <= 48; i++ )
    {
      const float angle = 6.2831853f * static_cast< float >( i ) / 48.f;
      const SDK::FVector2D point = { cx + radius * cosf( angle ), cy + radius * sinf( angle ) };
      Canvas->K2_DrawLine( prev, point, 1.f, circle_color );
      prev = point;
    }
  }

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

      // Visibility comes from per-bone traces, evaluated once and shared:
      // the skeleton colours each segment by its own bones, the box and
      // name take the colour of the total share of bones visible. With
      // the visibility check off, no traces run and everything is white.
      const bool vis_check = features::menu->visibility_check;

      bone_state bones[ 20 ] = { };
      evaluate_bones( world, local_controller, local_controller->AcknowledgedPawn, player, vis_check, bones );

      int bones_total = 0, bones_visible = 0;
      for ( const bone_state& bone : bones )
      {
        if ( !bone.exists )
          continue;
        bones_total++;
        if ( bone.visible )
          bones_visible++;
      }

      float vis_fraction;
      if ( bones_total > 0 )
        vis_fraction = static_cast< float >( bones_visible ) / static_cast< float >( bones_total );
      else // No usable bones (no mesh): fall back to one trace at the head point.
        vis_fraction = ( !vis_check || point_visible( world, local_controller, local_controller->AcknowledgedPawn, player, top_world ) ) ? 1.f : 0.f;

      const SDK::FLinearColor draw_color = !vis_check ? SDK::FLinearColor{ 1.f, 1.f, 1.f, 1.f }
        : total_visibility_color( vis_fraction );

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
        draw_skeleton( Canvas, bones, vis_check );
    }
  }
}
