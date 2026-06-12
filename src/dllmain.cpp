#define MINI_CASE_SENSITIVE
#define _USE_MATH_DEFINES
#define NOMINMAX

#include <Windows.h>
#include <shlwapi.h>

#include <stacktrace>

#include "MFortress\Patches_EA.hpp"
#include "MFortress\Patches_Steam.hpp"

#include "ini.hpp"
#include "dllmain.hpp"
#include "Controller.hpp"
#include "helper.hpp"
#include "FixIniBindings.hpp"
#include "CrashHandler.hpp"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "SDL3-static.lib")

struct ClothInstanceState
{
	float accumulator = 0.0f;
	bool primed = false;
	std::vector<float> trueP1;
	std::vector<float> relPrev;
	std::vector<float> relCurr;
	std::vector<float> applied;
};

struct GlobalState
{
	// System initialization
	bool isInit = false;
	HMODULE GameModule = NULL;

	// Display configuration
	float screenHeight = 0.0f;
	float screenWidth = 0.0f;
	float subtitlesScaleFactor = 0.0f;
	float scaleFactor = 0.0f;

	// Physics
	float frameTimeScale = 0.0f;
	float savedHairDeltaTime = 0.0f;
	std::unordered_map<uintptr_t, ClothInstanceState> clothes;

	// FOV
	DWORD pFOV = 0;
	DWORD pFOVCut = 0;
	DWORD pGEngine = 0;
	float FOVScale = 0.0f;
	bool updateFOV = false;
	bool isCutscene = false;

	// Input
	DWORD pInput = 0;
	bool isUsingGamepad = false;

	// Misc
	bool set40fps = false;
	bool prev_set40fps = false;
	float original_fps = 0.0f;
	bool shouldSkipMovie = false;
	uintptr_t physXCoreModuleAddress = 0;
};

// Global instance
GlobalState g_State;

// Ini Settings override
static std::vector<std::unique_ptr<wchar_t[]>> g_tempConfigStrings;
static const wchar_t* g_replacementString = nullptr;

struct ConfigOverride
{
	std::wstring value;
	bool* isEnabled;
};

struct WStringViewHash
{
	using is_transparent = void;

	size_t operator()(std::wstring_view sv) const
	{
		size_t hash = 0x811C9DC5;
		for (wchar_t c : sv)
		{
			hash ^= static_cast<size_t>(c);
			hash *= 0x01000193;
		}
		return hash;
	}

	size_t operator()(const std::wstring& s) const { return operator()(std::wstring_view(s)); }
	size_t operator()(const wchar_t* s) const { return operator()(std::wstring_view(s)); }
};

// Ini Input override
struct PendingRestore
{
	uintptr_t structAddr;
	wchar_t* originalPtr;
	int originalLength;
	int originalCapacity;
	bool needsRestore;
};

static PendingRestore g_pendingRestore = { 0, nullptr, 0, 0, false };
static std::vector<std::unique_ptr<wchar_t[]>> g_tempFixedStrings;

// Gamepad prompt override
struct TextureRedirect
{
	const char* target;
	size_t targetLen;
};

static std::unordered_map<std::string_view, TextureRedirect> g_textureRedirects;

static constexpr float TARGET_FRAME_TIME = 1.0f / 30.0f;
static constexpr float ASPECT_RATIO_16_9 = 16.0f / 9.0f;

// =============================
// Ini Variables
// =============================

// Fixes
bool FixHighFPSHairPhysics = false;
bool FixHighFPSClothPhysics = false;
bool FixHighFPSProjectileCollisionCheck = false;
bool FixHighFPSRagdollDeath = false;
bool FixHashTableRaceCondition = false;
bool FixUnattachedCollisionPanic = false;
bool FixHairCurveOverRead = false;
bool FixExperimental = false;
bool FixPhysX = false;
bool FixInputBinding = false;
bool FixWindowHandling = false;
int MaxProcessorCount = 0;

// General
bool UnlockCompleteEditionDLC = false;
bool DisableLegacyDriverHacks = false;
bool CheckAlice1InstallFolder = false;
bool SkipEAIntro = false;
bool SkipSHIntro = false;
bool SkipUEIntro = false;
bool EnableCrashHandler = false;

// Display
bool FontScaling = false;
float FontScalingFactor = 0;
bool AutoResolution = false;
bool UseWindowed = false;

// Input
bool UseSDLControllerInput = false;
bool UsePS3ControllerIcons = false;
bool DisableMouseAcceleration = false;
bool DisableControllerAcceleration = false;
bool TouchpadEnabled = false;
bool InvertABXYButtons = false;
bool DisableMouseSmoothing = false;
bool SkipCutscenesWithEnter = false;

// Graphics
int MaxFPS = 0;
bool EnableMaxSmoothedFrameRate = false;
bool ImprovedTextureStreaming = false;
bool ForceHighResTextures = false;
bool DisableBackgroundLevelStreaming = false;
bool FixUltraWideScreenFOV = false;
bool ReducedMipMapBias = false;
bool FixBinkVideoBT709 = false;

static std::unordered_map<std::wstring, ConfigOverride, WStringViewHash, std::equal_to<>> g_configOverrides =
{
	// [Engine.Engine]
	{L"MaxSmoothedFrameRate", {L"120", &EnableMaxSmoothedFrameRate}},

	// [Engine.ISVHacks]
	{L"DisableATITextureFilterOptimizationChecks", {L"False", &DisableLegacyDriverHacks}},
	{L"UseMinimalNVIDIADriverShaderOptimization", {L"False", &DisableLegacyDriverHacks}},

	// [TextureStreaming]
	{L"MinTimeToGuaranteeMinMipCount", {L"0", &ImprovedTextureStreaming}},
	{L"MaxTimeToGuaranteeMinMipCount", {L"0", &ImprovedTextureStreaming}},
	{L"HysteresisLimit", {L"30", &ImprovedTextureStreaming}},
	{L"DropMipLevelsLimit", {L"20", &ImprovedTextureStreaming}},
	{L"StopIncreasingLimit", {L"20", &ImprovedTextureStreaming}},
	{L"StopStreamingLimit", {L"12", &ImprovedTextureStreaming}},
	{L"MinEvictSize", {L"10", &ImprovedTextureStreaming}},
	{L"MipLevelFadingInRate", {L"0.0", &ImprovedTextureStreaming}},
	{L"MipLevelFadingOutRate", {L"0.0", &ImprovedTextureStreaming}},

	// [SystemSettings]
	{L"Fullscreen", {L"False", &UseWindowed}},
	{L"ResX", {L"1280", &AutoResolution}},
	{L"ResY", {L"720", &AutoResolution}},

	// [AliceGame.AliceGameEngine]
	{L"GIsSpecialPCEdition", {L"True", &UnlockCompleteEditionDLC}},

	// [Engine.PlayerInput]
	{L"bEnableMouseSmoothing", {L"False", &DisableMouseSmoothing}}
};

static void UpdateConfigInt(const wchar_t* key, int value)
{
	auto it = g_configOverrides.find(key);
	if (it != g_configOverrides.end())
	{
		it->second.value = std::to_wstring(value);
	}
}

static void UpdateConfigBool(const wchar_t* key, bool value)
{
	auto it = g_configOverrides.find(key);
	if (it != g_configOverrides.end())
	{
		it->second.value = value ? L"True" : L"False";
	}
}

static void ReadConfig()
{
	IniHelper::Init();

	// Fixes
	FixHighFPSHairPhysics = IniHelper::ReadInteger("Fixes", "FixHighFPSHairPhysics", 1) == 1;
	FixHighFPSClothPhysics = IniHelper::ReadInteger("Fixes", "FixHighFPSClothPhysics", 1) == 1;
	FixHighFPSProjectileCollisionCheck = IniHelper::ReadInteger("Fixes", "FixHighFPSProjectileCollisionCheck", 1) == 1;
	FixHighFPSRagdollDeath = IniHelper::ReadInteger("Fixes", "FixHighFPSRagdollDeath", 1) == 1;
	FixHashTableRaceCondition = IniHelper::ReadInteger("Fixes", "FixHashTableRaceCondition", 1) == 1;
	FixUnattachedCollisionPanic = IniHelper::ReadInteger("Fixes", "FixUnattachedCollisionPanic", 1) == 1;
	FixHairCurveOverRead = IniHelper::ReadInteger("Fixes", "FixHairCurveOverRead", 1) == 1;
	FixExperimental = IniHelper::ReadInteger("Fixes", "FixExperimental", 1) == 1;
	FixPhysX = IniHelper::ReadInteger("Fixes", "FixPhysX", 1) == 1;
	FixInputBinding = IniHelper::ReadInteger("Fixes", "FixInputBinding", 1) == 1;
	FixWindowHandling = IniHelper::ReadInteger("Fixes", "FixWindowHandling", 1) == 1;
	MaxProcessorCount = IniHelper::ReadInteger("Fixes", "MaxProcessorCount", 2);
	
	// General
	UnlockCompleteEditionDLC = IniHelper::ReadInteger("General", "UnlockCompleteEditionDLC", 1) == 1;
	DisableLegacyDriverHacks = IniHelper::ReadInteger("General", "DisableLegacyDriverHacks", 1) == 1;
	CheckAlice1InstallFolder = IniHelper::ReadInteger("General", "CheckAlice1InstallFolder", 1) == 1;
	SkipEAIntro = IniHelper::ReadInteger("General", "SkipEAIntro", 0) == 1;
	SkipSHIntro = IniHelper::ReadInteger("General", "SkipSHIntro", 1) == 1;
	SkipUEIntro = IniHelper::ReadInteger("General", "SkipUEIntro", 1) == 1;
	EnableCrashHandler = IniHelper::ReadInteger("General", "EnableCrashHandler", 1) == 1;

	// Display
	FontScaling = IniHelper::ReadInteger("Display", "FontScaling", 1) == 1;
	FontScalingFactor = IniHelper::ReadFloat("Display", "FontScalingFactor", 1.0f);
	AutoResolution = IniHelper::ReadInteger("Display", "AutoResolution", 1) == 1;
	UseWindowed = IniHelper::ReadInteger("Display", "UseWindowed", 0) == 1;

	// Input
	UseSDLControllerInput = IniHelper::ReadInteger("Input", "UseSDLControllerInput", 1) == 1;
	UsePS3ControllerIcons = IniHelper::ReadInteger("Input", "UsePS3ControllerIcons", 0) == 1;
	DisableMouseAcceleration = IniHelper::ReadInteger("Input", "DisableMouseAcceleration", 1) == 1;
	DisableControllerAcceleration = IniHelper::ReadInteger("Input", "DisableControllerAcceleration", 0) == 1;
	TouchpadEnabled = IniHelper::ReadInteger("Input", "TouchpadEnabled", 1) == 1;
	InvertABXYButtons = IniHelper::ReadInteger("Input", "InvertABXYButtons", 1) == 1;
	DisableMouseSmoothing = IniHelper::ReadInteger("Input", "DisableMouseSmoothing", 0) == 1;
	SkipCutscenesWithEnter = IniHelper::ReadInteger("Input", "SkipCutscenesWithEnter", 0) == 1;

	// Graphics
	MaxFPS = IniHelper::ReadInteger("Graphics", "MaxFPS", 120);
	ForceHighResTextures = IniHelper::ReadInteger("Graphics", "ForceHighResTextures", 1) == 1;
	ImprovedTextureStreaming = IniHelper::ReadInteger("Graphics", "ImprovedTextureStreaming", 1) == 1;
	DisableBackgroundLevelStreaming = IniHelper::ReadInteger("Graphics", "DisableBackgroundLevelStreaming", 1) == 1;
	FixUltraWideScreenFOV = IniHelper::ReadInteger("Graphics", "FixUltraWideScreenFOV", 1) == 1;
	ReducedMipMapBias = IniHelper::ReadInteger("Graphics", "ReducedMipMapBias", 1) == 1;
	FixBinkVideoBT709 = IniHelper::ReadInteger("Graphics", "FixBinkVideoBT709", 1) == 1;

	// MaxSmoothedFrameRate
	EnableMaxSmoothedFrameRate = MaxFPS != 0;
	UpdateConfigInt(L"MaxSmoothedFrameRate", MaxFPS);

	// Windowed
	UpdateConfigBool(L"Fullscreen", !UseWindowed);
	UseWindowed = true; // Can default to windowed if set to AliceEngine.ini

	// DisableMouseSmoothing
	UpdateConfigBool(L"bEnableMouseSmoothing", !DisableMouseSmoothing);
	DisableMouseSmoothing = true;

	// DisableLegacyDriverHacks
	UpdateConfigBool(L"DisableATITextureFilterOptimizationChecks", !DisableLegacyDriverHacks);
	UpdateConfigBool(L"UseMinimalNVIDIADriverShaderOptimization", !DisableLegacyDriverHacks);
	DisableLegacyDriverHacks = true;

	// GIsSpecialPCEdition
	UpdateConfigBool(L"GIsSpecialPCEdition", UnlockCompleteEditionDLC);
	UnlockCompleteEditionDLC = true;

	if (UseSDLControllerInput)
	{
		auto [screenWidth, screenHeight] = SystemHelper::GetScreenResolution();
		ControllerHelper::SetTouchpadDimensions(screenWidth, screenHeight);
	}

	if (MaxProcessorCount < -1 || MaxProcessorCount == 0)
	{
		MaxProcessorCount = 8;
	}

	ControllerHelper::SetTouchpadEnabled(TouchpadEnabled);
}

#pragma region Helper

static DWORD ScanModuleSignature(HMODULE Module, std::string_view Signature, const char* PatchName, int FunctionStartCheckCount = -1, bool ShowError = true)
{
	DWORD Address = MemoryHelper::FindSignatureAddress(Module, Signature, FunctionStartCheckCount);

	if (Address == 0 && ShowError)
	{
		std::string ErrorMessage = "Error: Unable to find signature for patch: ";
		ErrorMessage += PatchName;
		MessageBoxA(NULL, ErrorMessage.c_str(), "MadnessPatch", MB_ICONERROR);
	}

	return Address;
}

static float ScaleFOV(float fovDeg, float sourceAspect, float targetAspect)
{
	float baseTan = tanf((fovDeg * 0.5f) * (M_PI / 180.0f));
	return 2.0f * atanf((targetAspect / sourceAspect) * baseTan) * (180.0f / M_PI);
}

static void ReplaceConfigString(safetyhook::Context& ctx, const wchar_t* newString)
{
	std::wstring str(newString);
	bool wasModified = false;

	// Workaround for automatic movement 
	if (str == L"(Name=\"MoveForward\",Command=\"Axis aBaseY Speed=1.0\")")
	{
		str = L"(Name=\"MoveForward\",Command=\"Axis aBaseY Speed=1.0 | OnRelease Axis aBaseY Speed=0.0\")";
		wasModified = true;
	}

	// Handle SkipCutscenesWithEnter
	if (SkipCutscenesWithEnter && str.find(L"(Name=") != std::wstring::npos && str.find(L",Command=") != std::wstring::npos)
	{
		size_t commandPos = str.find(L",Command=\"");
		if (commandPos != std::wstring::npos)
		{
			commandPos += 10;

			size_t endQuotePos = str.find(L'"', commandPos);
			if (endQuotePos != std::wstring::npos)
			{
				std::wstring commandValue = str.substr(commandPos, endQuotePos - commandPos);
				std::wstring originalCommandValue = commandValue;

				// Check if this is the SpaceBar binding
				if (str.find(L"(Name=\"SpaceBar\"") != std::wstring::npos)
				{
					// Remove "TryToCancelMatinee" from the command
					size_t pos = commandValue.find(L"TryToCancelMatinee");
					if (pos != std::wstring::npos)
					{
						size_t startPos = pos;
						size_t endPos = pos + wcslen(L"TryToCancelMatinee");

						// Remove trailing " | "
						if (endPos + 3 <= commandValue.length() && commandValue.substr(endPos, 3) == L" | ")
						{
							endPos += 3;
						}
						// Or remove leading " | "
						else if (startPos >= 3 && commandValue.substr(startPos - 3, 3) == L" | ")
						{
							startPos -= 3;
						}

						commandValue = commandValue.substr(0, startPos) + commandValue.substr(endPos);
					}
				}
				// Check if this is the Enter binding
				else if (str.find(L"(Name=\"Enter\"") != std::wstring::npos)
				{
					// Add "TryToCancelMatinee" if not already present
					if (commandValue.find(L"TryToCancelMatinee") == std::wstring::npos)
					{
						commandValue = L"TryToCancelMatinee | " + commandValue;
					}
				}

				if (commandValue != originalCommandValue)
				{
					str = str.substr(0, commandPos) + commandValue + str.substr(endQuotePos);
					wasModified = true;
				}
			}
		}
	}

	// Only allocate and update if the string was modified
	if (wasModified)
	{
		// Save original values for restoration
		g_pendingRestore.structAddr = ctx.ebx;
		g_pendingRestore.originalPtr = *(wchar_t**)(ctx.ebx + 0xC);
		g_pendingRestore.originalLength = *(int*)(ctx.ebx + 0x10);
		g_pendingRestore.originalCapacity = *(int*)(ctx.ebx + 0x14);
		g_pendingRestore.needsRestore = true;

		// Create our fixed string
		int newLength = str.length();
		auto fixedString = std::make_unique<wchar_t[]>(newLength + 1);
		wcscpy_s(fixedString.get(), newLength + 1, str.c_str());

		// Temporarily replace with our pointer
		*(wchar_t**)(ctx.ebx + 0xC) = fixedString.get();
		*(int*)(ctx.ebx + 0x10) = newLength;
		*(int*)(ctx.ebx + 0x14) = newLength;

		// Store to keep alive during memcpy
		g_tempFixedStrings.push_back(std::move(fixedString));
	}
	else
	{
		g_pendingRestore.needsRestore = false;
	}
}

static void InitializeTextureRedirects()
{
	g_textureRedirects =
	{
		// Face Buttons
		{"HUD_I64.tga", {"HUD_I67.tga", 11}},  // A -> Cross
		{"HUD_I5F.tga", {"HUD_I6B.tga", 11}},  // B -> Circle
		{"HUD_I12.tga", {"HUD_IF.tga",  10}},  // X -> Square
		{"HUD_I19.tga", {"HUD_I16.tga", 11}},  // Y -> Triangle

		{"weapons_I6C.tga", {"weapons_I69.tga", 15}},  // A -> Cross
		{"weapons_I65.tga", {"weapons_I62.tga", 15}},  // B -> Circle

		// Bumpers
		{"HUD_I30.tga", {"HUD_I2D.tga", 11}},  // LB -> L1
		{"HUD_I29.tga", {"HUD_I26.tga", 11}},  // RB -> R1

		// Stick Clicks
		{"HUD_I54.tga",  {"HUD_I78.tga",  11}},
		{"HUD_I5A.tga",  {"HUD_I75.tga",  11}},
		{"HUD_I6F.tga",  {"HUD_I78.tga",  11}},
		{"HUD_I72.tga",  {"HUD_I75.tga",  11}},
		{"HUD_I1DB.tga", {"HUD_I1D7.tga", 12}},

		// Menu Buttons
		{"HUD_I3E.tga", {"HUD_I43.tga", 11}},

		// D-Pad
		{"HUD_IB.tga", {"HUD_I8.tga", 10}},
	};
}


static bool IsUALPresent()
{
	for (const auto& entry : std::stacktrace::current()) 
	{
		HMODULE hModule = NULL;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)entry.native_handle(), &hModule)) 
		{
			if (GetProcAddress(hModule, "IsUltimateASILoader") != NULL)
				return true;
		}
	}

	return false;
}

#pragma endregion

#pragma region Hooks

// =========================
// FixHighFPSHairPhysics
// =========================

safetyhook::InlineHook HairSimulator;

static void __fastcall HairSimulator_Hook(void* thisPtr, int, float delta)
{
	g_State.frameTimeScale = TARGET_FRAME_TIME / delta;
	g_State.savedHairDeltaTime = delta;

	HairSimulator.unsafe_thiscall<void>(thisPtr, delta);
}

// ==========================
// FixHighFPSClothPhysics
// ==========================

safetyhook::InlineHook ClothSimulator;

static uint32_t __fastcall ClothSimulator_Hook(void* thisPtr, int, float delta)
{
	uint8_t* cloth = (uint8_t*)thisPtr;
	int numParticles = *(int*)(cloth + 0xB8);
	uint8_t* particles = *(uint8_t**)(cloth + 0xC4);

	if (!particles || numParticles <= 0)
		return ClothSimulator.unsafe_thiscall<uint32_t>(thisPtr, delta);

	ClothInstanceState& state = g_State.clothes[(uintptr_t)cloth];
	if ((int)state.trueP1.size() != numParticles * 3)
	{
		state.trueP1.assign(numParticles * 3, 0.0f);
		state.relPrev.assign(numParticles * 3, 0.0f);
		state.relCurr.assign(numParticles * 3, 0.0f);
		state.applied.assign(numParticles * 3, 0.0f);
		state.primed = false;
	}

	// Restore the simulation-true p1. 
	// If the engine rewrote p1 since we set it (teleport reset, instance respawn), adopt its value and resync instead
	if (state.primed)
	{
		for (int i = 0; i < numParticles; i++)
		{
			float* p1 = (float*)(particles + i * 0x70 + 0x10);
			float* p3 = (float*)(particles + i * 0x70 + 0x30);

			for (int j = 0; j < 3; j++)
			{
				int k = i * 3 + j;
				if (p1[j] == state.applied[k])
				{
					p1[j] = state.trueP1[k];
				}
				else
				{
					state.trueP1[k] = p1[j];
					state.relCurr[k] = p1[j] - p3[j];
					state.relPrev[k] = state.relCurr[k];
				}
			}
		}
	}

	state.accumulator += delta;

	if (!state.primed)
		state.accumulator = TARGET_FRAME_TIME; // first sight of this instance: tick now

	uint32_t result = 0;
	if (state.accumulator >= TARGET_FRAME_TIME)
	{
		state.relPrev = state.relCurr;
		result = ClothSimulator.unsafe_thiscall<uint32_t>(thisPtr, TARGET_FRAME_TIME);
		state.accumulator -= TARGET_FRAME_TIME;

		// Capture the post-tick anchor-relative state
		for (int i = 0; i < numParticles; i++)
		{
			float* p1 = (float*)(particles + i * 0x70 + 0x10);
			float* p3 = (float*)(particles + i * 0x70 + 0x30);

			for (int j = 0; j < 3; j++)
			{
				state.relCurr[i * 3 + j] = p1[j] - p3[j];
			}
		}

		if (!state.primed)
		{
			state.relPrev = state.relCurr;
			state.primed = true;
		}
	}

	// Write the render state: live anchor + interpolated relative motion
	float alpha = state.accumulator / TARGET_FRAME_TIME;
	const float* matrix = (const float*)cloth;

	for (int i = 0; i < numParticles; i++)
	{
		uint8_t* particle = particles + i * 0x70;
		float* p1 = (float*)(particle + 0x10);
		const float* localAnchor = (const float*)(particle + 0x20);

		for (int j = 0; j < 3; j++)
		{
			// anchor = p2.x * M0 + p2.y * M1 + p2.z * M2 + M3
			float anchor = localAnchor[0] * matrix[j] + localAnchor[1] * matrix[4 + j] + localAnchor[2] * matrix[8 + j] + matrix[12 + j];

			int k = i * 3 + j;
			state.trueP1[k] = p1[j];
			p1[j] = anchor + state.relPrev[k] + (state.relCurr[k] - state.relPrev[k]) * alpha;
			state.applied[k] = p1[j];
		}
	}

	return result;
}

// ======================================
// FixHighFPSProjectileCollisionCheck
// ======================================

static SafetyHookInline RangeAttackPawnCollisionCheck{};

static void __fastcall RangeAttackPawnCollisionCheck_Hook(int thisPtr, float DeltaTime)
{
	DeltaTime = TARGET_FRAME_TIME;
	RangeAttackPawnCollisionCheck.unsafe_fastcall<void>(thisPtr, DeltaTime);
}

// =============================
// FixHashTableRaceCondition
// =============================

safetyhook::InlineHook Localize;

static DWORD __cdecl Localize_Hook(DWORD* a1, void* a2, const wchar_t* a3, int a4, wchar_t* String1, int a6)
{
	// Fix a race condition
	static std::mutex locMutex;
	std::lock_guard<std::mutex> lock(locMutex);
	return Localize.unsafe_ccall<DWORD>(a1, a2, a3, a4, String1, a6);
}

static SafetyHookInline SetRenderingState{};

static void __cdecl SetRenderingState_Hook(int a1, int a2)
{
	if (a1 == 0)
	{
		// Rendering is paused
		g_State.set40fps = true;
	}
	else
	{
		// Rendering is resumed
		g_State.set40fps = false;
	}

	SetRenderingState.unsafe_ccall<void>(a1, a2);
}

static SafetyHookInline GetMaxTickRate{};

static double __fastcall GetMaxTickRate_Hook(int thisp, int, float a2, int a3)
{
	if (g_State.set40fps != g_State.prev_set40fps)
	{
		if (g_State.set40fps)
		{
			// Save original FPS before setting to 40 (slow down loading at high fps)
			g_State.original_fps = *(float*)(thisp + 0x4A8);
			*(float*)(thisp + 0x4A8) = 40.0f;
		}
		else
		{
			// Restore original FPS
			*(float*)(thisp + 0x4A8) = g_State.original_fps;
		}
		g_State.prev_set40fps = g_State.set40fps;
	}

	return GetMaxTickRate.unsafe_thiscall<double>(thisp, a2, a3);
}

// ======================
// FixExperimental
// ======================

static safetyhook::InlineHook SetObjectReference{};
static safetyhook::InlineHook IteratorNotEqualHook{};

static int __fastcall SetObjectReference_Hook(DWORD* thisp, int, int a2)
{
	if (!thisp)
		return 0;

	return SetObjectReference.unsafe_thiscall<int>(thisp, a2);
}

static bool __fastcall IteratorNotEqual_Hook(DWORD* thisp, int, DWORD* other)
{
	const uint32_t current = *thisp;

	if (current == 1)
		return false;

	return current != *other;
}

// ======================
// FixPhysX
// ======================

safetyhook::InlineHook PhysXLoad;

static SafetyHookMid physxCrashFix1{};
static SafetyHookMid physxCrashFix2{};

static int __cdecl PhysXLoad_Hook()
{
	int result = PhysXLoad.ccall<int>();

	HMODULE hModule = GetModuleHandleW(L"PhysXCore.dll");
	if (hModule != NULL)
	{
		g_State.physXCoreModuleAddress = reinterpret_cast<uintptr_t>(hModule);
		IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)(g_State.physXCoreModuleAddress);
		IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_State.physXCoreModuleAddress + dos->e_lfanew);
		DWORD timestamp = nt->FileHeader.TimeDateStamp;

		if (timestamp == 0x60ED9FB3) // from NVIDIA drivers
		{
			physxCrashFix1 = safetyhook::create_mid(g_State.physXCoreModuleAddress + 0x1D9D8,
				[](safetyhook::Context& ctx)
				{
					uint32_t ebp = ctx.ebp;
					int ptr = MemoryHelper::ReadMemory<int>(ebp - 0x4);

					if (!MemoryHelper::IsReadable((void*)ptr, 4))
					{
						ctx.eip = g_State.physXCoreModuleAddress + 0x1DAB8;
					}
				}
			);

			physxCrashFix2 = safetyhook::create_mid(g_State.physXCoreModuleAddress + 0x12E368,
				[](safetyhook::Context& ctx)
				{
					uint32_t ebp = ctx.ebp;
					int ptr = MemoryHelper::ReadMemory<int>(ebp - 0xC);

					if (!MemoryHelper::IsReadable((void*)ptr, 4))
					{
						ctx.eip = g_State.physXCoreModuleAddress + 0x12E49A;
					}
				}
			);
		}
		else if (timestamp == 0x4DC9B2B0) // from the game folder
		{
			physxCrashFix1 = safetyhook::create_mid(g_State.physXCoreModuleAddress + 0x1C806,
				[](safetyhook::Context& ctx)
				{
					int ptr = MemoryHelper::ReadMemory<int>(ctx.ebx);

					if (!MemoryHelper::IsReadable((void*)ptr, 4))
					{
						ctx.eip = g_State.physXCoreModuleAddress + 0x1C8EF;
					}
				}
			);

			physxCrashFix2 = safetyhook::create_mid(g_State.physXCoreModuleAddress + 0x127C8A,
				[](safetyhook::Context& ctx)
				{
					uint32_t ebp = ctx.ebp;
					int ptr = MemoryHelper::ReadMemory<int>(ebp - 0xC);

					if (!MemoryHelper::IsReadable((void*)ptr, 4))
					{
						ctx.eip = g_State.physXCoreModuleAddress + 0x127DBA;
					}
				}
			);
		}
	}

	return result;
}

// ======================
// FixInputBinding
// ======================

safetyhook::InlineHook LoadStartupPackages;

static void __fastcall LoadStartupPackages_Hook()
{
	DWORD addr_InputFix = ScanModuleSignature(g_State.GameModule, "8B FB 8B 47 10 50 8B CE E8", "InputFix");

	if (addr_InputFix == 0)
	{
		LoadStartupPackages.fastcall<void>();
		return;
	}

	// Before memcpy, hijack the string if needed
	static SafetyHookMid iniInputFix{};
	iniInputFix = safetyhook::create_mid(addr_InputFix,
		[](safetyhook::Context& ctx)
		{
			const wchar_t* keyName = *(const wchar_t**)(ctx.ebx + 0xC);

			if (keyName)
			{
				ReplaceConfigString(ctx, keyName);
			}
		}
	);

	// After memcpy, restore the original data
	static SafetyHookMid iniInputFixPtrRestore{};
	iniInputFixPtrRestore = safetyhook::create_mid(addr_InputFix + 0x24,
		[](safetyhook::Context& ctx)
		{
			// Check if we need to restore anything
			if (g_pendingRestore.needsRestore)
			{
				// Restore the original pointer and values
				uintptr_t structAddr = g_pendingRestore.structAddr;
				*(wchar_t**)(structAddr + 0xC) = g_pendingRestore.originalPtr;
				*(int*)(structAddr + 0x10) = g_pendingRestore.originalLength;
				*(int*)(structAddr + 0x14) = g_pendingRestore.originalCapacity;
				g_pendingRestore.needsRestore = false;
			}
		}
	);

	// Only called at startup
	LoadStartupPackages.fastcall<void>();

	(void)iniInputFix.disable();
	(void)iniInputFixPtrRestore.disable();
}

// =======================
// FixWindowHandling
// =======================

safetyhook::InlineHook ProcessDeferredMessage;

static void __fastcall ProcessDeferredMessage_Hook(int thisPtr, int, int deferredMessage)
{
	DWORD* msg = (DWORD*)deferredMessage;
	UINT& messageType = ((UINT*)msg)[1];
	DWORD wParam = msg[2];

	// Alt+F4 change to WM_CLOSE
	if (messageType == WM_SYSKEYDOWN && wParam == VK_F4)
	{
		messageType = WM_CLOSE;
	}

	ProcessDeferredMessage.unsafe_thiscall<void>(thisPtr, deferredMessage);
}

safetyhook::InlineHook UpdateMouseLock;

static LONG __fastcall UpdateMouseLock_Hook(int thisPtr, int)
{
	HWND gameWindow = *(HWND*)(thisPtr + 0x68);  // Get window handle
	HWND foregroundWindow = GetForegroundWindow();

	// Only call original if window has focus
	if (gameWindow == foregroundWindow)
	{
		return UpdateMouseLock.unsafe_thiscall<LONG>(thisPtr);
	}

	// Not focused, release clip instead
	ClipCursor(nullptr);
	return 0;
}

// ===========================
// MaxProcessorCount
// ===========================

safetyhook::InlineHook hkGetSystemInfo;
safetyhook::InlineHook hkGetNativeSystemInfo;
safetyhook::InlineHook hkGetProcessAffinityMask;
safetyhook::InlineHook hkGetLogicalProcessorInformation;

static DWORD_PTR MakeProcessorMask(int count)
{
	if (count >= (int)(sizeof(DWORD_PTR) * 8))
		return ~(DWORD_PTR)0;

	return (DWORD_PTR)((1ULL << count) - 1);
}

static void WINAPI GetSystemInfo_Hook(LPSYSTEM_INFO lpSystemInfo)
{
	hkGetSystemInfo.stdcall<void, LPSYSTEM_INFO>(lpSystemInfo);

	if (lpSystemInfo->dwNumberOfProcessors > (DWORD)MaxProcessorCount)
	{
		lpSystemInfo->dwNumberOfProcessors = (DWORD)MaxProcessorCount;
		lpSystemInfo->dwActiveProcessorMask = MakeProcessorMask(MaxProcessorCount);
	}
}

static void WINAPI GetNativeSystemInfo_Hook(LPSYSTEM_INFO lpSystemInfo)
{
	hkGetNativeSystemInfo.stdcall<void, LPSYSTEM_INFO>(lpSystemInfo);

	if (lpSystemInfo->dwNumberOfProcessors > (DWORD)MaxProcessorCount)
	{
		lpSystemInfo->dwNumberOfProcessors = (DWORD)MaxProcessorCount;
		lpSystemInfo->dwActiveProcessorMask = MakeProcessorMask(MaxProcessorCount);
	}
}

static BOOL WINAPI GetProcessAffinityMask_Hook(HANDLE hProcess, PDWORD_PTR lpProcessAffinityMask, PDWORD_PTR lpSystemAffinityMask)
{
	BOOL result = hkGetProcessAffinityMask.stdcall<BOOL, HANDLE, PDWORD_PTR, PDWORD_PTR>(hProcess, lpProcessAffinityMask, lpSystemAffinityMask);

	if (result)
	{
		DWORD_PTR mask = MakeProcessorMask(MaxProcessorCount);
		if (lpProcessAffinityMask)
			*lpProcessAffinityMask &= mask;

		if (lpSystemAffinityMask)
			*lpSystemAffinityMask &= mask;
	}

	return result;
}

static BOOL WINAPI GetLogicalProcessorInformation_Hook(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION Buffer, PDWORD ReturnedLength)
{
	BOOL result = hkGetLogicalProcessorInformation.stdcall<BOOL, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD>(Buffer, ReturnedLength);

	if (result && Buffer && ReturnedLength)
	{
		DWORD_PTR mask = MakeProcessorMask(MaxProcessorCount);
		DWORD count = *ReturnedLength / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
		DWORD writeIdx = 0;

		for (DWORD i = 0; i < count; i++)
		{
			Buffer[i].ProcessorMask &= mask;
			if (Buffer[i].ProcessorMask != 0)
			{
				if (writeIdx != i)
				{
					Buffer[writeIdx] = Buffer[i];
				}

				writeIdx++;
			}
		}

		*ReturnedLength = writeIdx * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
	}

	return result;
}

// =======================
// Ini settings override
// =======================

safetyhook::InlineHook GetStringHook;

static unsigned int __fastcall FConfigCacheIni_GetString_Hook(int* thisp, int, const wchar_t* Section, const wchar_t* Key, int* Value, const wchar_t* Filename)
{
	if (Key)
	{
		auto it = g_configOverrides.find(Key);
		if (it != g_configOverrides.end() && *it->second.isEnabled)
		{
			// Prepare replacement string
			const wchar_t* newValue = it->second.value.c_str();
			size_t newLen = wcslen(newValue);
			auto newString = std::make_unique<wchar_t[]>(newLen + 1);
			wcscpy_s(newString.get(), newLen + 1, newValue);

			g_replacementString = newString.get();
			g_tempConfigStrings.push_back(std::move(newString));
		}
		else
		{
			g_replacementString = nullptr;
		}
	}

	int result = GetStringHook.thiscall<unsigned int>(thisp, Section, Key, Value, Filename);

	// Clear after use
	g_replacementString = nullptr;
	return result;
}

static SafetyHookMid ConfigStringReplace{};
static SafetyHookInline UpdateD3DDeviceFromViewports{};

static void __fastcall UpdateD3DDeviceFromViewports_Hook(int thisp, int)
{
	UpdateD3DDeviceFromViewports.thiscall<void>(thisp);

	// Permanent ini settings overriden, we don't need those hooks anymore
	(void)GetStringHook.disable();
	(void)ConfigStringReplace.disable();
	(void)UpdateD3DDeviceFromViewports.disable();
}

// ==================
// SkipIntro
// ==================

static SafetyHookMid SkipMovie{};
safetyhook::InlineHook FFullScreenMovieBink_PlayMovie;

static unsigned int __fastcall FFullScreenMovieBink_PlayMovie_Hook(int* thisp, int, int a2, const wchar_t* MovieFilename, int a4, int a5, int a6, int a7, unsigned int a8, int a9, WORD* Src)
{
	if (MovieFilename)
	{
		if (_wcsicmp(MovieFilename, L"LoadingMovie.bik\x00") == 0)
		{
			(void)FFullScreenMovieBink_PlayMovie.disable();
			(void)SkipMovie.disable();
		}
		else if (SkipEAIntro && _wcsicmp(MovieFilename, L"Intro_EA.bik\x00") == 0)
		{
			g_State.shouldSkipMovie = true;
		}
		else if (SkipSHIntro && _wcsicmp(MovieFilename, L"Intro_SH.bik\x00") == 0)
		{
			g_State.shouldSkipMovie = true;
		}
		else if (SkipUEIntro && _wcsicmp(MovieFilename, L"TechLogo_Short.bik\x00") == 0)
		{
			g_State.shouldSkipMovie = true;
		}
	}

	return FFullScreenMovieBink_PlayMovie.unsafe_thiscall<unsigned int>(thisp, a2, MovieFilename, a4, a5, a6, a7, a8, a9, Src);
}

// =========================
// UseSDLControllerInput
// =========================

safetyhook::InlineHook XInputGetStateHook;
safetyhook::InlineHook XInputSetStateHook;

static DWORD WINAPI XInputGetState_Hook(DWORD dwUserIndex, XINPUT_STATE* pState)
{
	if (dwUserIndex != 0) return ERROR_DEVICE_NOT_CONNECTED;
	return ControllerHelper::PollController(pState, InvertABXYButtons);
}

static DWORD WINAPI XInputSetState_Hook(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration)
{
	if (dwUserIndex != 0) return ERROR_DEVICE_NOT_CONNECTED;
	return ControllerHelper::SetVibration(pVibration);
}

// =========================
// UsePS3ControllerIcons
// =========================

static SafetyHookInline GString_AssignString{};

static int __fastcall GString_AssignString_Hook(int thisp, int, char* Src, size_t Size)
{
	if (UsePS3ControllerIcons == 2 || (UsePS3ControllerIcons == 0 && ControllerHelper::GetGamepadStyle() == ControllerHelper::GamepadStyle::PlayStation))
	{
		auto it = g_textureRedirects.find(std::string_view(Src, Size));
		if (it != g_textureRedirects.end())
		{
			return GString_AssignString.unsafe_thiscall<int>(thisp, (void*)it->second.target, it->second.targetLen);
		}
	}

	return GString_AssignString.unsafe_thiscall<int>(thisp, Src, Size);
}

// ===========================
// DisableMouseAcceleration
// ===========================

safetyhook::InlineHook UpdateAxisValue;

static int __fastcall UpdateAxisValue_Hook(DWORD thisp, int, float* a2, float a3)
{
	// Update 'isUsingGamepad' flag
	if (a3 != 0.0f)
	{
		uint32_t flags = *(uint32_t*)(thisp + 0xE4);
		g_State.isUsingGamepad = (flags & 0x1) != 0;
	}

	return UpdateAxisValue.unsafe_thiscall<int>(thisp, a2, a3);
}

// ===========================
// FixUltraWideScreenFOV
// ===========================

safetyhook::InlineHook PlayAnimation;

static void __fastcall PlayAnimation_Hook(int thisp, int, int a2)
{
	DWORD currentValue = *(DWORD*)(thisp + 0x238);
	g_State.isCutscene = ((currentValue & 0x20) != 0) && ((currentValue & 0x04) != 0);

	PlayAnimation.unsafe_thiscall<void>(thisp, a2);
}

// ============================
// ImprovedTextureStreaming
// ============================

safetyhook::InlineHook GetWantedMips;

static int __stdcall GetWantedMips_Hook(int a1, int a2, int a3, int a4)
{
	// Skip the timer
	return a4;
}

// ==========================
// ForceHighResTextures
// ==========================

static SafetyHookInline ShouldMipLevelsBeForcedResident{};

static bool __fastcall ShouldMipLevelsBeForcedResident_Hook(int thisp, int)
{
	return true;
}

// =====================================
// FixUltraWideScreenFOV & FontScaling
// =====================================

safetyhook::InlineHook UpdateViewportRHI;

static void __fastcall UpdateViewportRHI_Hook(int thisp, int, int a2, int NewSizeX, int NewSizeY, bool bNewIsFullscreen)
{
	g_State.screenWidth = NewSizeX;
	g_State.screenHeight = NewSizeY;

	// baseHeight for subtitles = 768
	g_State.subtitlesScaleFactor = g_State.screenHeight / 768.0f;

	// Minimum 1.0x (768p and below)
	if (g_State.subtitlesScaleFactor < 1.0f) g_State.subtitlesScaleFactor = 1.0f;
	g_State.subtitlesScaleFactor *= FontScalingFactor;

	g_State.scaleFactor = g_State.screenWidth / g_State.screenHeight;

	if (FixUltraWideScreenFOV)
	{
		MemoryHelper::WriteMemory<float>(g_State.pGEngine + 0x4A4, g_State.scaleFactor, false);

		if (g_State.scaleFactor > ASPECT_RATIO_16_9)
		{
			g_State.updateFOV = true;
			g_State.FOVScale = ScaleFOV(90.0f, ASPECT_RATIO_16_9, g_State.scaleFactor);
		}
		else
		{
			g_State.updateFOV = false;
		}
	}

	UpdateViewportRHI.thiscall<void>(thisp, a2, NewSizeX, NewSizeY, bNewIsFullscreen);
}

#pragma endregion

#pragma region Init

static void ApplyFixHighFPSHairPhysics()
{
	if (!FixHighFPSHairPhysics) return;

	DWORD addr_HairSimulator = ScanModuleSignature(g_State.GameModule, "53 8B DC 51 83 E4 F0 83 C4 04 55 8B EC 81 EC E8 00 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 56 8B F1 57 8D 8D 20 FF FF FF", "HairSimulator");
	DWORD addr_DampingScaler = ScanModuleSignature(g_State.GameModule, "D9 FC 0F 29 55 B0", "HairSimulator_DampingScaler");
	DWORD addr_DeltaTimeOverride = ScanModuleSignature(g_State.GameModule, "D9 43 08 B9 30 00 00 00 8D BD 20 FF FF FF", "HairSimulator_DeltaTimeOverride");

	if (addr_HairSimulator == 0 ||
		addr_DampingScaler == 0 ||
		addr_DeltaTimeOverride == 0) {
		return;
	}

	HairSimulator = HookHelper::CreateHook((void*)addr_HairSimulator, &HairSimulator_Hook);

	static SafetyHookMid hairDeltaTimeOverride{};
	hairDeltaTimeOverride = safetyhook::create_mid(addr_DeltaTimeOverride,
		[](safetyhook::Context& ctx) 
		{
			float* dt = reinterpret_cast<float*>(ctx.ebx + 0x8);
			*dt *= g_State.frameTimeScale;
		}
	);

	static SafetyHookMid hairDeltaTimeRestore{};
	hairDeltaTimeRestore = safetyhook::create_mid(addr_DeltaTimeOverride + 0x8,
		[](safetyhook::Context& ctx) 
		{
			float* dt = reinterpret_cast<float*>(ctx.ebx + 0x8);
			*dt = g_State.savedHairDeltaTime;
		}
	);

	static SafetyHookMid hairGravityAttenuate{};
	hairGravityAttenuate = safetyhook::create_mid(addr_DampingScaler,
		[](safetyhook::Context& ctx)
		{
			float k = 1.0f / g_State.frameTimeScale;
			ctx.xmm2.f32[0] *= k;
			ctx.xmm2.f32[1] *= k;
			ctx.xmm2.f32[2] *= k;
		}
	);

	static SafetyHookMid hairWindAttenuate{};
	hairWindAttenuate = safetyhook::create_mid(addr_DampingScaler + 0x6E,
		[](safetyhook::Context& ctx)
		{
			float k = 1.0f / g_State.frameTimeScale;
			ctx.xmm6.f32[0] *= k;
			ctx.xmm6.f32[1] *= k;
			ctx.xmm6.f32[2] *= k;
		}
	);

	static SafetyHookMid hairShapeMatchDecompound{};
	hairShapeMatchDecompound = safetyhook::create_mid(addr_DampingScaler + 0x179,
		[](safetyhook::Context& ctx) 
		{
			float f30 = ctx.xmm3.f32[0];
			float scale = g_State.frameTimeScale;
			float remain = 1.0f - f30;
			if (remain < 0.0f) remain = 0.0f;
			float fNew = 1.0f - std::pow(remain, 1.0f / scale);
			ctx.xmm3.f32[0] = fNew;
		}
	);

	static SafetyHookMid hairInnerPhaseFix{};
	hairInnerPhaseFix = safetyhook::create_mid(addr_DampingScaler + 0x4B2,
		[](safetyhook::Context& ctx) 
		{
			float* innerDt = reinterpret_cast<float*>(ctx.ebx + 0x8);
			*innerDt = g_State.savedHairDeltaTime;
		}
	);
}

static void ApplyFixHighFPSClothPhysics()
{
	if (!FixHighFPSClothPhysics) return;

	DWORD addr_ClothSimulator = ScanModuleSignature(g_State.GameModule, "53 8B DC 51 83 E4 F0 83 C4 04 55 8B EC 81 EC D8 00 00 00 F3 0F 10 4B 08 8B D1", "ClothSimulator");

	if (addr_ClothSimulator == 0) return;

	ClothSimulator = HookHelper::CreateHook((void*)addr_ClothSimulator, &ClothSimulator_Hook);
}

static void ApplyFixHighFPSProjectileCollisionCheck()
{
	if (!FixHighFPSProjectileCollisionCheck) return;

	DWORD addr_RangeAttackPawnCollisionCheck = ScanModuleSignature(g_State.GameModule, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 53 81 EC C8 01 00 00 56 57 A1 ?? ?? ?? ?? 33 C5 50 8D 45 F4 64 A3 00 00 00 00 A1", "RangeAttackPawnCollisionCheck");

	if (addr_RangeAttackPawnCollisionCheck == 0) return;

	RangeAttackPawnCollisionCheck = HookHelper::CreateHook((void*)addr_RangeAttackPawnCollisionCheck, &RangeAttackPawnCollisionCheck_Hook);
}

static void ApplyFixHighFPSRagdollDeath()
{
	if (!FixHighFPSRagdollDeath) return;

	DWORD addr_RagdollDeath = ScanModuleSignature(g_State.GameModule, "8B ?? 28 02 00 00 8B ?? 14 02 00 00 6A 01 50 6A 01 6A 01", "RagdollDeath");

	if (addr_RagdollDeath == 0) return;

	MemoryHelper::MakeNOP(addr_RagdollDeath, 0x18);
}

static void ApplyFixHashTableRaceCondition()
{
	if (!FixHashTableRaceCondition) return;

	DWORD addr_Localize = ScanModuleSignature(g_State.GameModule, "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 83 EC 2C 53 56 57 A1 ?? ?? ?? ?? 33 C5 50 8D 45 F4 64 A3 00 00 00 00 33 DB 89 5D EC 39 1D", "Localize");
	DWORD addr_hashLoop = ScanModuleSignature(g_State.GameModule, "83 C4 08 85 C0 74 1B 8B 03 8B 7C 06 54 83 FF FF 75 BC 8B 45 08 5F 5E C7 00 FF FF FF FF 5B 5D C2 08 00 8B 45 08 89 38 5F 5E 5B 5D C2 08", "HashLoop");
	DWORD addr_SetRenderingState = ScanModuleSignature(g_State.GameModule, "6A 02 6A 01 E8 ?? ?? ?? ?? 83 C4 08 C3", "SetRenderingState");
	addr_SetRenderingState = MemoryHelper::ResolveRelativeAddress(addr_SetRenderingState, 0x5);
	DWORD addr_SetFPSRate = ScanModuleSignature(g_State.GameModule, "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 83 EC 14 56 A1 ?? ?? ?? ?? 33 C5 50 8D 45 F4 64 A3 00 00 00 00 8B F1 C7 45 EC 00 00 00 00 F7", "GetMaxTickRate");

	if (addr_hashLoop == 0 ||
		addr_SetRenderingState == 0 ||
		addr_SetFPSRate == 0) {
		return;
	}

	Localize = HookHelper::CreateHook((void*)addr_Localize, &Localize_Hook);
	SetRenderingState = HookHelper::CreateHook((void*)addr_SetRenderingState, &SetRenderingState_Hook);
	GetMaxTickRate = HookHelper::CreateHook((void*)addr_SetFPSRate, &GetMaxTickRate_Hook);
	MemoryHelper::MakeNOP(addr_hashLoop + 0x10, 2);
}

static void ApplyFixUnattachedCollisionPanic()
{
	if (!FixUnattachedCollisionPanic) return;

	DWORD addr_FixUnattachedCollisionPanic = ScanModuleSignature(g_State.GameModule, "0F 85 93 00 00 00 8D 85 10 FF FF FF 50 E8", "FixUnattachedCollisionPanic");

	if (addr_FixUnattachedCollisionPanic == 0) return;

	static uintptr_t s_unattached_collision_skip = 0;
	s_unattached_collision_skip = addr_FixUnattachedCollisionPanic + 0x1B0B;

	static SafetyHookMid UnattachedCollisionGuard{};
	UnattachedCollisionGuard = safetyhook::create_mid(addr_FixUnattachedCollisionPanic + 0x6,
		[](safetyhook::Context& ctx)
		{
			*reinterpret_cast<uint32_t*>(ctx.esi + 0x3C) |= 0x80;
			ctx.eax = 0;
			ctx.eip = s_unattached_collision_skip;
		});
}

static void ApplyFixHairCurveOverRead()
{
	if (!FixHairCurveOverRead) return;

	DWORD addr_FixHairCurveOverRead = ScanModuleSignature(g_State.GameModule, "74 0E 83 C1 50 3B CE 8B 4D E0 74 04", "FixHairCurveOverRead");

	if (addr_FixHairCurveOverRead == 0) return;

	MemoryHelper::WriteMemory<uint8_t>(addr_FixHairCurveOverRead, 0x73);
}

static void ApplyFixExperimental()
{
	if (!FixExperimental) return;

	DWORD addr_MmxOverRead = ScanModuleSignature(g_State.GameModule, "83 EC 10 0F 28 0D ?? ?? ?? ?? 8B 43 08 0F EF C0 0F 6F 09", "MmxOverRead");
	DWORD addr_SetObjectReference = ScanModuleSignature(g_State.GameModule, "55 8B EC 83 EC 44 89 4D BC C6 45 FF 00 8B 45 BC", "SetObjectReference");
	DWORD addr_IteratorNotEqual = ScanModuleSignature(g_State.GameModule, "55 8B EC 51 89 4D FC 8B 45 FC 8B 4D 08 8B 10", "IteratorNotEqual");
	DWORD addr_ListWalkSentinel = ScanModuleSignature(g_State.GameModule, "3B C3 74 06 8B C8 3B CB 75 F3 F6 81 DC 00 00 00 04", "ListWalkSentinel");

	if (addr_MmxOverRead == 0 ||
		addr_SetObjectReference == 0 ||
		addr_IteratorNotEqual == 0 ||
		addr_ListWalkSentinel == 0) {
		return;
	}

	MemoryHelper::WriteMemory<uint8_t>(addr_MmxOverRead + 0x11, 0x6E);
	SetObjectReference = HookHelper::CreateHook((void*)addr_SetObjectReference, &SetObjectReference_Hook);
	IteratorNotEqualHook = HookHelper::CreateHook((void*)addr_IteratorNotEqual, &IteratorNotEqual_Hook);

	static SafetyHookMid ListWalkSentinelGuard{};
	ListWalkSentinelGuard = safetyhook::create_mid((void*)(addr_ListWalkSentinel),
		[](safetyhook::Context& ctx)
		{
			if (ctx.eax == 1)
				ctx.eax = 0;
		});
}

static void ApplyFixPhysX()
{
	if (!FixPhysX) return;

	DWORD addr_PhysXLoad = ScanModuleSignature(g_State.GameModule, "55 8B EC 83 EC ?? 53 56 57 68 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 6A 08 6A 04", "PhysXLoad");

	if (addr_PhysXLoad == 0) {
		return;
	}

	PhysXLoad = HookHelper::CreateHook((void*)addr_PhysXLoad, &PhysXLoad_Hook);
}

static void ApplyFixInputBinding()
{
	if (!FixInputBinding) return;

	DWORD addr_LoadStartupPackages = ScanModuleSignature(g_State.GameModule, "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 83 EC ?? 53 56 57 A1 ?? ?? ?? ?? 33 C5 50 8D 45 F4 64 A3 00 00 00 00 8D 45 ?? 50 FF 15", "LoadStartupPackages");

	if (addr_LoadStartupPackages != 0)
	{
		LoadStartupPackages = HookHelper::CreateHook((void*)addr_LoadStartupPackages, &LoadStartupPackages_Hook);
	}
}

static void ApplyFixWindowHandling()
{
	if (!FixWindowHandling) return;

	DWORD addr_UpdateMouseLock = ScanModuleSignature(g_State.GameModule, "55 8B EC 83 EC 24 53 56 57 8B F1 FF 15", "UpdateMouseLock");
	DWORD addr_ProcessDeferredMessage = ScanModuleSignature(g_State.GameModule, "8B 11 8D 46 04 50 8B 42 5C FF D0", "ProcessDeferredMessage", 3);

	// Different patterns for different builds
	DWORD addr_BlockHookV1 = ScanModuleSignature(g_State.GameModule, "68 ?? ?? ?? ?? 53 53 68 ?? ?? ?? ?? 53 53 FF 15", "BlockHookV1", -1, false);
	DWORD addr_BlockHookV2 = ScanModuleSignature(g_State.GameModule, "68 ?? ?? ?? ?? 33 F6 56 56 68 ?? ?? ?? ?? 56 56 FF 15", "BlockHookV2", -1, false);

	DWORD addr_BlockMessages_1V1 = ScanModuleSignature(g_State.GameModule, "8B 15 ?? ?? ?? ?? 53 53 68 00 04 00 00 52 FF 15", "BlockMessages_1V1", -1, false);
	DWORD addr_BlockMessages_1V2 = ScanModuleSignature(g_State.GameModule, "A1 ?? ?? ?? ?? 6A 00 6A 00 68 00 04 00 00 50 FF 15", "BlockMessages_1V2", -1, false);
	DWORD addr_BlockMessages_2V1 = ScanModuleSignature(g_State.GameModule, "A1 ?? ?? ?? ?? 6A 00 6A 01 68 00 04 00 00 50 FF 15", "BlockMessages_2V1", -1, false);
	DWORD addr_BlockMessages_2V2 = ScanModuleSignature(g_State.GameModule, "A1 ?? ?? ?? ?? 52 6A 01 68 00 04 00 00 50 FF 15", "BlockMessages_2V2", -1, false);

	if (addr_UpdateMouseLock == 0 ||
		addr_ProcessDeferredMessage == 0) {
		return;
	}

	UpdateMouseLock = HookHelper::CreateHook((void*)addr_UpdateMouseLock, &UpdateMouseLock_Hook);
	ProcessDeferredMessage = HookHelper::CreateHook((void*)addr_ProcessDeferredMessage, &ProcessDeferredMessage_Hook);

	// Block hook creation
	if (addr_BlockHookV1)
	{
		MemoryHelper::MakeNOP(addr_BlockHookV1, 0x14);
		MemoryHelper::MakeNOP(addr_BlockHookV1 + 0x2B, 0x5);
	}
	else if (addr_BlockHookV2)
	{
		MemoryHelper::MakeNOP(addr_BlockHookV2, 0x5);
		MemoryHelper::MakeNOP(addr_BlockHookV2 + 0x7, 0xF);
		MemoryHelper::MakeNOP(addr_BlockHookV2 + 0x2D, 0x5);
	}
	else
	{
		MessageBoxA(NULL, "Error: Unable to find signature for patch: BlockHook", "", MB_ICONERROR);
	}

	// Block thread messages
	if (addr_BlockMessages_1V1)
	{
		MemoryHelper::MakeNOP(addr_BlockMessages_1V1, 0x14);
	}
	else if (addr_BlockMessages_1V2)
	{
		MemoryHelper::MakeNOP(addr_BlockMessages_1V2, 0x15);
	}
	else
	{
		MessageBoxA(NULL, "Error: Unable to find signature for patch: BlockMessages_1", "", MB_ICONERROR);
	}

	if (addr_BlockMessages_2V1)
	{
		MemoryHelper::MakeNOP(addr_BlockMessages_2V1, 0x15);
		MemoryHelper::MakeNOP(addr_BlockMessages_2V1 + 0x23, 0x16);
	}
	else if (addr_BlockMessages_2V2)
	{
		MemoryHelper::MakeNOP(addr_BlockMessages_2V2, 0x14);
		MemoryHelper::MakeNOP(addr_BlockMessages_2V2 + 0x22, 0x16);
	}
	else
	{
		MessageBoxA(NULL, "Error: Unable to find signature for patch: BlockMessages_2", "", MB_ICONERROR);
	}
}

static void ApplyFixCPUCores()
{
	if (MaxProcessorCount == -1) return;

	hkGetSystemInfo = HookHelper::CreateHookAPI(L"kernel32.dll", "GetSystemInfo", &GetSystemInfo_Hook);
	hkGetNativeSystemInfo = HookHelper::CreateHookAPI(L"kernel32.dll", "GetNativeSystemInfo", &GetNativeSystemInfo_Hook);
	hkGetProcessAffinityMask = HookHelper::CreateHookAPI(L"kernel32.dll", "GetProcessAffinityMask", &GetProcessAffinityMask_Hook);
	hkGetLogicalProcessorInformation = HookHelper::CreateHookAPI(L"kernel32.dll", "GetLogicalProcessorInformation", &GetLogicalProcessorInformation_Hook);
}

static void ApplyIniSettingsHook()
{
	DWORD addr_GetStringHook = ScanModuleSignature(g_State.GameModule, "55 8B EC 8B 45 14 83 EC 18 56 57 33 FF 57 50 E8", "GetStringHook");
	DWORD addr_UpdateD3DDeviceFromViewports = ScanModuleSignature(g_State.GameModule, "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 81 EC ?? 00 00 00 53 56 57 A1 ?? ?? ?? ?? 33 C5 50 8D 45 F4 64 A3 00 00 00 00 8B F1 6A 01 8D 4D", "UpdateD3DDeviceFromViewports");
	DWORD addr_ConfigStringReplace = ScanModuleSignature(g_State.GameModule, "56 E8 ?? ?? ?? ?? 5F B8 01 00 00 00 5E 8B E5 5D C2 10 00", "ConfigStringReplace");

	if (addr_GetStringHook == 0 ||
		addr_UpdateD3DDeviceFromViewports == 0 ||
		addr_ConfigStringReplace == 0) {
		return;
	}

	GetStringHook = HookHelper::CreateHook((void*)addr_GetStringHook, &FConfigCacheIni_GetString_Hook);
	UpdateD3DDeviceFromViewports = HookHelper::CreateHook((void*)addr_UpdateD3DDeviceFromViewports, &UpdateD3DDeviceFromViewports_Hook);

	ConfigStringReplace = safetyhook::create_mid(addr_ConfigStringReplace,
		[](safetyhook::Context& ctx)
		{
			if (g_replacementString)
			{
				ctx.esi = (uintptr_t)g_replacementString;
			}
		}
	);
}

static void ApplyIntroSkip()
{
	if (!SkipEAIntro && !SkipSHIntro && !SkipUEIntro) return;

	DWORD addr_PlayMovie = ScanModuleSignature(g_State.GameModule, "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 81 EC 2C 01 00 00 53 56 57 A1 ?? ?? ?? ?? 33 C5 50 8D 45 F4 64 A3 00 00 00 00 8B F1 89 75 E4 8B 8E C0 00 00 00", "PlayMovie");
	DWORD addr_SkipMovie = ScanModuleSignature(g_State.GameModule, "3B C7 0F 85 D0 00 00 00 6A 01 8B CB E8", "SkipMovie");

	if (addr_PlayMovie == 0 ||
		addr_SkipMovie == 0) {
		return;
	}

	FFullScreenMovieBink_PlayMovie = HookHelper::CreateHook((void*)addr_PlayMovie, &FFullScreenMovieBink_PlayMovie_Hook);

	SkipMovie = safetyhook::create_mid(addr_SkipMovie,
		[](safetyhook::Context& ctx)
		{
			if (g_State.shouldSkipMovie)
			{
				ctx.eax = 0;
				g_State.shouldSkipMovie = false;
			}
		}
	);
}

static void ApplyCheckAlice1InstallFolder()
{
	if (!CheckAlice1InstallFolder) return;

	DWORD addr_CheckAlice1InstallFolder1 = ScanModuleSignature(g_State.GameModule, "A1 ?? ?? ?? ?? 75 ?? B8 ?? ?? ?? ?? 50 FF 15", "CheckAlice1InstallFolder_1", -1, false);
	if (!addr_CheckAlice1InstallFolder1)
	{
		// DRM Builds
		addr_CheckAlice1InstallFolder1 = ScanModuleSignature(g_State.GameModule, "75 05 B8 ?? ?? ?? ?? 50 68 ?? ?? ?? ?? E8 ?? ?? ?? FF 83 C4 08", "CheckAlice1InstallFolder_2");

		if (addr_CheckAlice1InstallFolder1)
		{
			addr_CheckAlice1InstallFolder1 -= 0x12;
		}
	}
	else
	{
		addr_CheckAlice1InstallFolder1 += 0xC;
	}

	if (addr_CheckAlice1InstallFolder1 == 0) return;

	static SafetyHookMid checkAlice1{};
	checkAlice1 = safetyhook::create_mid(addr_CheckAlice1InstallFolder1,
		[](safetyhook::Context& ctx)
		{
			const wchar_t* pathStr = (const wchar_t*)ctx.eax;

			if (pathStr)
			{
				wchar_t currentDir[MAX_PATH];
				GetCurrentDirectoryW(MAX_PATH, currentDir);

				wchar_t fullPath[MAX_PATH];
				if (PathIsRelativeW(pathStr))
				{
					PathCombineW(fullPath, currentDir, pathStr);
				}
				else
				{
					wcscpy_s(fullPath, MAX_PATH, pathStr);
				}

				wchar_t canonicalPath[MAX_PATH];
				if (PathCanonicalizeW(canonicalPath, fullPath))
				{
					wcscpy_s(fullPath, MAX_PATH, canonicalPath);
				}

				DWORD attribs = GetFileAttributesW(fullPath);
				if (attribs == INVALID_FILE_ATTRIBUTES || !(attribs & FILE_ATTRIBUTE_DIRECTORY))
				{
					std::wstring errorMsg = L"Alice1 Directory Not Found!\n\n";
					errorMsg += L"The game is trying to set the working directory to a folder that doesn't exist.\n\n";
					errorMsg += L"Alice1Path Configuration ([AliceGame.AliceGameEngine]):\n";
					errorMsg += pathStr;
					errorMsg += L"\n\n";
					errorMsg += L"Resolved Full Path:\n";
					errorMsg += fullPath;
					errorMsg += L"\n\n";
					errorMsg += L"Please verify your Alice1 installation path in the configuration.";

					MessageBoxW(NULL, errorMsg.c_str(), L"Alice1 Directory Error", MB_OK | MB_ICONWARNING | MB_TOPMOST);
				}
			}
		}
	);
}

static void ApplyFontScaling()
{
	if (!FontScaling) return;

	DWORD addr_HeightFactor = ScanModuleSignature(g_State.GameModule, "D9 45 08 51 8D 45 E0 D9 1C 24 50 8D 4D 08", "FontScaling_HeightFactor");
	DWORD addr_Size = ScanModuleSignature(g_State.GameModule, "33 FF F6 86 20 01 00 00 01 89 55 AC", "FontScaling_Size");
	DWORD addr_LayoutMetrics = ScanModuleSignature(g_State.GameModule, "D9 45 E8 8B 77 1C 51 F3 0F 59 C1", "FontScaling_LayoutMetrics");
	DWORD addr_LineSpacing = ScanModuleSignature(g_State.GameModule, "0F 88 BC 01 00 00", "FontScaling_LineSpacing");

	if (addr_HeightFactor == 0 ||
		addr_Size == 0 ||
		addr_LayoutMetrics == 0 ||
		addr_LineSpacing == 0) {
		return;
	}

	static SafetyHookMid scaleHeightFactor{};
	scaleHeightFactor = safetyhook::create_mid(addr_HeightFactor,
		[](safetyhook::Context& ctx)
		{
			uint32_t ebp = ctx.ebp;
			float* scaling = (float*)(ebp - 0x1C);

			*scaling = g_State.subtitlesScaleFactor;
		}
	);

	static SafetyHookMid scaleSize{};
	scaleSize = safetyhook::create_mid(addr_Size,
		[](safetyhook::Context& ctx)
		{
			uint32_t ebx = ctx.ebx;
			float* scaling1 = (float*)(ebx + 0x24);
			float* scaling2 = (float*)(ebx + 0x28);

			*scaling1 = *scaling1 * g_State.subtitlesScaleFactor;
			*scaling2 = *scaling2 * g_State.subtitlesScaleFactor;
		}
	);

	static SafetyHookMid scaleLayoutMetrics{};
	scaleLayoutMetrics = safetyhook::create_mid(addr_LayoutMetrics,
		[](safetyhook::Context& ctx)
		{
			ctx.xmm1.f32[0] = g_State.subtitlesScaleFactor;
		}
	);

	static SafetyHookMid scaleLineSpacing{};
	scaleLineSpacing = safetyhook::create_mid(addr_LineSpacing,
		[](safetyhook::Context& ctx)
		{
			uint32_t ebp = ctx.ebp;
			float* scaling = (float*)(ebp - 0x14);

			*scaling = *scaling * g_State.subtitlesScaleFactor;
		}
	);
}

static void ApplyAutoResolution()
{
	if (!AutoResolution) return;

	DWORD addr_DocPath = ScanModuleSignature(g_State.GameModule, "C6 45 FC 0B 89 9D ?? FD FF FF 89 9D ?? FD FF FF 3B C3", "DocPath");

	if (addr_DocPath == 0) return;

	static SafetyHookMid applyAutoResolution{};
	applyAutoResolution = safetyhook::create_mid(addr_DocPath,
		[](safetyhook::Context& ctx)
		{
			const wchar_t* docPath = reinterpret_cast<const wchar_t*>(ctx.eax);

			if (docPath)
			{
				// Check if the path exists
				if (std::filesystem::exists(docPath))
				{
					AutoResolution = false;
				}
			}

			if (AutoResolution)
			{
				auto [screenWidth, screenHeight] = SystemHelper::GetScreenResolution();
				UpdateConfigInt(L"ResX", screenWidth);
				UpdateConfigInt(L"ResY", screenHeight);
			}
		}
	);
}

static void ApplyUseSDLControllerInput()
{
	if (!UseSDLControllerInput) return;

	ControllerHelper::InitializeSDLGamepad();
	XInputGetStateHook = HookHelper::CreateHookAPI(L"XINPUT1_3.dll", "XInputGetState", &XInputGetState_Hook);
	XInputSetStateHook = HookHelper::CreateHookAPI(L"XINPUT1_3.dll", "XInputSetState", &XInputSetState_Hook);
}

static void ApplyUsePS3ControllerIcons()
{
	if (UsePS3ControllerIcons == 1 || (UsePS3ControllerIcons == 0 && !UseSDLControllerInput)) return;

	DWORD addr_AssignString = ScanModuleSignature(g_State.GameModule, "56 8B F1 8B 0E 57 8B F9 33 C0 83 E1 03 83 E7 FC", "AssignString");

	if (addr_AssignString != 0)
	{
		GString_AssignString = HookHelper::CreateHook((void*)addr_AssignString, &GString_AssignString_Hook);
		InitializeTextureRedirects();
	}
}

static void ApplyDisableMouseAcceleration()
{
	if (!DisableMouseAcceleration && !DisableControllerAcceleration) return;

	DWORD addr_UpdateAxisValue = ScanModuleSignature(g_State.GameModule, "55 8B EC F3 0F 10 45 0C 0F 2E 05", "UpdateAxisValue");
	DWORD addr_EngineVMOutput = ScanModuleSignature(g_State.GameModule, "F3 0F 58 45 08 8B 45 0C F3 0F 11 07 5F", "EngineVMOutput");

	if (addr_UpdateAxisValue == 0 ||
		addr_EngineVMOutput == 0) {
		return;
	}

	if (!DisableControllerAcceleration)
	{
		UpdateAxisValue = HookHelper::CreateHook((void*)addr_UpdateAxisValue, &UpdateAxisValue_Hook);
	}

	static SafetyHookMid scriptEngineVMOutput{};
	scriptEngineVMOutput = safetyhook::create_mid(addr_EngineVMOutput + 0xC,
		[](safetyhook::Context& ctx)
		{
			uint32_t edi = ctx.edi;

			if (!g_State.isUsingGamepad && g_State.pInput != 0 && ((g_State.pInput + 0x69C) == edi || (g_State.pInput + 0x6A0) == edi))
			{
				// Make the game think the camera has already moved
				*(float*)(edi) = 10.0f;
			}
		}
	);
}

static void ApplyFixUltraWideScreenFOV()
{
	if (!FixUltraWideScreenFOV) return;

	DWORD addr_PlayAnimation = ScanModuleSignature(g_State.GameModule, "55 8B EC 53 8B 5D 08 56 57 8B F9 8B 87 28 02 00 00", "PlayAnimation");
	DWORD addr_fovFix = ScanModuleSignature(g_State.GameModule, "D9 00 8B 4D 08 D9 19 5D C2 14 00 8B 51 50", "fovFix");

	if (addr_PlayAnimation == 0 ||
		addr_fovFix == 0) {
		return;
	}

	PlayAnimation = HookHelper::CreateHook((void*)addr_PlayAnimation, &PlayAnimation_Hook);

	static SafetyHookMid fovFix{};
	fovFix = safetyhook::create_mid(addr_fovFix,
		[](safetyhook::Context& ctx)
		{
			static float lastFOV = 0.0f;
			uint32_t eax = ctx.eax;

			if (g_State.updateFOV && g_State.pFOV != 0 && eax == g_State.pFOV)
			{
				float currentFOV = MemoryHelper::ReadMemory<float>(g_State.pFOV, false);

				if (currentFOV != lastFOV)
				{
					// London uses a different base FOV
					float scaledFOV = (currentFOV == 70.0f) ? ScaleFOV(currentFOV, ASPECT_RATIO_16_9, g_State.scaleFactor) : g_State.FOVScale;

					MemoryHelper::WriteMemory<float>(g_State.pFOV, scaledFOV, false);
					lastFOV = scaledFOV;
				}
			}

			if (g_State.isCutscene && g_State.updateFOV && g_State.pFOVCut != 0 && eax == g_State.pFOVCut)
			{
				MemoryHelper::WriteMemory<float>(g_State.pFOVCut, g_State.FOVScale, false);
			}
		}
	);
}

static void ApplyImprovedTextureStreaming()
{
	if (ForceHighResTextures)
	{
		DWORD addr_ShouldMipLevelsBeForcedResident = ScanModuleSignature(g_State.GameModule, "55 8B EC 83 EC 08 56 8B F1 F6 86 18 01 00 00 18", "ShouldMipLevelsBeForcedResident");

		if (addr_ShouldMipLevelsBeForcedResident != 0)
		{
			ShouldMipLevelsBeForcedResident = HookHelper::CreateHook((void*)addr_ShouldMipLevelsBeForcedResident, &ShouldMipLevelsBeForcedResident_Hook);
		}
	}
	else if (ImprovedTextureStreaming)
	{
		// This is never called when 'ShouldMipLevelsBeForcedResident' is forced to true
		DWORD addr_GetWantedMips = ScanModuleSignature(g_State.GameModule, "55 8B EC 8B 45 08 DD 05", "GetWantedMips");

		if (addr_GetWantedMips != 0)
		{
			GetWantedMips = HookHelper::CreateHook((void*)addr_GetWantedMips, &GetWantedMips_Hook);
		}
	}
}

static void ApplyReducedMipMapBias()
{
	if (!ReducedMipMapBias) return;

	DWORD addr_MipMapBias = ScanModuleSignature(g_State.GameModule, "50 8B 82 14 01 00 00 6A 08 56 51 FF D0 0F 57 C9", "MipMapBias");

	if (addr_MipMapBias == 0) return;

	static SafetyHookMid mipmapbias{};
	mipmapbias = safetyhook::create_mid(addr_MipMapBias,
		[](safetyhook::Context& ctx)
		{
			ctx.eax = (float)-0.5f;
		}
	);
}

static void ApplyDisableBackgroundLevelStreaming()
{
	if (!DisableBackgroundLevelStreaming) return;

	DWORD addr_BackgroundLevelStreaming = ScanModuleSignature(g_State.GameModule, "83 C4 10 8D 8D 60 FF FF FF", "BackgroundLevelStreaming");

	if (addr_BackgroundLevelStreaming == 0) return;

	static SafetyHookMid BackgroundLevelStreamingHook{};
	BackgroundLevelStreamingHook = safetyhook::create_mid(addr_BackgroundLevelStreaming - 0x5,
		[](safetyhook::Context& ctx)
		{
			const wchar_t* str = *(const wchar_t**)(ctx.eax);
			if (str && (wcscmp(str, L"AliceEntryExtra") == 0 || wcscmp(str, L"AliceEntryManual") == 0))
			{
				*(uint32_t*)(g_State.pGEngine + 0x2A8) |= 0x100;
			}
			else
			{
				*(uint32_t*)(g_State.pGEngine + 0x2A8) &= ~0x100u;
			}
		}
	);
}

static void ApplyFixBinkVideoBT709()
{
	if (!FixBinkVideoBT709) return;

	DWORD addr_Gyuvtorgb = ScanModuleSignature(g_State.GameModule, "00 02 95 3F 00 43 CC 3F 00 00 00 00 40 E3 5E BF", "Gyuvtorgb");

	if (addr_Gyuvtorgb == 0) return;

	uint8_t BT709_Matrix[] =
	{
		// tor
		0x00, 0x02, 0x95, 0x3F,
		0xE1, 0x7A, 0xE5, 0x3F,
		0x00, 0x00, 0x00, 0x00,
		0x85, 0xEB, 0x78, 0xBF,

		// tog
		0x00, 0x02, 0x95, 0x3F,
		0x0A, 0xD7, 0x08, 0xBF,
		0xB6, 0xF3, 0x59, 0xBE,
		0x48, 0xE1, 0x9A, 0x3E,

		// tob
		0x00, 0x02, 0x95, 0x3F,
		0x00, 0x00, 0x00, 0x00,
		0x3D, 0x0A, 0x07, 0x40,
		0xDF, 0x4F, 0x91, 0xBF,

		// consts
		0x00, 0x00, 0x80, 0x3F,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00
	};

	// BT.601 -> BT.709
	MemoryHelper::WriteMemoryRaw(addr_Gyuvtorgb, BT709_Matrix, sizeof(BT709_Matrix));
}

static void ApplyResolutionHook()
{
	if (!FontScaling && !FixUltraWideScreenFOV) return;

	DWORD addr_UpdateViewportRHI = ScanModuleSignature(g_State.GameModule, "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 83 EC ?? 53 56 57 A1 ?? ?? ?? ?? 33 C5 50 8D 45 F4 64 A3 00 00 00 00 8B ?? FF 15", "UpdateViewportRHI");

	if (addr_UpdateViewportRHI == 0) return;

	UpdateViewportRHI = HookHelper::CreateHook((void*)addr_UpdateViewportRHI, &UpdateViewportRHI_Hook);
}

static void ApplyGetPointerHook()
{
	if (!DisableMouseAcceleration && !FixUltraWideScreenFOV && !DisableBackgroundLevelStreaming) return;

	DWORD addr_PlayActorPtr = ScanModuleSignature(g_State.GameModule, "89 47 40 8B 45 ?? 88 5D FC 89 5D ?? 89 5D ?? 3B C3 74 0E 6A 01 50 E8 ?? ?? ?? ?? 83 C4 08 89 5D", "PlayActorPtr");
	DWORD addr_UpdatePlayActorPtr = ScanModuleSignature(g_State.GameModule, "2C 02 00 00 ?? ?? 14 06 00 00", "UpdatePlayActorPtr");
	DWORD addr_GetGEnginePtr = ScanModuleSignature(g_State.GameModule, "E8 ?? ?? ?? ?? 83 C4 40 A3 ?? ?? ?? ?? 68", "GetGEnginePtr");

	if (addr_PlayActorPtr == 0 ||
		addr_UpdatePlayActorPtr == 0 ||
		addr_GetGEnginePtr == 0) {
		return;
	}

	static SafetyHookMid capturePlayActorPtr{};
	capturePlayActorPtr = safetyhook::create_mid(addr_PlayActorPtr,
		[](safetyhook::Context& ctx)
		{
			uint32_t eax = ctx.eax;
			if (g_State.pInput != eax)
			{
				g_State.pInput = eax;
				g_State.pFOV = MemoryHelper::ReadMemory<int>(g_State.pInput + 0x22C) + 0xA44;
				g_State.pFOVCut = MemoryHelper::ReadMemory<int>(g_State.pInput + 0x3C8) + 0x384;
			}
		}
	);

	static SafetyHookMid updateActorFOVPtr{};
	updateActorFOVPtr = safetyhook::create_mid(addr_UpdatePlayActorPtr - 0x2,
		[](safetyhook::Context& ctx)
		{
			uint32_t esi = ctx.esi;
			if (g_State.pFOV != esi + 0xA44)
			{
				g_State.pFOV = esi + 0xA44;
			}
		}
	);

	static SafetyHookMid GetGEnginePtr{};
	GetGEnginePtr = safetyhook::create_mid(addr_GetGEnginePtr + 0x8,
		[](safetyhook::Context& ctx)
		{
			g_State.pGEngine = ctx.eax;
		}
	);
}

static void Init()
{
	ReadConfig();

	if (FixInputBinding)
	{
		FixIniBindings::FixAll();
	}

	if (EnableCrashHandler)
	{
		CrashHandler::Install();
	}

	// Fixes
	ApplyFixHighFPSHairPhysics();
	ApplyFixHighFPSClothPhysics();
	ApplyFixHighFPSProjectileCollisionCheck();
	ApplyFixHighFPSRagdollDeath();
	ApplyFixHashTableRaceCondition();
	ApplyFixUnattachedCollisionPanic();
	ApplyFixHairCurveOverRead();
	ApplyFixExperimental();
	ApplyFixPhysX();
	ApplyFixInputBinding();
	ApplyFixWindowHandling();
	ApplyFixCPUCores();

	// General
	ApplyIniSettingsHook();
	ApplyIntroSkip();
	ApplyCheckAlice1InstallFolder();

	// Display
	ApplyFontScaling();
	ApplyAutoResolution();

	// Input
	ApplyUseSDLControllerInput();
	ApplyUsePS3ControllerIcons();
	ApplyDisableMouseAcceleration();
		
	// Graphics
	ApplyFixUltraWideScreenFOV();
	ApplyImprovedTextureStreaming();
	ApplyReducedMipMapBias();
	ApplyDisableBackgroundLevelStreaming();
	ApplyFixBinkVideoBT709();

	// Misc
	ApplyResolutionHook();
	ApplyGetPointerHook();
}

safetyhook::InlineHook hkCreateMutexW;
static HANDLE WINAPI CreateMutexW_Hook(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCWSTR lpName)
{
	if (!g_State.isInit && lpName != nullptr)
	{
		if (_wcsicmp(lpName, L"UnrealEngine3_3") == 0)
		{
			g_State.isInit = true; // Make sure to never enter this condition again
			g_State.GameModule = GetModuleHandleA(NULL);
			(void)hkCreateMutexW.disable();
			Init(); // Once when Steam/EA DRM is decrypted
		}
	}

	return hkCreateMutexW.stdcall<HANDLE, LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR>(lpMutexAttributes, bInitialOwner, lpName);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	switch (ul_reason_for_call)
	{
		case DLL_PROCESS_ATTACH:
		{
			// Prevents DLL from receiving thread notifications
			DisableThreadLibraryCalls(hModule);

			// Verify timestamp
			uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
			IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)(base);
			IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
			DWORD timestamp = nt->FileHeader.TimeDateStamp;
			DWORD entrypoint = base + nt->OptionalHeader.AddressOfEntryPoint + (0x400000 - base);

			if (timestamp == 0x4D9DA4C5)
			{
				MessageBoxA(NULL, "The CD version is not supported.", "MadnessPatch", MB_ICONERROR);
				return FALSE;
			}
			else if (timestamp == 0x4DC8887C)
			{
				if (entrypoint != 0x1021EFF)
				{
					MFortress_Steam::ApplyPatches();
				}
			}
			else if (timestamp == 0x4DC89913)
			{
				if (entrypoint != 0x102255F)
				{
					MFortress_EA::ApplyPatches();
				}

				// RELOADED has fixed headers, skip dll loading
				if (entrypoint == 0x111BFBF)
				{
					MemoryHelper::MakeJMP(entrypoint, 0x102255F);
				}
			}
			else if (timestamp != 0x4DAC7482) // Current Steam/EA App version
			{
				MessageBoxA(NULL, "This .exe is not supported.", "MadnessPatch", MB_ICONERROR);
				return FALSE;
			}

			// Skip wrapper initialization when loaded as .asi
			if (!IsUALPresent())
			{
				SystemHelper::LoadProxyLibrary();
			}

			hkCreateMutexW = HookHelper::CreateHookAPI(L"kernel32.dll", "CreateMutexW", &CreateMutexW_Hook);
			break;
		}
		case DLL_PROCESS_DETACH:
		{
			break;
		}
	}
	return TRUE;
}

#pragma endregion
