#pragma once

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#include <Psapi.h>

#pragma comment(lib, "Dbghelp.lib")

namespace CrashHandler
{
	typedef BOOL(WINAPI* EnumProcessModulesFn)(HANDLE, HMODULE*, DWORD, LPDWORD);
	typedef DWORD(WINAPI* GetModuleFileNameExFn)(HANDLE, HMODULE, LPSTR, DWORD);
	typedef BOOL(WINAPI* GetModuleInformationFn)(HANDLE, HMODULE, LPMODULEINFO, DWORD);

	static LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
	static PVOID g_vectoredHandler = nullptr;
	static volatile LONG g_reportWritten = 0;

	static HMODULE g_psapi = nullptr;
	static EnumProcessModulesFn g_EnumProcessModules = nullptr;
	static GetModuleFileNameExFn g_GetModuleFileNameEx = nullptr;
	static GetModuleInformationFn g_GetModuleInformation = nullptr;

	static void WriteStr(HANDLE file, const char* text)
	{
		DWORD bw;
		WriteFile(file, text, (DWORD)strlen(text), &bw, nullptr);
	}

	static void WriteFmt(HANDLE file, const char* fmt, ...)
	{
		char buf[2048];
		va_list args;
		va_start(args, fmt);
		vsprintf_s(buf, fmt, args);
		va_end(args);
		WriteStr(file, buf);
	}

	static const char* FindFileNameA(const char* path)
	{
		const char* bs = strrchr(path, '\\');
		const char* fs = strrchr(path, '/');
		const char* sep = bs > fs ? bs : fs;

		return sep ? sep + 1 : path;
	}

	static void RemoveFileSpecW(wchar_t* path)
	{
		wchar_t* bs = wcsrchr(path, L'\\');
		wchar_t* fs = wcsrchr(path, L'/');
		wchar_t* sep = bs > fs ? bs : fs;

		if (sep)
		{
			*sep = L'\0';
		}
	}

	static bool IsAddressInModule(DWORD_PTR addr)
	{
		HMODULE mod = nullptr;
		return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod) && mod != nullptr;
	}

	static const char* ExceptionCodeName(DWORD code)
	{
		switch (code)
		{
			case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
			case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
			case EXCEPTION_BREAKPOINT:               return "BREAKPOINT";
			case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
			case EXCEPTION_FLT_DENORMAL_OPERAND:     return "FLT_DENORMAL_OPERAND";
			case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
			case EXCEPTION_FLT_INEXACT_RESULT:       return "FLT_INEXACT_RESULT";
			case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID_OPERATION";
			case EXCEPTION_FLT_OVERFLOW:             return "FLT_OVERFLOW";
			case EXCEPTION_FLT_STACK_CHECK:          return "FLT_STACK_CHECK";
			case EXCEPTION_FLT_UNDERFLOW:            return "FLT_UNDERFLOW";
			case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
			case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
			case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
			case EXCEPTION_INT_OVERFLOW:             return "INT_OVERFLOW";
			case EXCEPTION_INVALID_DISPOSITION:      return "INVALID_DISPOSITION";
			case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
			case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
			case EXCEPTION_SINGLE_STEP:              return "SINGLE_STEP";
			case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
			case 0xE06D7363:                         return "CPP_EXCEPTION";
			default:                                 return "UNKNOWN";
		}
	}

	static bool IsFatalCode(DWORD code)
	{
		switch (code)
		{
			case EXCEPTION_ACCESS_VIOLATION:
			case EXCEPTION_STACK_OVERFLOW:
			case EXCEPTION_ILLEGAL_INSTRUCTION:
			case EXCEPTION_PRIV_INSTRUCTION:
			case EXCEPTION_INT_DIVIDE_BY_ZERO:
			case EXCEPTION_IN_PAGE_ERROR:
			case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
			case EXCEPTION_DATATYPE_MISALIGNMENT:
			case EXCEPTION_NONCONTINUABLE_EXCEPTION:
			case EXCEPTION_INVALID_DISPOSITION:
			return true;
		default:
			return false;
		}
	}

	static void FormatAddress(DWORD_PTR addr, char* out, size_t outSize)
	{
		HMODULE mod = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod) && mod)
		{
			char modPath[MAX_PATH];
			GetModuleFileNameA(mod, modPath, MAX_PATH);
			const char* name = FindFileNameA(modPath);
			DWORD_PTR offset = addr - (DWORD_PTR)mod;
			sprintf_s(out, outSize, "%s+0x%X (0x%08X)", name, (unsigned)offset, (unsigned)addr);
		}
		else
		{
			sprintf_s(out, outSize, "0x%08X", (unsigned)addr);
		}
	}

	static void WriteRegisters(HANDLE file, CONTEXT* ctx)
	{
		WriteStr(file, "\r\n=== Registers ===\r\n");

		WriteFmt(file,
			"  EAX=0x%08X  EBX=0x%08X  ECX=0x%08X  EDX=0x%08X\r\n"
			"  ESI=0x%08X  EDI=0x%08X  EBP=0x%08X  ESP=0x%08X\r\n"
			"  EIP=0x%08X  EFLAGS=0x%08X\r\n"
			"  CS=0x%04X  DS=0x%04X  ES=0x%04X  FS=0x%04X  GS=0x%04X  SS=0x%04X\r\n",
			ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx,
			ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp,
			ctx->Eip, ctx->EFlags,
			ctx->SegCs, ctx->SegDs, ctx->SegEs, ctx->SegFs, ctx->SegGs, ctx->SegSs);

		if (ctx->ContextFlags & CONTEXT_DEBUG_REGISTERS)
		{
			WriteStr(file, "\r\n=== Debug Registers ===\r\n");
			WriteFmt(file,
				"  DR0=0x%08X  DR1=0x%08X  DR2=0x%08X  DR3=0x%08X\r\n"
				"  DR6=0x%08X  DR7=0x%08X\r\n",
				ctx->Dr0, ctx->Dr1, ctx->Dr2, ctx->Dr3,
				ctx->Dr6, ctx->Dr7);
		}

		if (ctx->ContextFlags & CONTEXT_FLOATING_POINT)
		{
			WriteStr(file, "\r\n=== x87 FPU ===\r\n");
			WriteFmt(file,
				"  ControlWord=0x%04X  StatusWord=0x%04X  TagWord=0x%04X\r\n"
				"  ErrorOffset=0x%08X  ErrorSelector=0x%08X\r\n"
				"  DataOffset=0x%08X   DataSelector=0x%08X\r\n",
				ctx->FloatSave.ControlWord & 0xFFFF, ctx->FloatSave.StatusWord & 0xFFFF, ctx->FloatSave.TagWord & 0xFFFF,
				ctx->FloatSave.ErrorOffset, ctx->FloatSave.ErrorSelector,
				ctx->FloatSave.DataOffset, ctx->FloatSave.DataSelector);

			for (int i = 0; i < 8; i++)
			{
				const BYTE* st = &ctx->FloatSave.RegisterArea[i * 10];
				WriteFmt(file,
					"  ST%d = %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
					i, st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7], st[8], st[9]);
			}
		}

		if (ctx->ContextFlags & CONTEXT_EXTENDED_REGISTERS)
		{
			WriteStr(file, "\r\n=== SSE / XMM ===\r\n");

			DWORD mxcsr = *(DWORD*)&ctx->ExtendedRegisters[24];
			WriteFmt(file, "  MXCSR=0x%08X\r\n", mxcsr);

			for (int i = 0; i < 8; i++)
			{
				const BYTE* xmm = &ctx->ExtendedRegisters[160 + i * 16];
				DWORD d0 = *(DWORD*)&xmm[0];
				DWORD d1 = *(DWORD*)&xmm[4];
				DWORD d2 = *(DWORD*)&xmm[8];
				DWORD d3 = *(DWORD*)&xmm[12];
				float f0 = *(float*)&d0;
				float f1 = *(float*)&d1;
				float f2 = *(float*)&d2;
				float f3 = *(float*)&d3;
				WriteFmt(file,
					"  XMM%d = %08X %08X %08X %08X   [ %g %g %g %g ]\r\n",
					i, d0, d1, d2, d3, f0, f1, f2, f3);
			}
		}
	}

	static void WriteStackTrace(HANDLE file, CONTEXT* ctx)
	{
		HANDLE process = GetCurrentProcess();
		HANDLE thread = GetCurrentThread();

		SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_NO_PROMPTS | SYMOPT_FAIL_CRITICAL_ERRORS);
		SymInitialize(process, nullptr, TRUE);

		STACKFRAME64 frame{};
		frame.AddrPC.Offset = ctx->Eip;
		frame.AddrPC.Mode = AddrModeFlat;
		frame.AddrFrame.Offset = ctx->Ebp;
		frame.AddrFrame.Mode = AddrModeFlat;
		frame.AddrStack.Offset = ctx->Esp;
		frame.AddrStack.Mode = AddrModeFlat;

		int frameNum = 0;
		int totalWalked = 0;

		while (StackWalk64(IMAGE_FILE_MACHINE_I386, process, thread, &frame, ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
		{
			if (frame.AddrPC.Offset == 0)
			{
				break;
			}

			if (++totalWalked > 128)
			{
				break;
			}

			if (!IsAddressInModule((DWORD_PTR)frame.AddrPC.Offset))
			{
				continue;
			}

			char addrStr[256];
			FormatAddress((DWORD_PTR)frame.AddrPC.Offset, addrStr, sizeof(addrStr));

			char symBuf[sizeof(SYMBOL_INFO) + 256]{};
			SYMBOL_INFO* sym = (SYMBOL_INFO*)symBuf;
			sym->SizeOfStruct = sizeof(SYMBOL_INFO);
			sym->MaxNameLen = 255;
			DWORD64 displacement = 0;

			char line[1024];
			if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, sym))
			{
				sprintf_s(line, "  #%02d  %s   %s+0x%llx\r\n", frameNum, addrStr, sym->Name, displacement);
			}
			else
			{
				sprintf_s(line, "  #%02d  %s\r\n", frameNum, addrStr);
			}

			WriteStr(file, line);
			frameNum++;

			if (frameNum > 64)
			{
				break;
			}
		}

		SymCleanup(process);
	}

	static void WriteModuleList(HANDLE file)
	{
		WriteStr(file, "\r\n=== Loaded Modules ===\r\n");

		if (!g_EnumProcessModules || !g_GetModuleFileNameEx || !g_GetModuleInformation)
		{
			return;
		}

		HMODULE mods[1024];
		DWORD needed = 0;

		if (!g_EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
		{
			return;
		}

		DWORD count = needed / sizeof(HMODULE);
		for (DWORD i = 0; i < count; i++)
		{
			char path[MAX_PATH];
			if (!g_GetModuleFileNameEx(GetCurrentProcess(), mods[i], path, MAX_PATH))
			{
				continue;
			}

			MODULEINFO mi{};
			g_GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi));

			WriteFmt(file, "  0x%08X - 0x%08X  %s\r\n",
				(unsigned)(DWORD_PTR)mi.lpBaseOfDll,
				(unsigned)((DWORD_PTR)mi.lpBaseOfDll + mi.SizeOfImage),
				path);
		}
	}

	static void WriteCrashReport(EXCEPTION_POINTERS* ep, const char* sourceTag)
	{
		if (InterlockedExchange(&g_reportWritten, 1) != 0)
		{
			return;
		}

		wchar_t exePath[MAX_PATH];
		GetModuleFileNameW(nullptr, exePath, MAX_PATH);
		RemoveFileSpecW(exePath);

		wchar_t dumpDir[MAX_PATH];
		swprintf_s(dumpDir, L"%s\\CrashDump", exePath);
		CreateDirectoryW(dumpDir, nullptr);

		SYSTEMTIME st;
		GetLocalTime(&st);

		wchar_t txtPath[MAX_PATH];
		swprintf_s(txtPath, L"%s\\crash_%04d%02d%02d_%02d%02d%02d.txt", dumpDir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

		HANDLE file = CreateFileW(txtPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			return;
		}

		EXCEPTION_RECORD* er = ep->ExceptionRecord;
		CONTEXT* ctx = ep->ContextRecord;

		char addrStr[256];
		FormatAddress((DWORD_PTR)er->ExceptionAddress, addrStr, sizeof(addrStr));

		WriteFmt(file,
			"=== Crash Report ===\r\n"
			"Source:     %s\r\n"
			"Time:       %04d-%02d-%02d %02d:%02d:%02d\r\n"
			"Exception:  0x%08X (%s)\r\n"
			"Address:    %s\r\n"
			"ThreadId:   %lu\r\n",
			sourceTag,
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
			(unsigned)er->ExceptionCode,
			ExceptionCodeName(er->ExceptionCode),
			addrStr,
			GetCurrentThreadId());

		if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
		{
			const char* op = er->ExceptionInformation[0] == 0 ? "read" : er->ExceptionInformation[0] == 1 ? "write" : er->ExceptionInformation[0] == 8 ? "DEP" : "execute";
			WriteFmt(file, "AV Detail:  %s at 0x%08X\r\n", op, (unsigned)er->ExceptionInformation[1]);
		}

		if (er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR && er->NumberParameters >= 3)
		{
			WriteFmt(file, "NTSTATUS:   0x%08X\r\n", (unsigned)er->ExceptionInformation[2]);
		}

		WriteRegisters(file, ctx);

		WriteStr(file, "\r\n=== Stack Trace ===\r\n");
		CONTEXT ctxCopy = *ctx;
		WriteStackTrace(file, &ctxCopy);

		WriteModuleList(file);

		CloseHandle(file);

		wchar_t msg[1024];
		swprintf_s(msg,
			L"The game has crashed.\n\n"
			L"Exception: 0x%08X (%hs)\n"
			L"At: %hs\n\n"
			L"Crash report saved to:\n%s",
			(unsigned)er->ExceptionCode,
			ExceptionCodeName(er->ExceptionCode),
			addrStr,
			txtPath);
		MessageBoxW(nullptr, msg, L"Game Crash", MB_OK | MB_ICONERROR | MB_TOPMOST);
	}

	static LONG WINAPI VectoredCrashFilter(EXCEPTION_POINTERS* ep)
	{
		if (!IsFatalCode(ep->ExceptionRecord->ExceptionCode))
		{
			return EXCEPTION_CONTINUE_SEARCH;
		}

		WriteCrashReport(ep, "VEH");

		return EXCEPTION_CONTINUE_SEARCH;
	}

	static LONG WINAPI UnhandledCrashFilter(EXCEPTION_POINTERS* ep)
	{
		WriteCrashReport(ep, "Unhandled");

		return g_previousFilter ? g_previousFilter(ep) : EXCEPTION_EXECUTE_HANDLER;
	}

	static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilterStub(LPTOP_LEVEL_EXCEPTION_FILTER)
	{
		return nullptr;
	}

	static void LockUnhandledExceptionFilter()
	{
		HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
		if (!k32)
		{
			return;
		}

		void* target = (void*)GetProcAddress(k32, "SetUnhandledExceptionFilter");
		if (!target)
		{
			return;
		}

		unsigned char patch[5] = { 0xE9, 0, 0, 0, 0 };
		intptr_t rel = (intptr_t)&SetUnhandledExceptionFilterStub - (intptr_t)target - 5;
		memcpy(patch + 1, &rel, 4);

		DWORD oldProtect;
		if (VirtualProtect(target, sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProtect))
		{
			memcpy(target, patch, sizeof(patch));
			VirtualProtect(target, sizeof(patch), oldProtect, &oldProtect);
			FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
		}
	}

	static void Install(bool useVEH = true)
	{
		SetErrorMode(SEM_NOGPFAULTERRORBOX);

		ULONG stackReserve = 64 * 1024;
		SetThreadStackGuarantee(&stackReserve);

		g_psapi = LoadLibraryA("psapi.dll");
		if (g_psapi)
		{
			g_EnumProcessModules = (EnumProcessModulesFn)GetProcAddress(g_psapi, "EnumProcessModules");
			g_GetModuleFileNameEx = (GetModuleFileNameExFn)GetProcAddress(g_psapi, "GetModuleFileNameExA");
			g_GetModuleInformation = (GetModuleInformationFn)GetProcAddress(g_psapi, "GetModuleInformation");
		}

		g_previousFilter = SetUnhandledExceptionFilter(UnhandledCrashFilter);

		if (useVEH)
		{
			g_vectoredHandler = AddVectoredExceptionHandler(1, VectoredCrashFilter);
		}

		LockUnhandledExceptionFilter();
	}

	static void Uninstall()
	{
		if (g_vectoredHandler)
		{
			RemoveVectoredExceptionHandler(g_vectoredHandler);
			g_vectoredHandler = nullptr;
		}

		SetUnhandledExceptionFilter(g_previousFilter);
		g_previousFilter = nullptr;

		if (g_psapi)
		{
			FreeLibrary(g_psapi);
			g_psapi = nullptr;
			g_EnumProcessModules = nullptr;
			g_GetModuleFileNameEx = nullptr;
			g_GetModuleInformation = nullptr;
		}
	}
}