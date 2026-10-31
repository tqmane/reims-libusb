/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Actual loader implementation with fake Windows loader/metadata boundaries. */
#include <config.h>
#include <windows.h>
#include <setupapi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); exit(1); \
} } while (0)

static const WCHAR *module_path = L"D:\\bundle\\\x65e5\x672c\\libusb-1.0.dll";
static const WCHAR system_path[] = L"E:\\Windows\\System32";
static bool module_ok = true, adjacent_ok = true, system_ok = true;
static bool filename_fail, filename_truncated, system_truncated;
static unsigned int loads, system_queries;
static WCHAR loaded_paths[2][32768];

static BOOL WINAPI mock_module(DWORD flags, LPCWSTR address, HMODULE *module)
{
	CHECK(flags == (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT));
	CHECK(address != NULL);
	*module = (HMODULE)(uintptr_t)1;
	return module_ok;
}

static DWORD WINAPI mock_filename(HMODULE module, LPWSTR path, DWORD capacity)
{
	CHECK(module == (HMODULE)(uintptr_t)1);
	if (filename_fail) return 0;
	if (filename_truncated) return capacity;
	wcscpy_s(path, capacity, module_path);
	return (DWORD)wcslen(path);
}

static UINT WINAPI mock_system(LPWSTR path, UINT capacity)
{
	system_queries++;
	if (system_truncated) return capacity;
	wcscpy_s(path, capacity, system_path);
	return (UINT)wcslen(path);
}

static HMODULE WINAPI mock_load(LPCWSTR path, HANDLE file, DWORD flags)
{
	CHECK(file == NULL && flags ==
		(LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32));
	CHECK(loads < 2);
	wcscpy_s(loaded_paths[loads++], ARRAYSIZE(loaded_paths[0]), path);
	if (!wcsncmp(path, system_path, wcslen(system_path)))
		return system_ok ? (HMODULE)(uintptr_t)3 : NULL;
	return adjacent_ok ? (HMODULE)(uintptr_t)2 : NULL;
}

#define GetModuleHandleExW mock_module
#define GetModuleFileNameW mock_filename
#define GetSystemDirectoryW mock_system
#define LoadLibraryExW mock_load
#include "../libusb/os/windows_winusb.c"
#undef LoadLibraryExW
#undef GetSystemDirectoryW
#undef GetModuleFileNameW
#undef GetModuleHandleExW

static void reset(void)
{
	loads = system_queries = 0;
}

static void expect_system(void)
{
	reset();
	CHECK(load_libusbk_library() == (HMODULE)(uintptr_t)3);
	CHECK(loads == 1 && system_queries == 1);
	CHECK(!wcscmp(loaded_paths[0], L"E:\\Windows\\System32\\libusbK.dll"));
}

int main(void)
{
	reset();
	CHECK(load_libusbk_library() == (HMODULE)(uintptr_t)2);
	CHECK(loads == 1 && system_queries == 0);
	CHECK(!wcscmp(loaded_paths[0], L"D:\\bundle\\\x65e5\x672c\\libusbK.dll"));
	reset(); adjacent_ok = false;
	CHECK(load_libusbk_library() == (HMODULE)(uintptr_t)3);
	CHECK(loads == 2 && system_queries == 1);
	CHECK(!wcscmp(loaded_paths[1], L"E:\\Windows\\System32\\libusbK.dll"));
	reset(); system_ok = false;
	CHECK(load_libusbk_library() == NULL && loads == 2);
	adjacent_ok = system_ok = true;
	module_ok = false; expect_system(); module_ok = true;
	filename_fail = true; expect_system(); filename_fail = false;
	filename_truncated = true; expect_system(); filename_truncated = false;
	module_path = L"relative\\libusb-1.0.dll"; expect_system();
	module_path = L"D:relative\\libusb-1.0.dll"; expect_system();
	module_path = L"\\relative\\libusb-1.0.dll"; expect_system();
	module_path = L"\\\\server\\bundle\\libusb-1.0.dll";
	reset();
	CHECK(load_libusbk_library() == (HMODULE)(uintptr_t)2);
	CHECK(!wcscmp(loaded_paths[0], L"\\\\server\\bundle\\libusbK.dll"));
	reset(); module_ok = false; system_truncated = true;
	CHECK(load_libusbk_library() == NULL && loads == 0);
	puts("libusbK loader: Unicode/UNC adjacency, restricted flags, System32 fallback and path failures PASS (fake loader only)");
	return 0;
}
