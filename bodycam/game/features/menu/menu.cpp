#include "menu.h"

// Layout shared by the drawing code and the mouse hit-testing, so the
// clickable rows always match what is on screen.
static constexpr float MenuX = 10.f;
static constexpr float MenuY = 44.f;
static constexpr float MenuW = 320.f;
static constexpr float MenuHeaderH = 40.f;
static constexpr float MenuRowH = 28.f;
static constexpr float MenuFooterH = 26.f;
static constexpr float MenuPad = 16.f;
static constexpr int MenuItemCount = 6;

// Palette: dark charcoal panel, one restrained blue accent, soft grays.
static const SDK::FLinearColor ColPanel{ 0.075f, 0.082f, 0.094f, 0.96f };
static const SDK::FLinearColor ColHeader{ 0.105f, 0.114f, 0.129f, 0.96f };
static const SDK::FLinearColor ColBorder{ 0.24f, 0.26f, 0.29f, 1.f };
static const SDK::FLinearColor ColAccent{ 0.16f, 0.55f, 0.85f, 1.f };
static const SDK::FLinearColor ColText{ 0.93f, 0.94f, 0.95f, 1.f };
static const SDK::FLinearColor ColMuted{ 0.55f, 0.58f, 0.62f, 1.f };
static const SDK::FLinearColor ColHover{ 1.f, 1.f, 1.f, 0.06f };
static const SDK::FLinearColor ColTrackOff{ 0.27f, 0.29f, 0.32f, 1.f };
static const SDK::FLinearColor ColKnob{ 0.95f, 0.96f, 0.97f, 1.f };

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

// True only on the frame the key goes down, not while it is held.
static bool key_just_pressed( int key )
{
	static bool was_down[ 256 ] = { };

	const bool down = ( GetAsyncKeyState( key ) & 0x8000 ) != 0;
	const bool just_pressed = down && !was_down[ key & 0xFF ];
	was_down[ key & 0xFF ] = down;
	return just_pressed;
}

// Filled rectangle on the canvas, using its built-in white texture.
static void fill_rect( SDK::UCanvas* canvas, float x, float y, float w, float h, const SDK::FLinearColor& color )
{
	canvas->K2_DrawTexture( canvas->DefaultTexture, { x, y }, { w, h }, { 0.f, 0.f }, { 1.f, 1.f }, color, SDK::EBlendMode::BLEND_Translucent, 0.f, { 0.f, 0.f } );
}

static void draw_text( SDK::UCanvas* canvas, const wchar_t* text, float x, float y, const SDK::FLinearColor& color )
{
	canvas->K2_DrawText( SDK::UEngine::GetEngine( )->SmallFont, SDK::FString( text ), { x, y }, { 1.f, 1.f }, color, 0.f, { 0.f, 0.f, 0.f, 1.f }, { 1.f, 1.f }, false, false, true, { 0.f, 0.f, 0.f, 1.f } );
}

// A small switch: filled track in the accent color when on, gray when off,
// with a white knob that sits on the active side.
static void draw_toggle( SDK::UCanvas* canvas, float x, float y, bool on )
{
	fill_rect( canvas, x, y, 36.f, 18.f, on ? ColAccent : ColTrackOff );
	fill_rect( canvas, on ? x + 20.f : x + 2.f, y + 2.f, 14.f, 14.f, ColKnob );
}

void features::c_menu::free_mouse( )
{
	SDK::APlayerController* const controller = get_local_controller( );
	if ( !controller || mouse_freed )
		return;

	controller->bShowMouseCursor = true;
	SDK::UWidgetBlueprintLibrary::SetInputMode_GameAndUIEx( controller, nullptr, SDK::EMouseLockMode::DoNotLock, false, false );
	mouse_freed = true;
}

void features::c_menu::capture_mouse( )
{
	SDK::APlayerController* const controller = get_local_controller( );
	if ( !controller || !mouse_freed )
		return;

	controller->bShowMouseCursor = false;
	SDK::UWidgetBlueprintLibrary::SetInputMode_GameOnly( controller, false );
	mouse_freed = false;
}

void features::c_menu::shutdown( )
{
	capture_mouse( );
	open = false;
}

void features::c_menu::handle_input( )
{
	if ( key_just_pressed( VK_INSERT ) )
	{
		open = !open;

		if ( open )
			free_mouse( );
		else
			capture_mouse( );
	}

	if ( !open )
	{
		hovered_item = -1;
		return;
	}

	SDK::APlayerController* const controller = get_local_controller( );
	if ( !controller )
		return;

	if ( !controller->GetMousePosition( &mouse_x, &mouse_y ) )
	{
		hovered_item = -1;
		return;
	}

	hovered_item = -1;
	if ( mouse_x >= MenuX && mouse_x <= MenuX + MenuW )
	{
		for ( int i = 0; i < MenuItemCount; i++ )
		{
			const float row_y = MenuY + MenuHeaderH + MenuRowH * static_cast< float >( i );
			if ( mouse_y >= row_y && mouse_y <= row_y + MenuRowH )
			{
				hovered_item = i;
				break;
			}
		}
	}

	if ( hovered_item >= 0 && key_just_pressed( VK_LBUTTON ) )
	{
		switch ( hovered_item )
		{
		case 0: esp = !esp; break;
		case 1: box = !box; break;
		case 2: skeleton = !skeleton; break;
		case 3: health = !health; break;
		case 4: visibility_check = !visibility_check; break;
		case 5: team_check = !team_check; break;
		}
	}
}

void features::c_menu::draw_watermark( SDK::UCanvas* canvas )
{
	fill_rect( canvas, 10.f, 8.f, 236.f, 26.f, ColPanel );
	fill_rect( canvas, 10.f, 8.f, 3.f, 26.f, ColAccent );

	const std::wstring text = L"BODYCAM   |   FPS " + std::to_wstring( fps );
	draw_text( canvas, text.c_str( ), 22.f, 14.f, ColText );
}

void features::c_menu::draw_menu( SDK::UCanvas* canvas )
{
	const float total_h = MenuHeaderH + MenuRowH * static_cast< float >( MenuItemCount ) + MenuFooterH;

	// Panel, header band, accent edge, and a hairline border.
	fill_rect( canvas, MenuX, MenuY, MenuW, total_h, ColPanel );
	fill_rect( canvas, MenuX, MenuY, MenuW, MenuHeaderH, ColHeader );
	fill_rect( canvas, MenuX, MenuY, 3.f, total_h, ColAccent );
	fill_rect( canvas, MenuX, MenuY, MenuW, 1.f, ColBorder );
	fill_rect( canvas, MenuX, MenuY + total_h - 1.f, MenuW, 1.f, ColBorder );
	fill_rect( canvas, MenuX + MenuW - 1.f, MenuY, 1.f, total_h, ColBorder );
	fill_rect( canvas, MenuX, MenuY + MenuHeaderH, MenuW, 1.f, ColBorder );

	draw_text( canvas, L"BODYCAM", MenuX + MenuPad + 4.f, MenuY + 13.f, ColText );
	draw_text( canvas, L"MENU", MenuX + MenuW - MenuPad - 30.f, MenuY + 13.f, ColMuted );

	struct item { const wchar_t* name; bool* value; };
	const item items[ MenuItemCount ] = {
		{ L"ESP",              &esp },
		{ L"Box",              &box },
		{ L"Skeleton",         &skeleton },
		{ L"Health",           &health },
		{ L"Visibility check", &visibility_check },
		{ L"Team check",       &team_check },
	};

	for ( int i = 0; i < MenuItemCount; i++ )
	{
		const float row_y = MenuY + MenuHeaderH + MenuRowH * static_cast< float >( i );

		if ( i == hovered_item )
			fill_rect( canvas, MenuX + 6.f, row_y + 2.f, MenuW - 12.f, MenuRowH - 4.f, ColHover );

		draw_text( canvas, items[ i ].name, MenuX + MenuPad + 4.f, row_y + 8.f, ColText );
		draw_toggle( canvas, MenuX + MenuW - MenuPad - 36.f, row_y + 5.f, *items[ i ].value );
	}

	// Footer with the two keys that matter.
	const float footer_y = MenuY + total_h - MenuFooterH;
	fill_rect( canvas, MenuX, footer_y, MenuW, 1.f, ColBorder );
	draw_text( canvas, L"INSERT  close menu        END  unload", MenuX + MenuPad + 4.f, footer_y + 7.f, ColMuted );
}

void features::c_menu::process( SDK::UCanvas* canvas )
{
	// FPS counter for the watermark.
	static ULONGLONG last_tick = 0;
	static int frames = 0;
	const ULONGLONG now = GetTickCount64( );
	frames++;
	if ( now - last_tick >= 500 )
	{
		fps = static_cast< int >( frames * 1000 / ( now - last_tick ) );
		frames = 0;
		last_tick = now;
	}

	handle_input( );

	draw_watermark( canvas );

	if ( open )
		draw_menu( canvas );
}
