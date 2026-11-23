#pragma once

// Saves linked GL programs to the SD card so a shader is only compiled once.
// Shaders are keyed by their final source: different combiner IDs that produce
// the same source share one program.
#ifdef __SWITCH__
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

void SwitchShaderCache_Open();
// Every shader ID saved by earlier runs.
const std::vector<std::pair<uint64_t, uint64_t>>& SwitchShaderCache_KnownShaders();
uint64_t SwitchShaderCache_HashSources(const char* vs, size_t vsSize, const char* fs, size_t fsSize);
// Returns a ready program, or 0 if this source must be compiled.
unsigned int SwitchShaderCache_Acquire(uint64_t id0, uint64_t id1, uint64_t sourceHash, const char** origin);
// Call before linking, so the driver keeps the binary.
void SwitchShaderCache_MarkRetrievable(unsigned int program);
void SwitchShaderCache_Store(uint64_t id0, uint64_t id1, uint64_t sourceHash, unsigned int program);
// Frees the saved binaries once boot has loaded them.
void SwitchShaderCache_ReleaseBinaries();
#endif
