#pragma once

#include <cstdint>
#include <windows.h>

namespace EnableDLCWeaponsFix
{
	void Install(HMODULE gameModule, uintptr_t playerControllerSlot);
	void Tick();
}
