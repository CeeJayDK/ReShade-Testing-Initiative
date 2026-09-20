/* D3D12 runtime entry points referenced by vkd3d-utils; not needed for a shader compiler DLL. */
#include <windows.h>
#include <stddef.h>
#define STUB(name) int name() { return 0x80004001; /* E_NOTIMPL */ }
STUB(vkd3d_create_device)
STUB(vkd3d_create_root_signature_deserializer)
STUB(vkd3d_create_versioned_root_signature_deserializer)
STUB(vkd3d_serialize_root_signature)
STUB(vkd3d_serialize_versioned_root_signature)
void vkd3d_set_log_callback(void *cb) {}
