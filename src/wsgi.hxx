#pragma once

#include "python.hxx"

#include <fmt/core.h>

#include <optional>
#include <string_view>
#include <stdexcept>

#include "HttpServer.hxx"
#include "util/CharUtil.hxx"
#include "util/NumberParser.hxx"

// WSGI-Spec: https://peps.python.org/pep-3333/

struct WsgiRequestHandler : public RequestHandler {
	~WsgiRequestHandler() = default;

	WsgiRequestHandler(std::optional<std::string_view> module_name, std::optional<std::string_view> app_name)
	{
		/* Flask applicatio discovery behavior
		   no arguments: The name “app” or “wsgi” is imported (as a “.py” file, or package), automatically detecting
			an app (app or application) or factory (create_app or make_app).
		   --app <name>: The given name is imported, automatically detecting an app (app or application) or factory
			(create_app or make_app).
		 */

		if (module_name) {
			module = Py::import(*module_name);
			if (!module) {
				Py::rethrow_python_exception();
			}
		} else {
			module = Py::import("app");
			if (!module) {
				module = Py::import("wsgi");
			}
			if (!module) {
				throw std::runtime_error("Could not import module 'app' or 'wsgi'");
			}
		}
		assert(module);

		if (app_name) {
			const auto app_name_s = std::string(*app_name);
			app = Py::wrap(PyObject_GetAttrString(module, app_name_s.c_str()));
			if (!app) {
				throw std::runtime_error(fmt::format("Could not find object '{}' in module", *app_name));
			}
		} else {
			app = Py::wrap(PyObject_GetAttrString(module, "app"));
			if (!app) {
				app = Py::wrap(PyObject_GetAttrString(module, "application"));
			}
			if (!app) {
				throw std::runtime_error("Could not find object 'app' or 'application' in module");
			}
		}

		if (!PyCallable_Check(app)) {
			throw std::runtime_error("Application is not a callable");
		}
	}

	static PyObject* StartResponse(PyObject* /*self*/, PyObject* args) {
		// These are owned by the Python interpreter, because they are passed to the function.
		// Therefore we don't have to decref them and therefore I don't wrap them.
		PyObject *status, *headers;
		if (!PyArg_ParseTuple(args, "OO", &status, &headers)) {
			return nullptr;
		}

		fmt::print("{}\n", Py::to_string_view(status));
		const auto len = PyList_Size(headers);
		for (std::remove_const_t<decltype(len)> i = 0; i < len; i++) {
			// Also a borrowed reference, see above
			PyObject* item = PyList_GetItem(headers, i);
			if (item) {
				fmt::print("{}: {}\n",
					Py::to_string_view(PyTuple_GetItem(item, 0)),
					Py::to_string_view(PyTuple_GetItem(item, 1)));
			}
		}

		Py_RETURN_NONE;
	}

	static std::string TranslateHeader(std::string_view header_name)
	{
		std::string str(header_name.size() + 5, '\0');
		str.append("HTTP_");
		for (size_t i = 0; i < header_name.size(); ++i)
		{
			str[i] = ToUpperASCII(header_name[i]);
			if (str[i] == '-') {
				str[i] = '_';
			}
		}
		return str;
	}

	virtual void OnRequest(const HttpRequestHeader& req) override
	{
		auto environ = Py::wrap(PyDict_New());
		if (!environ) {
			Py::rethrow_python_exception();
		}

		const auto uri = Uri::split(req.uri);
		const auto content_type = req.FindHeader("Content-Type");
		// The spec is unclear, but Flask also passes Content-Length as a string
		const auto content_length = req.FindHeader("Content-Length");

		PyDict_SetItemString(environ, "REQUEST_METHOD", PyUnicode_FromString(http_method_to_string(req.method)));
		// TODO: SCRIPT_NAME, may be empty
		PyDict_SetItemString(environ, "PATH_INFO", Py::to_pyunicode(uri.path).release());
		PyDict_SetItemString(environ, "QUERY_STRING", Py::to_pyunicode(uri.query).release());
		PyDict_SetItemString(environ, "CONTENT_TYPE", Py::to_pyunicode(content_type.value_or("")).release());
		PyDict_SetItemString(environ, "CONTENT_LENGTH", Py::to_pyunicode(content_length.value_or("")).release());
		PyDict_SetItemString(environ, "SERVER_NAME", PyUnicode_FromString("localhost")); // TODO
		PyDict_SetItemString(environ, "SERVER_PORT", PyUnicode_FromString("8081")); // TODO
		PyDict_SetItemString(environ, "SERVER_PROTOCOL",  Py::to_pyunicode(req.protocol).release());

		PyDict_SetItemString(environ, "wsgi.version", Py_BuildValue("(ii)", 1, 0));
		PyDict_SetItemString(environ, "wsgi.url_scheme", Py::to_pyunicode(req.scheme).release());
		PyDict_SetItemString(environ, "wsgi.input", PyBytes_FromString("")); // TODO
		PyDict_SetItemString(environ, "wsgi.errors", PySys_GetObject("stderr"));  // TODO
		PyDict_SetItemString(environ, "wsgi.multithread", Py_False);  // TODO
		PyDict_SetItemString(environ, "wsgi.multiprocess", Py_True);  // TODO
		PyDict_SetItemString(environ, "wsgi.run_once", Py_False);

		for (const auto& [name, value] : req.headers) {
			const auto translated_name = TranslateHeader(name);
			PyDict_SetItemString(environ, translated_name.c_str(), Py::to_pyunicode(value).release());
		}

		// Dummy start_response callable
		PyMethodDef start_response_def = {
			"start_response", (PyCFunction)StartResponse, METH_VARARGS, "WSGI start_response"};
		auto start_response_callable = Py::wrap(PyCFunction_New(&start_response_def, nullptr));

		if (!start_response_callable) {
			Py::rethrow_python_exception();
		}

		auto args = Py::wrap(PyTuple_Pack(2,
			static_cast<PyObject*>(environ),
			static_cast<PyObject*>(start_response_callable)));
		auto result = Py::wrap(PyObject_CallObject(app, args));
		if (!result) {
			Py::rethrow_python_exception();
		}

		auto result_iterator = Py::wrap(PyObject_GetIter(result));
		if (!result_iterator) {
			Py::rethrow_python_exception();
		}

		for (auto item = PyIter_Next(result_iterator); item; item = PyIter_Next(result_iterator)) {
			if (PyErr_Occurred()) {
				Py::rethrow_python_exception();
			}
			fmt::print("{}", Py::to_string_view(item));
		}

		if (PyErr_Occurred()) {
			Py::rethrow_python_exception();
		}
	}

	Py::Object module;
	Py::Object app;
};
