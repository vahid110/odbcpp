#pragma once
#include <windows.h>
#include "../setup_model.h"
namespace rs::odbc::setup {
bool edit_dialog(HINSTANCE module, HWND parent, Fields& fields, bool fixed_name);
}
