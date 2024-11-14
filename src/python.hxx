#pragma once

#include <stdexcept>
#include <string_view>
#include <utility>

#define PY_SSIZE_T_CLEAN
#include <Python.h>

namespace Py {

struct Error : std::runtime_error {
	Error(std::string str) : std::runtime_error(std::move(str)) {}
};

struct Python {
	Python() { Py_Initialize(); }
	~Python() { Py_Finalize(); }
};

class Object {
public:
	Object() = default;
	explicit Object(PyObject *obj) : object(obj) {}
	~Object() { Py_XDECREF(object); }
	Object(Object &&other) : object(std::exchange(other.object, nullptr)) {}

	Object &operator=(Object &&other)
	{
		reset(std::exchange(other.object, nullptr));
		return *this;
	}

	Object &operator=(PyObject *obj)
	{
		reset(obj);
		return *this;
	}

	operator PyObject *() const { return object; }
	PyObject **operator&() { return &object; }
	explicit operator bool() const { return object != nullptr; }
	PyObject *release() { return std::exchange(object, nullptr); }

	PyObject *borrow()
	{
		Py_XINCREF(object);
		return Object(object);
	}

	void reset(PyObject *obj)
	{
		Py_XDECREF(object);
		object = obj;
	}

private:
	PyObject *object = nullptr;
};

Object
wrap(PyObject *obj) noexcept;

Object
to_pyunicode(std::string_view str) noexcept;

Object
to_bytes(std::string_view str) noexcept;

std::string_view
to_string_view(PyObject *obj) noexcept;

std::string
get_type(PyObject *obj);

void
rethrow_python_exception();

void
add_sys_path(std::string_view path);

Object
import(std::string_view module_name) noexcept;

} // namespace Py
