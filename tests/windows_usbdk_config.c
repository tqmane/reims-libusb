/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* CPU-only: include the actual backend, replacing every helper/loader boundary. */
#include <config.h>
#include <windows.h>
#include <stdio.h>
#include "libusbi.h"
#include "os/windows_usbdk.h"

static bool export_caps = true, export_set = true, caps_ok = true, set_ok = true;
static DWORD helper_error = ERROR_GEN_FAILURE;
static USB_DK_REDIRECTOR_CAPABILITIES reported_caps = { 16, 1, 1, 0 };
static unsigned int cap_calls, set_calls, pipe_calls, start_calls, stop_calls;
static ULONG64 selected_value;
static uint8_t descriptor_values[] = { 5, 9, 255 };
static USB_DK_DEVICE_ID redirected_id;

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); exit(1); \
} } while (0)

static BOOL __cdecl mock_caps(HANDLE handle, PUSB_DK_REDIRECTOR_CAPABILITIES caps)
{
	CHECK(handle == (HANDLE)(uintptr_t)0x1234);
	CHECK(caps->Size == sizeof(*caps));
	cap_calls++;
	if (!caps_ok) { SetLastError(helper_error); return FALSE; }
	*caps = reported_caps;
	return TRUE;
}

static BOOL __cdecl mock_set(HANDLE handle, ULONG64 value)
{
	CHECK(handle == (HANDLE)(uintptr_t)0x1234);
	set_calls++; selected_value = value;
	if (!set_ok) { SetLastError(helper_error); return FALSE; }
	return TRUE;
}

static BOOL __cdecl mock_descriptor(PUSB_DK_CONFIG_DESCRIPTOR_REQUEST request,
	PUSB_CONFIGURATION_DESCRIPTOR *descriptor, PULONG length)
{
	CHECK(request->Index < ARRAYSIZE(descriptor_values));
	*descriptor = calloc(1, sizeof(**descriptor)); CHECK(*descriptor);
	(*descriptor)->bLength = LIBUSB_DT_CONFIG_SIZE;
	(*descriptor)->bDescriptorType = LIBUSB_DT_CONFIG;
	(*descriptor)->wTotalLength = sizeof(**descriptor);
	(*descriptor)->bConfigurationValue = descriptor_values[request->Index];
	*length = sizeof(**descriptor);
	return TRUE;
}

static void __cdecl mock_release(PUSB_CONFIGURATION_DESCRIPTOR descriptor)
{
	free(descriptor);
}

static HANDLE __cdecl mock_start(PUSB_DK_DEVICE_ID id)
{
	start_calls++; redirected_id = *id;
	return (HANDLE)(uintptr_t)0x1234;
}

static BOOL __cdecl mock_stop(HANDLE handle)
{
	CHECK(handle == (HANDLE)(uintptr_t)0x1234); stop_calls++;
	return TRUE;
}

static HANDLE __cdecl mock_system_handle(HANDLE handle)
{
	CHECK(handle == (HANDLE)(uintptr_t)0x1234);
	return (HANDLE)(uintptr_t)0x5678;
}

static TransferResult __cdecl mock_pipe(HANDLE handle,
	PUSB_DK_TRANSFER_REQUEST request, LPOVERLAPPED overlapped)
{
	CHECK(handle == (HANDLE)(uintptr_t)0x1234);
	CHECK(request && overlapped); pipe_calls++;
	SetLastError(ERROR_GEN_FAILURE);
	return TransferFailure;
}

static INT_PTR WINAPI unused_helper(void) { CHECK(false); return 0; }

static FARPROC WINAPI mock_proc(HMODULE module, LPCSTR name)
{
	CHECK(module == (HMODULE)(uintptr_t)0xbeef);
	if (!strcmp(name, "UsbDk_GetRedirectorCapabilities")) return export_caps ? (FARPROC)mock_caps : NULL;
	if (!strcmp(name, "UsbDk_SetConfiguration")) return export_set ? (FARPROC)mock_set : NULL;
	if (!strcmp(name, "UsbDk_GetConfigurationDescriptor")) return (FARPROC)mock_descriptor;
	if (!strcmp(name, "UsbDk_ReleaseConfigurationDescriptor")) return (FARPROC)mock_release;
	if (!strcmp(name, "UsbDk_StartRedirect")) return (FARPROC)mock_start;
	if (!strcmp(name, "UsbDk_StopRedirect")) return (FARPROC)mock_stop;
	if (!strcmp(name, "UsbDk_GetRedirectorSystemHandle")) return (FARPROC)mock_system_handle;
	if (!strcmp(name, "UsbDk_ReadPipe") || !strcmp(name, "UsbDk_WritePipe")) return (FARPROC)mock_pipe;
	return (FARPROC)unused_helper; /* Other required exports must never be called. */
}

static HMODULE mock_load(struct libusb_context *ctx, const char *name)
{
	UNUSED(ctx); CHECK(!strcmp(name, "UsbDkHelper"));
	return (HMODULE)(uintptr_t)0xbeef;
}

static BOOL WINAPI mock_unload(HMODULE module)
{
	CHECK(module == (HMODULE)(uintptr_t)0xbeef); return TRUE;
}

static HANDLE WINAPI mock_iocp(HANDLE file, HANDLE port, ULONG_PTR key, DWORD threads)
{
	CHECK(file == (HANDLE)(uintptr_t)0x5678);
	UNUSED(port); UNUSED(key); UNUSED(threads);
	return (HANDLE)(uintptr_t)1;
}

#define GetProcAddress mock_proc
#define load_system_library mock_load
#define FreeLibrary mock_unload
#define CreateIoCompletionPort mock_iocp
#define usbdk_backend usbdk_backend_under_test
#include "../libusb/os/windows_usbdk.c"
#undef usbdk_backend
#undef CreateIoCompletionPort
#undef FreeLibrary
#undef load_system_library
#undef GetProcAddress

struct fixture {
	struct libusb_context *ctx;
	struct libusb_device *dev;
	struct libusb_device_handle *handle;
	struct usbdk_device_priv *priv;
};

static struct fixture create_fixture(void)
{
	struct fixture f;
	USB_DK_DEVICE_INFO info = { 0 };
	f.ctx = calloc(1, PTR_ALIGN(sizeof(*f.ctx)) + sizeof(struct windows_context_priv)); CHECK(f.ctx);
	f.dev = calloc(1, PTR_ALIGN(sizeof(*f.dev)) + sizeof(union windows_device_priv)); CHECK(f.dev);
	f.handle = calloc(1, PTR_ALIGN(sizeof(*f.handle)) + sizeof(struct windows_device_handle_priv)); CHECK(f.handle);
	f.dev->ctx = f.ctx; f.handle->dev = f.dev; f.priv = usbi_get_device_priv(f.dev);
	info.DeviceDescriptor.bNumConfigurations = ARRAYSIZE(descriptor_values);
	info.ID.DeviceID[0] = L'D'; info.ID.InstanceID[0] = L'I';
	usbdk_device_init(f.dev, &info);
	CHECK(load_usbdk_helper_dll(f.ctx) == LIBUSB_SUCCESS);
	CHECK(usbdk_device_priv_init(f.ctx, f.dev, &info) == LIBUSB_SUCCESS);
	usbi_atomic_store(&f.dev->attached, 1);
	usbi_mutex_init(&f.handle->lock);
	list_init(&((struct windows_device_handle_priv *)usbi_get_device_handle_priv(f.handle))->active_transfers);
	CHECK(usbdk_open(f.handle) == LIBUSB_SUCCESS);
	CHECK(!memcmp(&redirected_id, &info.ID, sizeof(redirected_id)));
	return f;
}

static void destroy_fixture(struct fixture *f)
{
	usbdk_close(f->handle); usbdk_destroy_device(f->dev);
	usbi_mutex_destroy(&f->handle->lock);
	free(f->handle); free(f->dev); free(f->ctx); unload_usbdk_helper_dll();
}

static void require_value(struct fixture *f, uint8_t expected, uint8_t index)
{
	uint8_t value = 0; USB_CONFIGURATION_DESCRIPTOR descriptor;
	CHECK(usbdk_get_configuration(f->handle, &value) == LIBUSB_SUCCESS && value == expected);
	CHECK(f->priv->active_configuration == index);
	CHECK(usbdk_get_active_config_descriptor(f->dev, &descriptor, sizeof(descriptor)) == sizeof(descriptor));
	CHECK(descriptor.bConfigurationValue == expected);
}

static void check_legacy(void)
{
	for (unsigned int mask = 0; mask < 3; mask++) {
		export_caps = (mask & 1) != 0; export_set = (mask & 2) != 0;
		struct fixture f = create_fixture();
		unsigned int before = set_calls;
		require_value(&f, 5, 0);
		CHECK(usbdk_set_configuration(f.handle, 0) == LIBUSB_ERROR_NOT_SUPPORTED);
		CHECK(usbdk_set_configuration(f.handle, 5) == LIBUSB_ERROR_NOT_SUPPORTED);
		CHECK(set_calls == before); require_value(&f, 5, 0);
		destroy_fixture(&f);
	}
	export_caps = export_set = true;
}

static void check_capabilities(void)
{
	struct fixture f = create_fixture();
	unsigned int before = set_calls;
	caps_ok = false; helper_error = ERROR_INVALID_FUNCTION;
	CHECK(usbdk_set_configuration(f.handle, 9) == LIBUSB_ERROR_NOT_SUPPORTED);
	require_value(&f, 5, 0); caps_ok = true;
	USB_DK_REDIRECTOR_CAPABILITIES invalid[] = { { 12, 1, 1, 0 }, { 16, 2, 1, 0 }, { 16, 1, 0, 0 }, { 16, 1, 1, 1 } };
	for (unsigned int i = 0; i < ARRAYSIZE(invalid); i++) {
		reported_caps = invalid[i];
		CHECK(usbdk_set_configuration(f.handle, 9) == LIBUSB_ERROR_NOT_SUPPORTED);
		require_value(&f, 5, 0);
	}
	CHECK(set_calls == before);
	reported_caps = (USB_DK_REDIRECTOR_CAPABILITIES){ 16, 1, 1, 0 };
	destroy_fixture(&f);
}

static void check_commit_and_guards(void)
{
	struct fixture f = create_fixture();
	struct windows_device_handle_priv *hp = usbi_get_device_handle_priv(f.handle);
	struct list_head pending;
	unsigned int before = set_calls;
	CHECK(usbdk_set_configuration(f.handle, 128) == LIBUSB_ERROR_NOT_FOUND);
	f.handle->claimed_interfaces = 1;
	CHECK(usbdk_set_configuration(f.handle, 9) == LIBUSB_ERROR_BUSY);
	f.handle->claimed_interfaces = 0;
	list_add(&pending, &hp->active_transfers);
	CHECK(usbdk_set_configuration(f.handle, 9) == LIBUSB_ERROR_BUSY);
	list_del(&pending);
	usbi_atomic_store(&f.dev->attached, 0);
	CHECK(usbdk_set_configuration(f.handle, 9) == LIBUSB_ERROR_NO_DEVICE);
	usbi_atomic_store(&f.dev->attached, 1);
	CHECK(set_calls == before); require_value(&f, 5, 0);
	CHECK(usbdk_set_configuration(f.handle, 9) == LIBUSB_SUCCESS);
	CHECK(selected_value == 9); require_value(&f, 9, 1);
	CHECK(usbdk_set_configuration(f.handle, 9) == LIBUSB_SUCCESS);
	CHECK(set_calls == before + 2); /* Same value still performs the requested reset. */
	const struct { DWORD error; int expected; } failures[] = {
		{ ERROR_INVALID_FUNCTION, LIBUSB_ERROR_NOT_SUPPORTED },
		{ ERROR_NOT_SUPPORTED, LIBUSB_ERROR_NOT_SUPPORTED },
		{ ERROR_DEVICE_NOT_CONNECTED, LIBUSB_ERROR_NO_DEVICE },
		{ ERROR_BUSY, LIBUSB_ERROR_BUSY },
		{ ERROR_INVALID_PARAMETER, LIBUSB_ERROR_INVALID_PARAM }
	};
	for (unsigned int i = 0; i < ARRAYSIZE(failures); i++) {
		set_ok = false; helper_error = failures[i].error;
		CHECK(usbdk_set_configuration(f.handle, 5) == failures[i].expected);
		CHECK(f.priv->active_configuration == 1 && !f.priv->configuration_valid);
		set_ok = true;
		CHECK(usbdk_set_configuration(f.handle, 9) == LIBUSB_SUCCESS);
		require_value(&f, 9, 1);
	}
	set_ok = false; helper_error = ERROR_GEN_FAILURE;
	CHECK(usbdk_set_configuration(f.handle, 5) == LIBUSB_ERROR_IO);
	CHECK(f.priv->active_configuration == 1 && !f.priv->configuration_valid);
	uint8_t value = 77;
	CHECK(usbdk_get_configuration(f.handle, &value) == LIBUSB_ERROR_IO && value == 77);
	CHECK(usbdk_claim_interface(f.handle, 0) == LIBUSB_ERROR_IO);
	CHECK(usbdk_set_interface_altsetting(f.handle, 0, 0) == LIBUSB_ERROR_IO);
	CHECK(usbdk_clear_halt(f.handle, 0x81) == LIBUSB_ERROR_IO);
	CHECK(usbdk_reset_device(f.handle) == LIBUSB_ERROR_IO);
	struct libusb_transfer *transfer = libusb_alloc_transfer(0); CHECK(transfer);
	transfer->dev_handle = f.handle; transfer->type = LIBUSB_TRANSFER_TYPE_BULK;
	unsigned int pipes_before = pipe_calls;
	CHECK(usbdk_submit_transfer(LIBUSB_TRANSFER_TO_USBI_TRANSFER(transfer)) == LIBUSB_ERROR_IO);
	CHECK(pipe_calls == pipes_before);
	set_ok = true;
	CHECK(usbdk_set_configuration(f.handle, 255) == LIBUSB_SUCCESS);
	require_value(&f, 255, 2);
	CHECK(usbdk_set_configuration(f.handle, 0) == LIBUSB_SUCCESS && selected_value == 0);
	CHECK(usbdk_get_configuration(f.handle, &value) == LIBUSB_SUCCESS && value == 0);
	USB_CONFIGURATION_DESCRIPTOR descriptor;
	CHECK(usbdk_get_active_config_descriptor(f.dev, &descriptor, sizeof(descriptor)) == LIBUSB_ERROR_NOT_FOUND);
	CHECK(usbdk_clear_halt(f.handle, 0x81) == LIBUSB_ERROR_NOT_FOUND);
	CHECK(usbdk_reset_device(f.handle) == LIBUSB_ERROR_NOT_FOUND);
	CHECK(usbdk_submit_transfer(LIBUSB_TRANSFER_TO_USBI_TRANSFER(transfer)) == LIBUSB_ERROR_NOT_FOUND);
	CHECK(pipe_calls == pipes_before);
	CHECK(usbdk_set_configuration(f.handle, 5) == LIBUSB_SUCCESS); require_value(&f, 5, 0);
	CHECK(usbdk_submit_transfer(LIBUSB_TRANSFER_TO_USBI_TRANSFER(transfer)) == LIBUSB_ERROR_IO);
	CHECK(pipe_calls == pipes_before + 1); /* The genuine backend reaches only our mocked pipe. */
	libusb_free_transfer(transfer);
	CHECK(usbdk_set_configuration(f.handle, 9) == LIBUSB_SUCCESS);
	usbdk_close(f.handle); CHECK(!f.priv->configuration_valid);
	CHECK(usbdk_open(f.handle) == LIBUSB_SUCCESS); require_value(&f, 5, 0);
	destroy_fixture(&f);
	descriptor_values[0] = 7; f = create_fixture(); require_value(&f, 7, 0);
	destroy_fixture(&f); descriptor_values[0] = 5;
}

int main(void)
{
	CHECK(sizeof(USB_DK_REDIRECTOR_CAPABILITIES) == 16);
	check_legacy(); check_capabilities(); check_commit_and_guards();
	CHECK(start_calls == stop_calls && cap_calls && set_calls);
	puts("UsbDk configuration: legacy/capability/value/index/commit/endpoint/reopen mocks passed; no driver calls");
	return 0;
}
