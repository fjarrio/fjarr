#include "raii.hpp"

namespace fjarr::glib {

ObjectCensus& ObjectCensus::instance() {
    static ObjectCensus census;
    return census;
}

} // namespace fjarr::glib
