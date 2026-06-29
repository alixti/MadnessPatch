#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <atomic>
#include <thread>
#include <cstring>

#include "EnableDLCWeaponsOnMapChange.hpp"
#include "SDK/SdkHeaders.hpp"

namespace EnableDLCWeaponsFix
{
	namespace
	{
		std::atomic<bool> g_running{ false };
		std::atomic<bool> g_stop{ false };

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

		bool EnableDLCWeapons(APlayerController* Controller, UFunction*& Function)
		{
			__try
			{
				if (!Function)
				{
					Function = FindFunction(Controller, "EnableAllDLCWeapons");
				}

				if (!Function)
				{
					return false;
				}

				unsigned char params[8] = { 0 };
				Controller->ProcessEvent(Function, params, nullptr);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		void Watch(uintptr_t controllerSlot)
		{
			UFunction* enableFunction = nullptr;
			APlayerController* lastController = nullptr;
			bool pending = false;
			int ticks = 0;

			while (!g_stop.load(std::memory_order_relaxed))
			{
				Sleep(250);

				APlayerController* controller = ReadController(controllerSlot);
				if (!controller)
				{
					continue;
				}

				if (controller != lastController)
				{
					lastController = controller;
					pending = true;
					ticks = 0;
				}

				if (!pending)
				{
					continue;
				}

				++ticks;

				if (ticks == 2 || ticks == 20 || ticks == 40)
				{
					EnableDLCWeapons(controller, enableFunction);
				}

				if (ticks >= 40)
				{
					pending = false;
				}
			}

			g_running.store(false, std::memory_order_relaxed);
		}
	}

	void Install(HMODULE gameModule, uintptr_t playerControllerSlot)
	{
		if (!gameModule || !playerControllerSlot || g_running.exchange(true))
		{
			return;
		}

		GNames = reinterpret_cast<TArray<FNameEntry*>*>(reinterpret_cast<uintptr_t>(gameModule) + static_cast<uintptr_t>(GNames_Offset));

		HMODULE pinned = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&Watch), &pinned);

		g_stop.store(false, std::memory_order_relaxed);
		std::thread(Watch, playerControllerSlot).detach();
	}

	void Stop()
	{
		g_stop.store(true, std::memory_order_relaxed);
	}
}
