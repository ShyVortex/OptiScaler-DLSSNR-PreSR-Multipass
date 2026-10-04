#pragma once
#include <unknwn.h>
namespace Util
{
inline bool CheckForRealObject(const char*, IUnknown*, IUnknown**) { return false; }
} // namespace Util
