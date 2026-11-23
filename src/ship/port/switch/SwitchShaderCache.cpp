#ifdef __SWITCH__
#include "SwitchShaderCache.h"

#include <SDL2/SDL.h>
#include <glad/glad.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>

namespace {
constexpr const char* kPath = "shader-cache.bin";
constexpr char kMagic[8] = { '2', 'S', '2', 'H', 'S', 'C', '0', '1' };
constexpr uint32_t kMaxBinary = 4 * 1024 * 1024;
constexpr GLenum kProgramBinaryLength = 0x8741;
constexpr GLenum kProgramBinaryRetrievableHint = 0x8257;

using GetProgramBinaryFn = void(APIENTRY*)(GLuint, GLsizei, GLsizei*, GLenum*, void*);
using ProgramBinaryFn = void(APIENTRY*)(GLuint, GLenum, const void*, GLsizei);
using ProgramParameteriFn = void(APIENTRY*)(GLuint, GLenum, GLint);

struct Header {
    char magic[8];
    uint64_t driver;
};

// size 0 means this ID uses a program saved under the same source hash.
struct Record {
    uint64_t id0;
    uint64_t id1;
    uint64_t sourceHash;
    uint64_t checksum;
    uint32_t format;
    uint32_t size;
};

struct Binary {
    uint32_t format;
    std::vector<uint8_t> data;
};

GetProgramBinaryFn sGetBinary = nullptr;
ProgramBinaryFn sLoadBinary = nullptr;
ProgramParameteriFn sSetParameter = nullptr;
FILE* sFile = nullptr;
std::vector<std::pair<uint64_t, uint64_t>> sKnown;
std::set<std::pair<uint64_t, uint64_t>> sSaved;
std::unordered_map<uint64_t, Binary> sBinaries;
std::unordered_map<uint64_t, GLuint> sPrograms;

uint64_t Hash(const void* data, size_t size, uint64_t hash = 0xcbf29ce484222325ULL) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; i++) {
        hash = (hash ^ bytes[i]) * 0x100000001b3ULL;
    }
    return hash;
}

uint64_t Checksum(Record record, const void* data) {
    record.checksum = 0;
    return Hash(data, record.size, Hash(&record, sizeof(record)));
}

uint64_t DriverHash() {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (GLenum name : { GL_VENDOR, GL_RENDERER, GL_VERSION, GL_SHADING_LANGUAGE_VERSION }) {
        const auto* text = reinterpret_cast<const char*>(glGetString(name));
        if (text != nullptr) {
            hash = Hash(text, strlen(text), hash);
        }
    }
    return hash;
}

void Append(uint64_t id0, uint64_t id1, uint64_t sourceHash, uint32_t format, const void* data, uint32_t size) {
    // A new binary replaces one the driver rejected, so it is always written.
    const bool isNewId = sSaved.insert({ id0, id1 }).second;
    if (sFile == nullptr || (!isNewId && size == 0)) {
        return;
    }
    Record record = { id0, id1, sourceHash, 0, format, size };
    record.checksum = Checksum(record, data);
    fwrite(&record, sizeof(record), 1, sFile);
    if (size > 0) {
        fwrite(data, size, 1, sFile);
    }
    fflush(sFile);
}

// Reads the file and returns how many bytes of it are valid.
size_t Parse(const std::vector<uint8_t>& file, uint64_t driver) {
    Header header;
    if (file.size() < sizeof(header)) {
        return 0;
    }
    memcpy(&header, file.data(), sizeof(header));
    if (memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 || header.driver != driver) {
        return 0;
    }
    size_t pos = sizeof(header);
    while (pos + sizeof(Record) <= file.size()) {
        Record record;
        memcpy(&record, file.data() + pos, sizeof(record));
        const size_t payload = pos + sizeof(record);
        if (record.size > kMaxBinary || payload + record.size > file.size() ||
            Checksum(record, file.data() + payload) != record.checksum) {
            break;
        }
        if (record.size > 0) {
            sBinaries[record.sourceHash] = { record.format, std::vector<uint8_t>(file.begin() + payload,
                                                                                 file.begin() + payload + record.size) };
        }
        if (sSaved.insert({ record.id0, record.id1 }).second) {
            sKnown.push_back({ record.id0, record.id1 });
        }
        pos = payload + record.size;
    }
    return pos;
}
} // namespace

void SwitchShaderCache_Open() {
    if (sFile != nullptr) {
        return;
    }
    sGetBinary = reinterpret_cast<GetProgramBinaryFn>(SDL_GL_GetProcAddress("glGetProgramBinary"));
    sLoadBinary = reinterpret_cast<ProgramBinaryFn>(SDL_GL_GetProcAddress("glProgramBinary"));
    sSetParameter = reinterpret_cast<ProgramParameteriFn>(SDL_GL_GetProcAddress("glProgramParameteri"));
    if (sGetBinary == nullptr || sLoadBinary == nullptr || sSetParameter == nullptr) {
        SPDLOG_WARN("Shader cache is off: this driver cannot save programs");
        return;
    }

    std::vector<uint8_t> file;
    if (FILE* in = fopen(kPath, "rb")) {
        fseek(in, 0, SEEK_END);
        const long size = ftell(in);
        fseek(in, 0, SEEK_SET);
        if (size > 0) {
            file.resize(size);
            if (fread(file.data(), 1, size, in) != static_cast<size_t>(size)) {
                file.clear();
            }
        }
        fclose(in);
    }

    const uint64_t driver = DriverHash();
    const size_t valid = Parse(file, driver);
    if (valid == 0 || valid != file.size()) {
        // New, from another driver, or cut off by a crash: keep what is valid.
        FILE* out = fopen(kPath, "wb");
        if (out == nullptr) {
            SPDLOG_WARN("Shader cache is off: cannot write {}", kPath);
            return;
        }
        if (valid == 0) {
            Header header = {};
            memcpy(header.magic, kMagic, sizeof(kMagic));
            header.driver = driver;
            fwrite(&header, sizeof(header), 1, out);
        } else {
            fwrite(file.data(), 1, valid, out);
        }
        fclose(out);
    }
    sFile = fopen(kPath, "ab");
    SPDLOG_INFO("Shader cache: {} shaders, {} programs", sKnown.size(), sBinaries.size());
}

const std::vector<std::pair<uint64_t, uint64_t>>& SwitchShaderCache_KnownShaders() {
    return sKnown;
}

uint64_t SwitchShaderCache_HashSources(const char* vs, size_t vsSize, const char* fs, size_t fsSize) {
    return Hash(fs, fsSize, Hash(vs, vsSize));
}

unsigned int SwitchShaderCache_Acquire(uint64_t id0, uint64_t id1, uint64_t sourceHash, const char** origin) {
    auto shared = sPrograms.find(sourceHash);
    if (shared != sPrograms.end()) {
        *origin = "shared";
        Append(id0, id1, sourceHash, 0, nullptr, 0);
        return shared->second;
    }
    auto saved = sBinaries.find(sourceHash);
    if (saved == sBinaries.end() || sLoadBinary == nullptr) {
        return 0;
    }
    GLuint program = glCreateProgram();
    sLoadBinary(program, saved->second.format, saved->second.data.data(),
                static_cast<GLsizei>(saved->second.data.size()));
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        // A driver update can reject an old binary. Compile it again.
        while (glGetError() != GL_NO_ERROR) {
        }
        glDeleteProgram(program);
        sBinaries.erase(saved);
        return 0;
    }
    sPrograms[sourceHash] = program;
    *origin = "saved";
    Append(id0, id1, sourceHash, 0, nullptr, 0);
    return program;
}

void SwitchShaderCache_MarkRetrievable(unsigned int program) {
    if (sSetParameter != nullptr) {
        sSetParameter(program, kProgramBinaryRetrievableHint, GL_TRUE);
    }
}

void SwitchShaderCache_Store(uint64_t id0, uint64_t id1, uint64_t sourceHash, unsigned int program) {
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        return;
    }
    sPrograms[sourceHash] = program;
    if (sGetBinary == nullptr || sFile == nullptr) {
        return;
    }
    GLint length = 0;
    glGetProgramiv(program, kProgramBinaryLength, &length);
    if (length <= 0 || static_cast<uint32_t>(length) > kMaxBinary) {
        return;
    }
    std::vector<uint8_t> data(length);
    GLsizei written = 0;
    GLenum format = 0;
    sGetBinary(program, length, &written, &format, data.data());
    if (written > 0) {
        Append(id0, id1, sourceHash, format, data.data(), static_cast<uint32_t>(written));
    }
    while (glGetError() != GL_NO_ERROR) {
    }
}

void SwitchShaderCache_ReleaseBinaries() {
    std::unordered_map<uint64_t, Binary>().swap(sBinaries);
}
#endif
