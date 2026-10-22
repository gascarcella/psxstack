// Valve's KeyValues files (VDF), for the Steam shortcut (steam.h; docs/LAUNCHER.md "Steam"): the binary form Steam
// keeps a user's non-Steam shortcuts in (userdata/<account>/config/shortcuts.vdf), read and written back byte for
// byte, and the text form of config/loginusers.vdf, read only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace psxstack {

struct VdfNode {
    // The binary form's type bytes. A text file has maps and strings only.
    enum class Type : uint8_t {
        Map = 0x00,
        String = 0x01,
        Int32 = 0x02,
        Float32 = 0x03,
        Pointer = 0x04,
        Color = 0x06,
        UInt64 = 0x07,
        Int64 = 0x0A,
    };
    Type type = Type::Map;
    std::string key;
    std::string str;                // String: the value's bytes (UTF-8, no NUL)
    uint64_t raw = 0;               // Int32, Float32, Pointer, Color: the 4 bytes; UInt64, Int64: the 8 (little-endian)
    std::vector<VdfNode> children;  // Map: its members, in the file's order

    static VdfNode map(const std::string &key);
    static VdfNode string(const std::string &key, const std::string &value);
    static VdfNode int32(const std::string &key, int32_t value);

    int32_t as_int32() const { return (int32_t)(uint32_t)raw; }
    // A member by key, case-insensitively as Steam compares them (the first one; nullptr when absent).
    const VdfNode *find(const std::string &key) const;
    VdfNode *find(const std::string &key);
    // A String member's value, or `fallback` when it is absent or not a string.
    std::string get_string(const std::string &key, const std::string &fallback = "") const;
};

// A binary VDF file into `root` (a Map with an empty key holding the file's top-level members). False, with the byte
// offset in `err`, for a truncated file, an unknown type byte or bytes after the end: such a file is never rewritten.
bool vdf_binary_read(const std::string &bytes, VdfNode *root, std::string *err);
// `root`'s members as a binary VDF file: what vdf_binary_read read, byte for byte, when nothing was changed.
std::string vdf_binary_write(const VdfNode &root);

// A text VDF file ("key" "value", "key" { ... }; // comments; \" \\ \n \t escapes) into `root`, as above.
bool vdf_text_read(const std::string &text, VdfNode *root, std::string *err);

} // namespace psxstack
