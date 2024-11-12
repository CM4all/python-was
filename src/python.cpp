#include "python.hxx"

#include <stdexcept>

namespace Py {

Object wrap(PyObject* obj)
{
	return Object(obj);
}

Object to_pyunicode(std::string_view str)
{
	return wrap(PyUnicode_FromStringAndSize(str.data(), str.size()));
}

std::string_view to_string_view(PyObject* obj)
{
	if (PyBytes_Check(obj)) {
		return std::string_view(PyBytes_AsString(obj), PyBytes_Size(obj));
	}
	Py_ssize_t size = 0;
	const auto data = PyUnicode_AsUTF8AndSize(obj, &size);
	return std::string_view(data, size);
}

std::string get_type(PyObject* obj)
{
	if (!obj) {
		return "nullptr";
	}

	auto type_obj = wrap(PyObject_Type(obj));
	if (!type_obj) {
		rethrow_python_exception();
	}

	auto type_str = wrap(PyObject_Str(type_obj));
	if (!type_str) {
		rethrow_python_exception();
	}

	return std::string(to_string_view(type_str));
}

void rethrow_python_exception()
{
	if (PyErr_Occurred()) {
		Object type, value, traceback;
		PyErr_Fetch(&type, &value, &traceback);
		PyErr_NormalizeException(&type, &value, &traceback);

		if (value) {
			auto str = wrap(PyObject_Str(value));
			if (str) {
				const auto message = to_string_view(str);
				if (!message.empty()) {
					throw Error(std::string(message));
				}
			}
		}
		throw Error("Cannot convert Python exception to string");
	}
	throw Error("No Python exception set");
}

void add_sys_path(std::string_view path)
{
	auto sys_module = wrap(PyImport_ImportModule("sys"));
	if (!sys_module) {
		rethrow_python_exception();
	}

	auto sys_path = wrap(PyObject_GetAttrString(sys_module, "path"));
	if (!sys_path || !PyList_Check(sys_path)) {
		rethrow_python_exception();
	}

	// Append the current directory or any specific path to sys.path
	auto py_path = wrap(to_pyunicode(path));
	if (!py_path) {
		rethrow_python_exception();
	}

	if (PyList_Append(sys_path, py_path) != 0) {
		rethrow_python_exception();
	}
}

Object import(std::string_view module_name) {
	auto py_name = wrap(PyUnicode_DecodeFSDefaultAndSize(module_name.data(), module_name.size()));
	return wrap(PyImport_Import(py_name));
}

}
