#pragma once
// The diagnostics bundle (docs/24): named text members packed as a ustar archive and
// gzipped with GLib's GZlibCompressor — no new dependency.
// spec: docs/24-pipeline-introspection.md#files-and-bundles
#include <string>
#include <utility>
#include <vector>

namespace fjarr::introspect {

using BundleFiles = std::vector<std::pair<std::string, std::string>>; // path inside the archive → contents

/// A ustar archive (uncompressed) of the given members, all regular files, mode 0644.
std::string tar(const BundleFiles& files);
/// gzip any bytes (GZlibCompressor, format GZIP).
std::string gzip(const std::string& bytes);
/// gunzip (tests and `fjarr-agent --diagnostics` verification).
std::string gunzip(const std::string& bytes);
/// The member names of a ustar archive (tests).
std::vector<std::string> tar_names(const std::string& archive);

} // namespace fjarr::introspect
