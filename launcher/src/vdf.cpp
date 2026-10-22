#include "vdf.h"

#include <SDL3/SDL.h>

namespace psxstack {

VdfNode VdfNode::map(const std::string &key) {
    VdfNode n;
    n.type = Type::Map;
    n.key = key;
    return n;
}

VdfNode VdfNode::string(const std::string &key, const std::string &value) {
    VdfNode n;
    n.type = Type::String;
    n.key = key;
    n.str = value;
    return n;
}

VdfNode VdfNode::int32(const std::string &key, int32_t value) {
    VdfNode n;
    n.type = Type::Int32;
    n.key = key;
    n.raw = (uint32_t)value;
    return n;
}

const VdfNode *VdfNode::find(const std::string &k) const {
    for (const VdfNode &c : children) {
        if (SDL_strcasecmp(c.key.c_str(), k.c_str()) == 0) {
            return &c;
        }
    }
    return nullptr;
}

VdfNode *VdfNode::find(const std::string &k) {
    return const_cast<VdfNode *>(static_cast<const VdfNode *>(this)->find(k));
}

std::string VdfNode::get_string(const std::string &k, const std::string &fallback) const {
    const VdfNode *n = find(k);
    return n != nullptr && n->type == Type::String ? n->str : fallback;
}

// ---- binary

namespace {

struct Reader {
    explicit Reader(const std::string &bytes) : b(bytes) {}
    const std::string &b;
    size_t at = 0;
    std::string err;

    bool fail(const std::string &what) {
        if (err.empty()) {
            err = what + " at byte " + std::to_string(at);
        }
        return false;
    }
    bool cstr(std::string *out) {
        const size_t end = b.find('\0', at);
        if (end == std::string::npos) {
            return fail("a string without its end");
        }
        *out = b.substr(at, end - at);
        at = end + 1;
        return true;
    }
    bool bytes(size_t n, uint64_t *out) {
        if (b.size() - at < n) {
            return fail("a value cut short");
        }
        *out = 0;
        for (size_t i = 0; i < n; i++) {
            *out |= (uint64_t)(unsigned char)b[at + i] << (8 * i);
        }
        at += n;
        return true;
    }
    // A map's members up to its 0x08.
    bool members(VdfNode *map, int depth) {
        if (depth > 64) {
            return fail("maps nested too deep");
        }
        for (;;) {
            if (at >= b.size()) {
                return fail("a map without its end");
            }
            const unsigned char t = (unsigned char)b[at++];
            if (t == 0x08) {
                return true;
            }
            VdfNode n;
            n.type = (VdfNode::Type)t;
            if (!cstr(&n.key)) {
                return false;
            }
            bool ok;
            switch (n.type) {
            case VdfNode::Type::Map:
                ok = members(&n, depth + 1);
                break;
            case VdfNode::Type::String:
                ok = cstr(&n.str);
                break;
            case VdfNode::Type::Int32:
            case VdfNode::Type::Float32:
            case VdfNode::Type::Pointer:
            case VdfNode::Type::Color:
                ok = bytes(4, &n.raw);
                break;
            case VdfNode::Type::UInt64:
            case VdfNode::Type::Int64:
                ok = bytes(8, &n.raw);
                break;
            default:
                at--;
                return fail("an unknown type byte " + std::to_string(t));
            }
            if (!ok) {
                return false;
            }
            map->children.push_back(std::move(n));
        }
    }
};

void write_members(const VdfNode &map, std::string *out) {
    for (const VdfNode &n : map.children) {
        out->push_back((char)n.type);
        out->append(n.key);
        out->push_back('\0');
        switch (n.type) {
        case VdfNode::Type::Map:
            write_members(n, out);
            break;
        case VdfNode::Type::String:
            out->append(n.str);
            out->push_back('\0');
            break;
        default: {
            const int size = n.type == VdfNode::Type::UInt64 || n.type == VdfNode::Type::Int64 ? 8 : 4;
            for (int i = 0; i < size; i++) {
                out->push_back((char)((n.raw >> (8 * i)) & 0xFF));
            }
            break;
        }
        }
    }
    out->push_back('\x08');
}

} // namespace

bool vdf_binary_read(const std::string &bytes, VdfNode *root, std::string *err) {
    *root = VdfNode::map("");
    Reader r(bytes);
    if (!r.members(root, 0)) {
        *err = r.err;
        return false;
    }
    if (r.at != bytes.size()) {
        r.fail("bytes after the end");
        *err = r.err;
        return false;
    }
    return true;
}

std::string vdf_binary_write(const VdfNode &root) {
    std::string out;
    write_members(root, &out);
    return out;
}

// ---- text

namespace {

struct Lexer {
    explicit Lexer(const std::string &text) : t(text) {}
    const std::string &t;
    size_t at = 0;
    int line = 1;
    std::string err;

    bool fail(const std::string &what) {
        if (err.empty()) {
            err = what + " on line " + std::to_string(line);
        }
        return false;
    }
    // The next token: '{', '}', a string (quoted or bare) in `*s`, or 0 at the end.
    bool next(char *kind, std::string *s) {
        for (;;) {
            while (at < t.size() && (t[at] == ' ' || t[at] == '\t' || t[at] == '\r' || t[at] == '\n')) {
                line += t[at] == '\n';
                at++;
            }
            if (t.compare(at, 2, "//") == 0) {
                while (at < t.size() && t[at] != '\n') {
                    at++;
                }
                continue;
            }
            break;
        }
        if (at >= t.size()) {
            *kind = 0;
            return true;
        }
        const char c = t[at];
        if (c == '{' || c == '}') {
            *kind = c;
            at++;
            return true;
        }
        *kind = '"';
        s->clear();
        if (c == '"') {
            for (at++;; at++) {
                if (at >= t.size()) {
                    return fail("a string without its closing quote");
                }
                char d = t[at];
                if (d == '"') {
                    at++;
                    return true;
                }
                if (d == '\\' && at + 1 < t.size()) {
                    d = t[++at];
                    d = d == 'n' ? '\n' : d == 't' ? '\t' : d;
                }
                line += d == '\n';
                s->push_back(d);
            }
        }
        while (at < t.size() && t[at] != ' ' && t[at] != '\t' && t[at] != '\r' && t[at] != '\n' && t[at] != '{' &&
               t[at] != '}' && t[at] != '"') {
            s->push_back(t[at++]);
        }
        return true;
    }
    // Members up to the closing brace (`top`: up to the end instead).
    bool members(VdfNode *map, bool top, int depth) {
        if (depth > 64) {
            return fail("maps nested too deep");
        }
        for (;;) {
            char kind;
            std::string key, value;
            if (!next(&kind, &key)) {
                return false;
            }
            if (kind == 0) {
                return top || fail("a map without its closing brace");
            }
            if (kind == '}') {
                return !top || fail("a closing brace too many");
            }
            if (kind == '{') {
                return fail("a map without a key");
            }
            if (!next(&kind, &value)) {
                return false;
            }
            if (kind == '{') {
                VdfNode m = VdfNode::map(key);
                if (!members(&m, false, depth + 1)) {
                    return false;
                }
                map->children.push_back(std::move(m));
            } else if (kind == '"') {
                map->children.push_back(VdfNode::string(key, value));
            } else {
                return fail("a key without its value");
            }
        }
    }
};

} // namespace

bool vdf_text_read(const std::string &text, VdfNode *root, std::string *err) {
    *root = VdfNode::map("");
    Lexer lx(text);
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        lx.at = 3; // a UTF-8 byte order mark
    }
    if (!lx.members(root, true, 0)) {
        *err = lx.err;
        return false;
    }
    return true;
}

} // namespace psxstack
