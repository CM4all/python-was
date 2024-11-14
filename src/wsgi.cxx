#include "wsgi.hxx"

#include <fmt/core.h>

#include <stdexcept>

#include "cpython/genobject.h"
#include "http.hxx"
#include "object.h"
#include "python.hxx"

#include "http/status.h"
#include "util/CharUtil.hxx"
#include "util/NumberParser.hxx"

// WSGI-Spec: https://peps.python.org/pep-3333/

namespace {

struct WsgiInputStream {
	PyObject ob_base;

	InputStream *stream; // This is an owning pointer, but this type needs to be POD

	static PyObject *read(PyObject *self, PyObject *args);
	static PyObject *readline(PyObject *self, PyObject *args);
	static PyObject *readlines(PyObject *self, PyObject *args);
	static PyObject *iter(PyObject *self);
	static PyObject *next(PyObject *self);
	static void dealloc(PyObject *self);

	static PyMethodDef *GetMethodDef() noexcept;
	static PyTypeObject *GetTypeObject();

	static Py::Object CreatePyObject(InputStream *stream);
};

PyObject *
WsgiInputStream::read(PyObject *self, PyObject *args)
{
	int64_t size = -1;
	if (!PyArg_ParseTuple(args, "|L", &size)) {
		Py::rethrow_python_exception();
	}

	if (size == 0) {
		return Py::to_bytes("").Release();
	}

	auto stream = reinterpret_cast<WsgiInputStream *>(self)->stream;

	if (size > 0) {
		std::string buf(size, '\0');
		buf.resize(stream->Read(buf));
		return Py::to_bytes(buf).Release();
	}

	assert(size < 0);
	std::array<char, 4096> read_buf;
	std::string ret;
	size_t n = 0;
	while ((n = stream->Read(read_buf)) > 0) {
		ret.append(std::string_view(read_buf.data(), n));
	}
	return Py::to_bytes(ret).Release();
}

PyObject *
WsgiInputStream::readline(PyObject * /*self*/, PyObject * /*args*/)
{
	assert(false); // TODO: Implement this! Not used by flask
	return Py_None;
}

PyObject *
WsgiInputStream::readlines(PyObject * /*self*/, PyObject * /*args*/)
{
	assert(false); // TODO: Implement this! Not used by flask
	return Py_None;
}

PyObject *
WsgiInputStream::iter(PyObject *self)
{
	Py_INCREF(self);
	return self;
}

PyObject *
WsgiInputStream::next(PyObject * /*self*/)
{
	assert(false); // TODO: Implement this! Not used by flask
	return Py_None;
}

void
WsgiInputStream::dealloc(PyObject *self)
{
	auto *obj = reinterpret_cast<WsgiInputStream *>(self);
	delete obj->stream;
	PyObject_Del(self);
}

PyMethodDef *
WsgiInputStream::GetMethodDef() noexcept
{
	// https://docs.python.org/3/library/io.html
	static PyMethodDef methods[]{
		// read(size=-1): Read up to size bytes, size < 0 or None => read until EOF
		{ "read", &WsgiInputStream::read, METH_VARARGS, "Read up to size bytes" },
		// readline(size=-1): Read until next \n (including), but at most size bytes
		{ "readline", &WsgiInputStream::readline, METH_VARARGS, "Read until next newline" },
		// readlines(hint=-1): Read multiple lines as a list, at most hint bytes, hint <= 0 or None is no hint
		{ "readlines", &WsgiInputStream::readlines, METH_VARARGS, "Read multiple lines" },
		// The object must be iterable and return lines in each iteration
		//{"__iter__", reinterpret_cast<PyCFunction>(iter), METH_NOARGS, "Return line iterator"},
		//{"__next__", reinterpret_cast<PyCFunction>(next), METH_NOARGS, "Return line iterator"},
		{ nullptr, nullptr, 0, nullptr } // Sentinel
	};
	return &methods[0];
}

PyTypeObject *
WsgiInputStream::GetTypeObject()
{
	static auto type = []() {
		PyTypeObject type = {
			.tp_name = "py_gi_bridge.WsgiInputStream",
			.tp_basicsize = sizeof(WsgiInputStream),
			.tp_dealloc = &WsgiInputStream::dealloc,
			.tp_flags = Py_TPFLAGS_DEFAULT,
			.tp_doc = "File-like object to read request body",
			.tp_iter = iter,
			.tp_iternext = next,
			.tp_methods = GetMethodDef(),
			.tp_new = PyType_GenericNew,
		};

		if (PyType_Ready(&type) < 0) {
			Py::rethrow_python_exception();
		}

		return type;
	}();
	return &type;
};

Py::Object
WsgiInputStream::CreatePyObject(InputStream *stream)
{
	auto *obj = PyObject_New(WsgiInputStream, GetTypeObject());
	if (!obj) {
		Py::rethrow_python_exception();
	}
	obj->stream = stream;
	return Py::wrap(reinterpret_cast<PyObject *>(obj));
}

PyObject *
StartResponse(PyObject *self, PyObject *args)
{
	// These are owned by the Python interpreter, because they are passed to the function.
	// Therefore we don't have to decref them and therefore I don't wrap them.
	PyObject *status, *headers;
	if (!PyArg_ParseTuple(args, "OO", &status, &headers)) {
		Py::rethrow_python_exception();
	}

	auto responder = static_cast<HttpResponder *>(PyCapsule_GetPointer(self, "HttpResponder"));
	if (!responder) {
		throw std::runtime_error("Cannot call start_response after request is finished");
	}

	const auto status_str = Py::to_string_view(status);
	const auto status_code = ParseInteger<uint16_t>(status_str.substr(0, status_str.find(' ')));
	if (!status_code) {
		throw std::runtime_error(fmt::format("Could not parse status code: '{}'", status_str));
	}
	HttpResponse response;
	response.status = static_cast<http_status_t>(*status_code);

	const auto len = PyList_Size(headers);
	for (std::remove_const_t<decltype(len)> i = 0; i < len; i++) {
		// Also a borrowed reference, see above
		PyObject *item = PyList_GetItem(headers, i);
		if (item) {
			const auto name = Py::to_string_view(PyTuple_GetItem(item, 0));
			const auto value = Py::to_string_view(PyTuple_GetItem(item, 1));
			response.headers.emplace_back(name, value);
		}
	}

	responder->SendHeaders(std::move(response));

	Py_RETURN_NONE;
}

[[gnu::pure]] std::string
TranslateHeader(std::string_view header_name) noexcept
{
	std::string str(header_name.size() + 5, '\0');
	str.append("HTTP_");
	for (size_t i = 0; i < header_name.size(); ++i) {
		str[i] = ToUpperASCII(header_name[i]);
		if (str[i] == '-') {
			str[i] = '_';
		}
	}
	return str;
}

}

Py::Object
WsgiRequestHandler::FindApp(std::optional<std::string> module_name, std::optional<std::string> app_name)
{
	/* Flask application discovery behavior:
	   no arguments: The name “app” or “wsgi” is imported (as a “.py” file, or package), automatically detecting
		an app (app or application) or factory (create_app or make_app).
	   --app <name>: The given name is imported, automatically detecting an app (app or application) or factory
		(create_app or make_app).
	 */

	const std::vector<std::string> module_fallback = { "app", "wsgi" };
	const std::vector<std::string> app_fallback = { "app", "application" };

	Py::Object module;
	if (module_name) {
		module = Py::import(*module_name);
		if (!module) {
			Py::rethrow_python_exception();
		}
	}

	if (!module) {
		for (const auto &name : module_fallback) {
			module = Py::import(name);
			if (!module) {
				PyErr_Clear();
			} else {
				break;
			}
		}
	}

	if (!module) {
		throw std::runtime_error("Could not import module 'app' or 'wsgi'");
	}

	Py::Object app;
	if (app_name) {
		app = Py::wrap(PyObject_GetAttrString(module, app_name->c_str()));
		if (!app) {
			throw std::runtime_error(fmt::format("Could not find object '{}' in module", *app_name));
		}
	}

	if (!app) {
		for (const auto &name : app_fallback) {
			app = Py::wrap(PyObject_GetAttrString(module, name.c_str()));
			if (!app || !PyCallable_Check(app)) {
				PyErr_Clear();
			} else {
				break;
			}
		}
	}

	if (!app) {
		throw std::runtime_error("Could not find object 'app' or 'application' in module");
	}

	if (PyCoro_CheckExact(app)) {
		throw std::runtime_error("Application is a coroutine. ASGI is not supported yet.");
	}

	return app;
}

WsgiRequestHandler::WsgiRequestHandler(Py::Object app) : app(std::move(app)) {}

void
WsgiRequestHandler::Process(HttpRequest &&req, HttpResponder &responder)
{
	auto environ = Py::wrap(PyDict_New());
	if (!environ) {
		Py::rethrow_python_exception();
	}

	const auto content_type = req.FindHeader("Content-Type");
	// The spec is unclear, but Flask also passes Content-Length as a string
	const auto content_length = req.FindHeader("Content-Length");

	InputStream *body_stream = req.body ? req.body.release() : new NullInputStream;

	PyDict_SetItemString(
	    environ, "REQUEST_METHOD", Py::wrap(PyUnicode_FromString(http_method_to_string(req.method))));
	PyDict_SetItemString(environ, "SCRIPT_NAME", Py::to_pyunicode(req.script_name));
	PyDict_SetItemString(environ, "PATH_INFO", Py::to_pyunicode(req.uri.path));
	PyDict_SetItemString(environ, "QUERY_STRING", Py::to_pyunicode(req.uri.query));
	PyDict_SetItemString(environ, "CONTENT_TYPE", Py::to_pyunicode(content_type.value_or("")));
	PyDict_SetItemString(environ, "CONTENT_LENGTH", Py::to_pyunicode(content_length.value_or("")));
	PyDict_SetItemString(environ, "SERVER_NAME", Py::wrap(PyUnicode_FromString("localhost"))); // TODO
	PyDict_SetItemString(environ, "SERVER_PORT", Py::wrap(PyUnicode_FromString("8081")));	   // TODO
	PyDict_SetItemString(environ, "SERVER_PROTOCOL", Py::to_pyunicode(req.protocol));

	PyDict_SetItemString(environ, "wsgi.version", Py::wrap(Py_BuildValue("(ii)", 1, 0)));
	PyDict_SetItemString(environ, "wsgi.url_scheme", Py::to_pyunicode(req.scheme));
	PyDict_SetItemString(environ, "wsgi.input", WsgiInputStream::CreatePyObject(body_stream));
	PyDict_SetItemString(environ, "wsgi.errors", PySys_GetObject("stderr")); // TODO
	PyDict_SetItemString(environ, "wsgi.multithread", Py_False);		 // TODO
	PyDict_SetItemString(environ, "wsgi.multiprocess", Py_True);		 // TODO
	PyDict_SetItemString(environ, "wsgi.run_once", Py_False);

	for (const auto &[name, value] : req.headers) {
		const auto translated_name = TranslateHeader(name);
		PyDict_SetItemString(environ, translated_name.c_str(), Py::to_pyunicode(value));
	}

	if (PyErr_Occurred()) {
		Py::rethrow_python_exception();
	}

	auto responder_capsule = PyCapsule_New(&responder, "HttpResponder", nullptr);

	PyMethodDef start_response_def = {
		"start_response", (PyCFunction)StartResponse, METH_VARARGS, "WSGI start_response"
	};
	auto start_response_callable = Py::wrap(PyCFunction_New(&start_response_def, responder_capsule));
	if (!start_response_callable) {
		Py::rethrow_python_exception();
	}

	auto args = Py::wrap(
	    PyTuple_Pack(2, static_cast<PyObject *>(environ), static_cast<PyObject *>(start_response_callable)));
	auto result = Py::wrap(PyObject_CallObject(app, args));
	if (!result) {
		Py::rethrow_python_exception();
	}

	if (!responder.HeadersSent()) {
		throw std::runtime_error("start_response needs to be called before WSGI application returns");
	}

	auto result_iterator = Py::wrap(PyObject_GetIter(result));
	if (!result_iterator) {
		Py::rethrow_python_exception();
	}

	for (auto item = Py::wrap(PyIter_Next(result_iterator)); item; item = PyIter_Next(result_iterator)) {
		if (PyErr_Occurred()) {
			Py::rethrow_python_exception();
		}
		responder.SendBody(Py::to_string_view(item));
	}

	// A capsule must not store a nullptr, so SetPointer will fail with nullptr.
	// Instead we change the name, so GetPointer will fail in StartResponse.
	// We reset the capsule so in case the Python code kept a reference to start_response
	// and called it after this function has finished, we do not get a use-after-free on `response`.
	if (PyCapsule_SetName(responder_capsule, nullptr)) {
		Py::rethrow_python_exception();
	}

	if (PyErr_Occurred()) {
		Py::rethrow_python_exception();
	}
}
