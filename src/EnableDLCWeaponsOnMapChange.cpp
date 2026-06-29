#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <cstring>

#include "EnableDLCWeaponsOnMapChange.hpp"
#include "SDK/SdkHeaders.hpp"

namespace EnableDLCWeaponsFix
{
	namespace
	{
		uintptr_t g_controllerSlot = 0;
		APlayerController* g_lastController = nullptr;
		UFunction* g_enableFunction = nullptr;
		DWORD g_changeTime = 0;
		int g_fireStage = 0;

		bool IsReadable(const void* ptr, size_t size)
		{
			MEMORY_BASIC_INFORMATION mbi;
			if (!ptr || !VirtualQuery(ptr, &mbi, sizeof(mbi)))
				return false;

			if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD))
				return false;

			if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
				return false;

			return (reinterpret_cast<uintptr_t>(ptr) + size) <= (reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize);
		}

		UFunction* FindFunction(UObject* Object, const char* FunctionName)
		{
			if (!IsReadable(Object, 0x38))
				return nullptr;

			int depth = 0;
			for (UStruct* Class = Object->Class; IsReadable(Class, 0x50) && depth < 32; Class = Class->SuperField, ++depth)
			{
				int guard = 0;
				for (UField* Field = Class->Children; IsReadable(Field, 0x44) && guard < 100000; Field = Field->Next, ++guard)
				{
					FNameEntry* Entry = Field->Name.GetEntry();

					if (IsReadable(Entry, 0x18) && strcmp(Entry->Name, FunctionName) == 0)
					{
						return reinterpret_cast<UFunction*>(Field);
					}
				}
			}

			return nullptr;
		}

		APlayerController* ReadController(uintptr_t slot)
		{
			__try
			{
				if (!IsReadable(reinterpret_cast<void*>(slot), sizeof(void*)))
					return nullptr;

				APlayerController* controller = *reinterpret_cast<APlayerController**>(slot);
				if (!IsReadable(controller, 0x230) || !controller->Pawn)
					return nullptr;

				return controller;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return nullptr;
			}
		}

		void EnableDLCWeapons(APlayerController* Controller)
		{
			__try
			{
				if (!g_enableFunction)
				{
					g_enableFunction = FindFunction(Controller, "EnableAllDLCWeapons");
				}

				if (g_enableFunction)
				{
					unsigned char params[8] = { 0 };
					Controller->ProcessEvent(g_enableFunction, params, nullptr);
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}
		}
	}

	void Install(HMODULE gameModule, uintptr_t playerControllerSlot)
	{
		if (!gameModule || !playerControllerSlot)
			return;

		GNames = reinterpret_cast<TArray<FNameEntry*>*>(reinterpret_cast<uintptr_t>(gameModule) + static_cast<uintptr_t>(GNames_Offset));
		g_controllerSlot = playerControllerSlot;
	}

	void Tick()
	{
		if (!g_controllerSlot)
			return;

		APlayerController* controller = ReadController(g_controllerSlot);
		if (!controller)
			return;

		if (controller != g_lastController)
		{
			g_lastController = controller;
			g_changeTime = GetTickCount();
			g_fireStage = 0;
		}

		const DWORD elapsed = GetTickCount() - g_changeTime;

		if (g_fireStage == 0 && elapsed >= 500)
		{
			EnableDLCWeapons(controller);
			g_fireStage = 1;
		}
		else if (g_fireStage == 1 && elapsed >= 5000)
		{
			EnableDLCWeapons(controller);
			g_fireStage = 2;
		}
		else if (g_fireStage == 2 && elapsed >= 10000)
		{
			EnableDLCWeapons(controller);
			g_fireStage = 3;
		}
	}
}
