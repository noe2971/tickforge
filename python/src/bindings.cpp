#include <pybind11/pybind11.h>

#include "tickforge/version.hpp"

namespace py = pybind11;

PYBIND11_MODULE(_tickforge, m) {
    m.doc() = "Tickforge pybind11 bindings";
    m.attr("__version__") =
        py::str("{}.{}.{}").format(tickforge::kVersionMajor, tickforge::kVersionMinor,
                                    tickforge::kVersionPatch);
}
