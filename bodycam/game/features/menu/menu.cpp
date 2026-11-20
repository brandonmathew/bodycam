#include "menu.h"

// The menu is a table of rows (sections, toggles, sliders) built from the
// live settings. Drawing and mouse handling both walk the same table, so
// what is on screen is always what is clickable.
static constexpr float MenuX = 10.f;
static constexpr float MenuY = 44.f;
static constexpr float MenuW = 320.f;
static constexpr float MenuHeaderH = 40.f;
static constexpr float MenuSectionH = 22.f;
static constexpr float MenuRowH = 28.f;
static constexpr float MenuFooterH = 26.f;
static constexpr float MenuPad = 16.f;

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

struct menu_row
{
	enum class kind { section, toggle, slider } kind;
	const wchar_t* label;
	bool* toggle = nullptr;       // toggle rows
	float* value = nullptr;       // slider rows
	float min = 0.f, max = 0.f;    // slider rows
	const wchar_t* format = nullptr;
	const bool* dim_until = nullptr; // slider label dims while this is false
};

static int build_rows( features::c_menu& m, menu_row out[ 20 ] )
{
	int n = 0;
	out[ n++ ] = { menu_row::kind::section, L"VISUALS" };
	out[ n++ ] = { menu_row::kind::toggle, L"ESP", &m.esp };
	out[ n++ ] = { menu_row::kind::toggle, L"Box", &m.box };
	out[ n++ ] = { menu_row::kind::toggle, L"Skeleton", &m.skeleton };
	out[ n++ ] = { menu_row::kind::toggle, L"Health", &m.health };
	out[ n++ ] = { menu_row::kind::toggle, L"Visibility check", &m.visibility_check };
	out[ n++ ] = { menu_row::kind::toggle, L"Team check", &m.team_check };
	out[ n++ ] = { menu_row::kind::slider, L"FOV", nullptr, &m.fov, 0.f, 600.f, L"%.0f" };
	out[ n++ ] = { menu_row::kind::section, L"AIM" };
	out[ n++ ] = { menu_row::kind::toggle, L"Aim move", &m.aim_move };
	out[ n++ ] = { menu_row::kind::section, L"GAMEPLAY" };
	out[ n++ ] = { menu_row::kind::toggle, L"No recoil", &m.no_recoil };
	out[ n++ ] = { menu_row::kind::toggle, L"No shake", &m.no_shake };
	return n;
}

static float row_height( const menu_row& row )
{
	return row.kind == menu_row::kind::section ? MenuSectionH : MenuRowH;
}

static float rows_height( const menu_row rows[ ], int count )
{
	float h = 0.f;
	for ( int i = 0; i < count; i++ )
		h += row_height( rows[ i ] );
	return h;
}

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

// Slider geometry, shared by drawing and mouse handling.
static float slider_track_x0( ) { return MenuX + 118.f; }
static float slider_track_x1( ) { return MenuX + MenuW - MenuPad - 46.f; }

static float clampf( float v, float lo, float hi ) { return v < lo ? lo : ( v > hi ? hi : v ); }

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
		dragging_row = -1;
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

	menu_row rows[ 20 ];
	const int row_count = build_rows( *this, rows );

	// Hover: the toggle/slider row under the mouse (sections don't hover).
	hovered_item = -1;
	if ( mouse_x >= MenuX && mouse_x <= MenuX + MenuW )
	{
		float y = MenuY + MenuHeaderH;
		for ( int i = 0; i < row_count; i++ )
		{
			const float h = row_height( rows[ i ] );
			if ( rows[ i ].kind != menu_row::kind::section && mouse_y >= y && mouse_y <= y + h )
			{
				hovered_item = i;
				break;
			}
			y += h;
		}
	}

	// Slider drag: while the button is held, the value follows the mouse.
	if ( dragging_row >= 0 )
	{
		if ( ( GetAsyncKeyState( VK_LBUTTON ) & 0x8000 ) == 0 )
		{
			dragging_row = -1;
		}
		else
		{
			menu_row& row = rows[ dragging_row ];
			const float fraction = clampf( ( mouse_x - slider_track_x0( ) ) / ( slider_track_x1( ) - slider_track_x0( ) ), 0.f, 1.f );
			*row.value = row.min + fraction * ( row.max - row.min );
		}
	}

	if ( hovered_item >= 0 && key_just_pressed( VK_LBUTTON ) )
	{
		menu_row& row = rows[ hovered_item ];
		if ( row.kind == menu_row::kind::toggle )
		{
			*row.toggle = !*row.toggle;
		}
		else if ( row.kind == menu_row::kind::slider )
		{
			dragging_row = hovered_item;
			const float fraction = clampf( ( mouse_x - slider_track_x0( ) ) / ( slider_track_x1( ) - slider_track_x0( ) ), 0.f, 1.f );
			*row.value = row.min + fraction * ( row.max - row.min );
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
	menu_row rows[ 20 ];
	const int row_count = build_rows( *this, rows );
	const float total_h = MenuHeaderH + rows_height( rows, row_count ) + MenuFooterH;

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

	float y = MenuY + MenuHeaderH;
	for ( int i = 0; i < row_count; i++ )
	{
		const menu_row& row = rows[ i ];

		if ( row.kind == menu_row::kind::section )
		{
			draw_text( canvas, row.label, MenuX + MenuPad + 4.f, y + 5.f, ColMuted );
			fill_rect( canvas, MenuX + MenuPad, y + MenuSectionH - 1.f, MenuW - MenuPad * 2.f, 1.f, ColBorder );
			y += MenuSectionH;
			continue;
		}

		if ( i == hovered_item || i == dragging_row )
			fill_rect( canvas, MenuX + 6.f, y + 2.f, MenuW - 12.f, MenuRowH - 4.f, ColHover );

		if ( row.kind == menu_row::kind::toggle )
		{
			draw_text( canvas, row.label, MenuX + MenuPad + 4.f, y + 8.f, ColText );
			draw_toggle( canvas, MenuX + MenuW - MenuPad - 36.f, y + 5.f, *row.toggle );
		}
		else // slider
		{
			// Label dims while the feature it belongs to is switched off.
			const SDK::FLinearColor& label_color = ( !row.dim_until || *row.dim_until ) ? ColText : ColMuted;
			draw_text( canvas, row.label, MenuX + MenuPad + 4.f, y + 8.f, label_color );

			const float fraction = ( *row.value - row.min ) / ( row.max - row.min );
			const float track_w = slider_track_x1( ) - slider_track_x0( );
			fill_rect( canvas, slider_track_x0( ), y + 11.f, track_w, 6.f, ColTrackOff );
			fill_rect( canvas, slider_track_x0( ), y + 11.f, track_w * fraction, 6.f, ColAccent );
			fill_rect( canvas, slider_track_x0( ) + track_w * fraction - 4.f, y + 7.f, 8.f, 14.f, ColKnob );

			wchar_t value_text[ 16 ] = { };
			swprintf( value_text, 16, row.format, *row.value );
			draw_text( canvas, value_text, MenuX + MenuW - MenuPad - 32.f, y + 8.f, label_color );
		}

		y += MenuRowH;
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
