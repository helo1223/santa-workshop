#pragma once
#include <string>

// Empty result means cancellation or failure; error distinguishes failure.
std::string PickMapFile(void* owner, const std::string& currentPath, std::string& error);
