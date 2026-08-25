// 转发头：MinGW-w64 的 dbghelp.h 位于 include 根目录，
// 而 x64dbg SDK 的 _plugin_types.h 在 __GNUC__ 分支下包含 "dbghelp/dbghelp.h"，
// 此文件提供该路径的转发。
#pragma once
#include <dbghelp.h>
