/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* CPU-only: synthetic handles and a fake reset API, no USB initialization. */
#include "../libusb/os/windows_winusb.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)

static DWORD failure;
static unsigned int calls;
static DWORD initialize_failure;
static unsigned int initialized, freed;

static BOOL WINAPI initialize(HANDLE file, HANDLE *handle)
{
	CHECK(file == (HANDLE)(uintptr_t)84);
	initialized++;
	SetLastError(initialize_failure);
	if (initialize_failure != ERROR_SUCCESS)
		return FALSE;
	*handle = (HANDLE)(uintptr_t)42;
	return TRUE;
}

static BOOL WINAPI release(HANDLE handle)
{
	CHECK(handle == (HANDLE)(uintptr_t)42);
	freed++;
	return TRUE;
}

static BOOL WINAPI reset(HANDLE handle)
{
	CHECK(handle == (HANDLE)(uintptr_t)42);
	calls++;
	SetLastError(failure);
	return failure == ERROR_SUCCESS;
}

int main(void)
{
	struct libusb_context *ctx = calloc(1, PTR_ALIGN(sizeof(*ctx)) + sizeof(struct windows_context_priv));
	struct libusb_device *dev = calloc(1, PTR_ALIGN(sizeof(*dev)) + sizeof(union windows_device_priv));
	struct libusb_device_handle *handle = calloc(1, PTR_ALIGN(sizeof(*handle)) + sizeof(struct windows_device_handle_priv));
	struct winusb_device_priv *priv;
	struct winusb_device_handle_priv *opened;
	const DWORD errors[] = { ERROR_DEVICE_NOT_CONNECTED, ERROR_FILE_NOT_FOUND, ERROR_NO_SUCH_DEVICE,
		ERROR_ACCESS_DENIED, ERROR_NOT_SUPPORTED, ERROR_INVALID_FUNCTION, ERROR_SEM_TIMEOUT, ERROR_GEN_FAILURE };
	const int expected[] = { LIBUSB_ERROR_NO_DEVICE, LIBUSB_ERROR_NO_DEVICE, LIBUSB_ERROR_NO_DEVICE,
		LIBUSB_ERROR_ACCESS, LIBUSB_ERROR_NOT_SUPPORTED, LIBUSB_ERROR_NOT_SUPPORTED, LIBUSB_ERROR_TIMEOUT, LIBUSB_ERROR_IO };
	size_t i;
	int api;
	CHECK(ctx && dev && handle);
	dev->ctx = ctx;
	handle->dev = dev;
	priv = usbi_get_device_priv(dev);
	opened = get_winusb_device_handle_priv(handle);
	usbi_mutex_init(&priv->interface_lock);
	for (api = SUB_API_LIBUSBK; api <= SUB_API_LIBUSB0; api++) {
		WinUSBX[api].hDll = (HMODULE)(uintptr_t)1;
		WinUSBX[api].Initialize = initialize;
		WinUSBX[api].Free = release;
		CHECK(winusbx_reset_device(api, handle) == LIBUSB_ERROR_NOT_SUPPORTED);
		WinUSBX[api].ResetDevice = reset;
		CHECK(winusbx_reset_device(api, handle) == LIBUSB_ERROR_NOT_FOUND && calls == 0);
		opened->interface_handle[0].api_handle = (HANDLE)(uintptr_t)42;
		failure = ERROR_SUCCESS;
		CHECK(winusbx_reset_device(api, handle) == 0 && calls == 1);
		for (i = 0; i < ARRAYSIZE(errors); i++) {
			failure = errors[i];
			CHECK(winusbx_reset_device(api, handle) == expected[i]);
		}
		CHECK(calls == 1 + ARRAYSIZE(errors));
		CHECK(initialized == 0 && freed == 0);
		opened->interface_handle[0].api_handle = NULL;
		calls = 0;
		opened->interface_handle[0].dev_handle = (HANDLE)(uintptr_t)84;
		handle->claimed_interfaces = 0x80;
		failure = ERROR_SUCCESS;
		CHECK(winusbx_reset_device(api, handle) == LIBUSB_SUCCESS);
		CHECK(calls == 1 && initialized == 1 && freed == 1);
		CHECK(opened->interface_handle[0].api_handle == NULL && handle->claimed_interfaces == 0x80);
		for (i = 0; i < ARRAYSIZE(errors); i++) {
			failure = errors[i];
			CHECK(winusbx_reset_device(api, handle) == expected[i]);
			CHECK(initialized == 2 + i && freed == initialized);
			CHECK(opened->interface_handle[0].api_handle == NULL && handle->claimed_interfaces == 0x80);
		}
		initialize_failure = ERROR_ACCESS_DENIED;
		CHECK(winusbx_reset_device(api, handle) == LIBUSB_ERROR_ACCESS);
		CHECK(calls == 1 + ARRAYSIZE(errors) && initialized == freed + 1);
		initialize_failure = ERROR_SUCCESS;
		opened->interface_handle[0].dev_handle = NULL;
		calls = initialized = freed = 0;
	}
	WinUSBX[SUB_API_WINUSB].hDll = (HMODULE)(uintptr_t)1;
	CHECK(winusbx_reset_device(SUB_API_WINUSB, handle) == 0 && calls == 0);
	usbi_mutex_destroy(&priv->interface_lock);
	free(handle);
	free(dev);
	free(ctx);
	puts("Physical Windows reset: pre-claim API handle lifetime, real success and driver failures PASS (no device calls)");
	return 0;
}
