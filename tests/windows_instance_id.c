/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* CPU-only: replace the CM metadata call; never initialize/enumerate libusb. */
#include "../libusb/os/windows_winusb.c"

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); exit(1); \
} } while (0)

static unsigned int calls;
static CONFIGRET result;
static const WCHAR *identity = L"USB\\VID_1234&PID_5678\\SYNTHETIC";

static CONFIGRET WINAPI mock_id(DEVINST node, PWSTR buffer, ULONG count, ULONG flags)
{
	CHECK(node == 42 && flags == 0 && count == 200);
	calls++;
	if (result == CR_SUCCESS)
		wcscpy_s(buffer, count, identity);
	return result;
}

int main(void)
{
	struct libusb_context *ctx = calloc(1,
		PTR_ALIGN(sizeof(*ctx)) + sizeof(struct windows_context_priv));
	struct libusb_device *dev = calloc(1, sizeof(*dev));
	struct windows_context_priv *priv;
	char buffer[800];
	unsigned int before;

	CHECK(ctx && dev);
	priv = usbi_get_context_priv(ctx);
	priv->backend = &winusb_backend;
	dev->ctx = ctx;
	dev->session_data = 42;
	usbi_atomic_store(&dev->attached, 1);
	CM_Get_Device_IDW = mock_id;
	CHECK(reims_libusb_get_windows_instance_id(NULL, buffer, sizeof(buffer)) == LIBUSB_ERROR_INVALID_PARAM);
	CHECK(buffer[0] == 0 && calls == 0);
	CHECK(reims_libusb_get_windows_instance_id(dev, NULL, sizeof(buffer)) == LIBUSB_ERROR_INVALID_PARAM);
	CHECK(reims_libusb_get_windows_instance_id(dev, buffer, 0) == LIBUSB_ERROR_INVALID_PARAM);
	CHECK(reims_libusb_get_windows_instance_id(dev, buffer, -1) == LIBUSB_ERROR_INVALID_PARAM);
	CHECK(calls == 0);
	CHECK(reims_libusb_get_windows_instance_id(dev, buffer, sizeof(buffer)) == 0);
	CHECK(!strcmp(buffer, "USB\\VID_1234&PID_5678\\SYNTHETIC") && calls == 1);
	CHECK(reims_libusb_get_windows_instance_id(dev, buffer, 1) == LIBUSB_ERROR_OVERFLOW);
	CHECK(buffer[0] == 0);
	identity = L"USB\\VID_1234&PID_5678\\\x65e5\x672c";
	CHECK(reims_libusb_get_windows_instance_id(dev, buffer, sizeof(buffer)) == 0);
	CHECK(!strcmp(buffer, "USB\\VID_1234&PID_5678\\\xe6\x97\xa5\xe6\x9c\xac"));
	identity = L"USB\\VID_1234&PID_5678\\\xd800"; /* Unpaired surrogate. */
	CHECK(reims_libusb_get_windows_instance_id(dev, buffer, sizeof(buffer)) == LIBUSB_ERROR_IO);
	CHECK(buffer[0] == 0);
	identity = L"";
	CHECK(reims_libusb_get_windows_instance_id(dev, buffer, sizeof(buffer)) == LIBUSB_ERROR_IO);
	result = CR_NO_SUCH_DEVNODE;
	CHECK(reims_libusb_get_windows_instance_id(dev, buffer, sizeof(buffer)) == LIBUSB_ERROR_NO_DEVICE);
	result = CR_BUFFER_SMALL;
	CHECK(reims_libusb_get_windows_instance_id(dev, buffer, sizeof(buffer)) == LIBUSB_ERROR_OVERFLOW);
	result = 0x13; /* CR_FAILURE */
	CHECK(reims_libusb_get_windows_instance_id(dev, buffer, sizeof(buffer)) == LIBUSB_ERROR_IO);
	CHECK(buffer[0] == 0);
	before = calls;
	usbi_atomic_store(&dev->attached, 0);
	CHECK(reims_libusb_get_windows_instance_id(dev, buffer, sizeof(buffer)) == LIBUSB_ERROR_NO_DEVICE);
	usbi_atomic_store(&dev->attached, 1);
	dev->session_data = 0;
	CHECK(reims_libusb_get_windows_instance_id(dev, buffer, sizeof(buffer)) == LIBUSB_ERROR_NO_DEVICE);
	dev->session_data = 42;
	priv->backend = &usbdk_backend;
	CHECK(reims_libusb_get_windows_instance_id(dev, buffer, sizeof(buffer)) == LIBUSB_ERROR_NOT_SUPPORTED);
	priv->backend = &winusb_backend;
	CM_Get_Device_IDW = NULL;
	CHECK(reims_libusb_get_windows_instance_id(dev, buffer, sizeof(buffer)) == LIBUSB_ERROR_NOT_SUPPORTED);
	CHECK(calls == before);
	free(dev);
	free(ctx);
	puts("Windows instance ID: synthetic UTF-8, bounded buffers, absent node/backend and invalid arguments PASS (no device calls)");
	return 0;
}
