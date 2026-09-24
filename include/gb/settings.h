#pragma once

#include "types.h"
#include <string>

namespace gb
{
struct Settings;
// Simple persistence without external JSON lib (avoids glaze dependency for now)
// Stored as tiny key=value text file at ~/.config/GB4ME/settings.cfg
std::string default_settings_path();
} // namespace gb
