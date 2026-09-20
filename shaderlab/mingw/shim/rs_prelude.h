#pragma once
// Forced-include prelude for building ReShade (and ShaderLab) with clang + mingw-w64.
#ifdef __cplusplus
struct _GUID;
template <typename T> inline const _GUID &rs_builtin_uuidof() { return __uuidof(T); }
#define DECLSPEC_UUID(x) __declspec(uuid(x))
#endif
#define __REQUIRED_RPCNDR_H_VERSION__ 475
#ifdef __cplusplus
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <memory>
#include <unordered_map>
#include <type_traits>
#endif
#include <share.h>
#include <malloc.h>
#ifndef PFORCEINLINE
#define PFORCEINLINE inline
#endif
#ifdef __cplusplus
#include <windows.h>
#include <unknwn.h>
#include <rpcndr.h>
#undef MIDL_INTERFACE
#define MIDL_INTERFACE(x) struct __declspec(uuid(x)) __declspec(novtable)
#include <d3dcommon.h>
#undef __uuidof
template <typename T, typename = void> struct rs_has_mingw_uuid : std::false_type {};
template <typename T> struct rs_has_mingw_uuid<T, std::void_t<decltype(__mingw_uuidof_s<T>::__uuid_inst)>> : std::true_type {};
template <typename T> constexpr const GUID &rs_uuidof()
{
	using U = std::remove_cv_t<std::remove_pointer_t<std::remove_cv_t<std::remove_reference_t<T>>>>;
	if constexpr (rs_has_mingw_uuid<U>::value) return __mingw_uuidof_s<U>::__uuid_inst;
	else return rs_builtin_uuidof<U>();
}
#define __uuidof(x) rs_uuidof<__typeof(x)>()
// Primary template fallback for mingw's __uuidof emulation (used by templates inside mingw headers, e.g. IUnknown::QueryInterface<Q>)
template <typename T> constexpr const GUID &__mingw_uuidof() { return rs_builtin_uuidof<std::remove_cv_t<std::remove_pointer_t<T>>>(); }
#include <d3d11.h>
#ifndef D3D11_DEFAULT_BLEND_FACTOR_RED
#define D3D11_DEFAULT_BLEND_FACTOR_RED   ( 1.0f )
#define D3D11_DEFAULT_BLEND_FACTOR_GREEN ( 1.0f )
#define D3D11_DEFAULT_BLEND_FACTOR_BLUE  ( 1.0f )
#define D3D11_DEFAULT_BLEND_FACTOR_ALPHA ( 1.0f )
#endif
#include <d3d11_3.h>
DEFINE_ENUM_FLAG_OPERATORS(D3D11_FENCE_FLAG)
enum NvAPI_Status : int {};
enum D3D12_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT_TYPE : int {};
namespace std { using ::powf; using ::sqrtf; using ::floorf; using ::ceilf; using ::fabsf; using ::expf; using ::logf; }
#endif
#ifndef D3DPRESENT_DONOTFLIP
#define D3DPRESENT_DONOTFLIP 0x00000004L
#endif
