#include "includes.h"

static DWORD WINAPI bootstrap_thread( void* param )
{
  core::c_cheat_bootstrap::run( static_cast< HMODULE >( param ) );
  return 0;
}

bool __stdcall DllMain( HMODULE module, DWORD call_reason, void* reserved )
{
  switch ( call_reason )
  {
    case DLL_PROCESS_ATTACH:
    {
      const HANDLE thread = CreateThread( nullptr, 0, bootstrap_thread, module, 0, nullptr );
      if ( thread )
        CloseHandle( thread );
      break;
    }
    case DLL_PROCESS_DETACH:
    break;
  }
  return true;
}