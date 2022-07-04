
#pragma once
#include <utility> // `std::pair`

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>

#include "ukv.hpp"

namespace unum::ukv {

namespace py = pybind11;

struct py_db_t;
struct py_txn_t;
struct py_collection_t;

struct py_buffer_t {
    ~py_buffer_t() {
        if (initialized)
            PyBuffer_Release(&py);
        initialized = false;
    }
    py_buffer_t() = default;
    py_buffer_t(py_buffer_t&&) = delete;
    py_buffer_t(py_buffer_t const&) = delete;

    Py_buffer py;
    bool initialized = false;
};

template <typename scalar_at>
std::pair<py_buffer_t, strided_range_gt<scalar_at>> strided_array(py::handle handle) {
    PyObject* obj = handle.ptr();
    if (!PyObject_CheckBuffer(obj))
        throw std::invalid_argument("Buffer protocol unsupported");

    auto flags = PyBUF_ANY_CONTIGUOUS | PyBUF_STRIDED;
    if constexpr (std::is_const_v<scalar_at>)
        flags |= PyBUF_WRITABLE;

    py_buffer_t raii;
    raii.initialized = PyObject_GetBuffer(obj, &raii.py, flags) == 0;
    if (!raii.initialized)
        throw std::invalid_argument("Couldn't obtain buffer overviews");
    if (raii.py.ndim != 1)
        throw std::invalid_argument("Expecting tensor rank 1");
    if (!raii.py.shape)
        throw std::invalid_argument("Shape wasn't inferred");
    if (raii.py.itemsize != sizeof(scalar_at))
        throw std::invalid_argument("Scalar type mismatch");

    strided_range_gt<scalar_at> result;
    result.raw = reinterpret_cast<scalar_at*>(raii.py.buf);
    result.stride = static_cast<ukv_size_t>(raii.py.strides[0]);
    result.count = static_cast<ukv_size_t>(raii.py.shape[0]);
    return {std::move(raii), result};
}

void wrap_database(py::module&);
void wrap_dataframe(py::module&);
void wrap_network(py::module&);

} // namespace unum::ukv