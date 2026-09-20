#include "bundle.hpp"

#include <cstring>
#include <ctime>
#include <stdexcept>

#include <gio/gio.h>

#include "core/glib/raii.hpp"

namespace fjarr::introspect {

namespace {

void octal(char* dst, std::size_t width, std::uint64_t value) {
    // width-1 digits, NUL terminated (ustar convention)
    for (std::size_t i = width - 1; i-- > 0;) {
        dst[i] = static_cast<char>('0' + (value & 7));
        value >>= 3;
    }
    dst[width - 1] = '\0';
}

std::string convert(const std::string& in, GConverter* conv) {
    glib::GObjectPtr<GConverter> guard(conv);
    std::string out;
    std::size_t consumed = 0;
    char buf[64 * 1024];
    for (;;) {
        gsize read = 0, written = 0;
        GError* err = nullptr;
        const GConverterResult r =
            g_converter_convert(conv, in.data() + consumed, in.size() - consumed, buf, sizeof buf,
                                consumed >= in.size() ? G_CONVERTER_INPUT_AT_END : G_CONVERTER_NO_FLAGS, &read, &written, &err);
        if (r == G_CONVERTER_ERROR) {
            glib::GErrorPtr e(err);
            throw std::runtime_error(std::string("gzip: ") + (err ? err->message : "error"));
        }
        consumed += read;
        out.append(buf, written);
        if (r == G_CONVERTER_FINISHED) break;
    }
    return out;
}

} // namespace

std::string tar(const BundleFiles& files) {
    std::string out;
    const auto mtime = static_cast<std::uint64_t>(std::time(nullptr));
    for (const auto& [path, body] : files) {
        char h[512];
        std::memset(h, 0, sizeof h);
        // name(100) mode(8) uid(8) gid(8) size(12) mtime(12) chksum(8) typeflag(1) linkname(100) magic(6) version(2) uname(32) gname(32) devmajor(8) devminor(8) prefix(155)
        std::string name = path, prefix;
        if (name.size() > 100) {
            const auto slash = name.rfind('/', 155);
            if (slash != std::string::npos && name.size() - slash - 1 <= 100) {
                prefix = name.substr(0, slash);
                name = name.substr(slash + 1);
            } else throw std::runtime_error("tar: member name too long: " + path);
        }
        std::memcpy(h, name.data(), name.size());
        octal(h + 100, 8, 0644);
        octal(h + 108, 8, 0);
        octal(h + 116, 8, 0);
        octal(h + 124, 12, body.size());
        octal(h + 136, 12, mtime);
        std::memset(h + 148, ' ', 8);
        h[156] = '0';
        std::memcpy(h + 257, "ustar", 6);
        std::memcpy(h + 263, "00", 2);
        std::memcpy(h + 265, "fjarr", 5);
        std::memcpy(h + 297, "fjarr", 5);
        std::memcpy(h + 345, prefix.data(), prefix.size());
        unsigned sum = 0;
        for (unsigned char c : h) sum += c;
        octal(h + 148, 7, sum);
        h[155] = ' ';
        out.append(h, sizeof h);
        out.append(body);
        const std::size_t pad = (512 - body.size() % 512) % 512;
        out.append(pad, '\0');
    }
    out.append(1024, '\0'); // end-of-archive
    return out;
}

// Level 1: the bundle is built on the core loop; a few MB at level 6 is tens of ms of loop stall.
std::string gzip(const std::string& bytes) { return convert(bytes, G_CONVERTER(g_zlib_compressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP, 1))); }

std::string gunzip(const std::string& bytes) { return convert(bytes, G_CONVERTER(g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP))); }

std::vector<std::string> tar_names(const std::string& archive) {
    std::vector<std::string> names;
    std::size_t off = 0;
    while (off + 512 <= archive.size()) {
        const char* h = archive.data() + off;
        if (h[0] == '\0') break;
        std::string name(h, strnlen(h, 100));
        const std::string prefix(h + 345, strnlen(h + 345, 155));
        if (!prefix.empty()) name = prefix + "/" + name;
        const std::uint64_t size = std::strtoull(std::string(h + 124, 12).c_str(), nullptr, 8);
        names.push_back(name);
        off += 512 + ((size + 511) / 512) * 512;
    }
    return names;
}

} // namespace fjarr::introspect
