#include "wsgi.hxx"

#include <fmt/core.h>

#include <algorithm>
#include <stdexcept>

#include "http.hxx"
#include "python.hxx"

#include "http/header.h"
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
		return nullptr;
	}

	if (size == 0) {
		return Py::to_bytes("").Release();
	}

	auto stream = reinterpret_cast<WsgiInputStream *>(self)->stream;

	try {
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
	} catch (const std::exception &exc) {
		PyErr_SetString(PyExc_IOError,
				fmt::format("Error reading body from WsgiInputStream: {}", errno).c_str());
		return nullptr;
	}
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
			.tp_name = "python_was.WsgiInputStream",
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

bool
is_valid_header_name(std::string_view name) noexcept
{
	// https://datatracker.ietf.org/doc/html/rfc2616#section-2.2
	static constexpr std::array<bool, 256> is_valid = []() {
		std::array<bool, 256> v = {};

		// "1*<any CHAR except CTLs or separators>", CHAR=(octets 0 - 127)
		// Control Characters "<any US-ASCII control character (octets 0 - 31) and DEL (127)>"
		std::fill(v.begin() + 32, v.begin() + 127, true);
		assert(!v[0] && !v[31] && v[32] && v[126] && !v[127]);

		// Separators
		constexpr std::array separators = { '(', ')', '<', '>', '@', ',', ';', ':', '\\', '"',
						    '/', '[', ']', '?', '=', '{', '}', ' ', '\t' };
		for (const auto c : separators) {
			v[c] = false;
		}

		return v;
	}();

	if (name.empty()) {
		return false;
	}

	for (char c : name) {
		if (!is_valid[static_cast<uint8_t>(c)]) {
			return false;
		}
	}

	return true;
}

bool
check_header_name(std::string_view name) noexcept
{
	if (!is_valid_header_name(name)) {
		PyErr_SetString(PyExc_ValueError, fmt::format("Invalid header name '{}'", name).c_str());
		return false;
	}

	std::string lower(name.size(), '\0');
	std::transform(name.begin(), name.end(), lower.begin(), ToLowerASCII);
	// http_header_is_hop_by_hop also checks for Content-Length
	if (lower != "content-length" && http_header_is_hop_by_hop(lower.c_str())) {
		PyErr_SetString(PyExc_ValueError, fmt::format("Hop-by-hop header '{}' is not allowed", name).c_str());
		return false;
	}
	return true;
}

bool
is_valid_header_value(std::string_view value) noexcept
{
	// https://www.rfc-editor.org/rfc/rfc7230#section-3.2
	// Exclude line folding (obs-fold)
	/*
	field-content = field-vchar [ 1*( SP / HTAB ) field-vchar ]
	field-value = *( field-content )
	field-vchar = VCHAR / obs-text
	*/
	static constexpr std::array<bool, 256> is_valid = []() {
		std::array<bool, 256> v = {};

		std::fill(v.begin() + 0x21, v.begin() + 0x7E + 1, true); // VCHAR (%x21-7E)
		std::fill(v.begin() + 0x80, v.begin() + 0xFF + 1, true); // obs-text (%x80-FF)
		v[0x20] = true;						 // SP
		v[0x09] = true;						 // HTAB

		return v;
	}();

	for (char c : value) {
		if (!is_valid[static_cast<uint8_t>(c)]) {
			return false;
		}
	}

	return true;
}

bool
check_header_value(std::string_view value) noexcept
{
	if (!is_valid_header_value(value)) {
		PyErr_SetString(PyExc_ValueError, fmt::format("Invalid header value '{}'", value).c_str());
		return false;
	}
	return true;
}

PyObject *
StartResponse(PyObject *self, PyObject *args)
{
	// These are owned by the Python interpreter, because they are passed to the function.
	// Therefore we don't have to decref them and therefore I don't wrap them.
	const char *status = nullptr;
	PyObject *headers = nullptr;
	PyObject *exc_info_type = nullptr;
	PyObject *exc_info_exception = nullptr;
	PyObject *exc_info_traceback = nullptr;

	// clang-format off
	if (!PyArg_ParseTuple(args,
			      "sO!|(O!O!O!)",
			      &status,
			      &PyList_Type, &headers,
			      &PyType_Type, &exc_info_type,
			      &PyObject_Type, &exc_info_exception,
			      &PyTraceBack_Type, &exc_info_traceback)) {
		return nullptr;
	}
	// clang-format on

	if (exc_info_type || exc_info_exception || exc_info_traceback) {
		assert(exc_info_type && exc_info_exception && exc_info_traceback);
		if (!PyExceptionClass_Check(exc_info_type) || !PyObject_IsInstance(exc_info_exception, exc_info_type)) {
			PyErr_SetString(PyExc_TypeError, "Invalid exc_info argument");
			return nullptr;
		}
	}

	auto response = static_cast<HttpResponse *>(PyCapsule_GetPointer(self, "HttpResponse"));
	if (!response) {
		PyErr_SetString(PyExc_RuntimeError, "Cannot call start_response after WSGI application has returned");
		return nullptr;
	}

	const auto status_sv = std::string_view(status);
	const auto status_code = ParseInteger<uint16_t>(status_sv.substr(0, status_sv.find(' ')));
	if (!status_code) {
		PyErr_SetString(PyExc_ValueError, fmt::format("Could not parse status code '{}'", status_sv).c_str());
		return nullptr;
	}
	response->status = static_cast<http_status_t>(*status_code);
	if (!http_status_is_valid(response->status)) {
		PyErr_SetString(PyExc_ValueError, fmt::format("Invalid HTTP Status '{}'", response->status).c_str());
		return nullptr;
	}

	const auto len = PyList_Size(headers);
	for (Py_ssize_t i = 0; i < len; i++) {
		PyObject *item = PyList_GetItem(headers, i); // borrowed reference

		assert(item); // Item should be !null, if i < len.

		const auto name_obj = PyTuple_GetItem(item, 0);	 // borrowed reference
		const auto value_obj = PyTuple_GetItem(item, 1); // borrowed reference

		if (!PyUnicode_Check(name_obj) || !PyUnicode_Check(value_obj)) {
			PyErr_SetString(PyExc_TypeError, "headers must be list of tuples (str, str)");
			return nullptr;
		}

		const auto name = Py::to_string_view(name_obj);
		if (!check_header_name(name)) {
			return nullptr;
		}
		const auto value = Py::to_string_view(value_obj);
		if (!check_header_value(name)) {
			return nullptr;
		}
		response->headers.emplace_back(name, value);

		if (HeaderMatch(name, "Content-Length")) {
			const auto num = ParseInteger<uint64_t>(value);
			if (!num) {
				PyErr_SetString(
				    PyExc_ValueError,
				    fmt::format("Could not parse Content-Length header: '{}'", value).c_str());
				return nullptr;
			}
			response->content_length = *num;
		}
	}

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
	// stderr will be captured by beng-proxy and transmitted to a logging server
	PyDict_SetItemString(environ, "wsgi.errors", PySys_GetObject("stderr"));
	PyDict_SetItemString(environ, "wsgi.multithread", Py_False);
	PyDict_SetItemString(environ, "wsgi.multiprocess", Py_True);
	PyDict_SetItemString(environ, "wsgi.run_once", Py_False);

	for (const auto &[name, value] : req.headers) {
		const auto translated_name = TranslateHeader(name);
		PyDict_SetItemString(environ, translated_name.c_str(), Py::to_pyunicode(value));
	}

	if (PyErr_Occurred()) {
		Py::rethrow_python_exception();
	}

	HttpResponse response;
	auto response_capsule = PyCapsule_New(&response, "HttpResponse", nullptr);

	PyMethodDef start_response_def = {
		"start_response", (PyCFunction)StartResponse, METH_VARARGS, "WSGI start_response"
	};
	auto start_response_callable = Py::wrap(PyCFunction_New(&start_response_def, response_capsule));
	if (!start_response_callable) {
		Py::rethrow_python_exception();
	}

	auto args = Py::wrap(
	    PyTuple_Pack(2, static_cast<PyObject *>(environ), static_cast<PyObject *>(start_response_callable)));
	auto result = Py::wrap(PyObject_CallObject(app, args));
	if (!result) {
		Py::rethrow_python_exception();
	}

	if (response.status == 0) {
		throw std::runtime_error("start_response needs to be called before WSGI application returns");
	}

	responder.SendHeaders(std::move(response));

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
	if (PyCapsule_SetName(response_capsule, nullptr)) {
		Py::rethrow_python_exception();
	}

	if (PyErr_Occurred()) {
		Py::rethrow_python_exception();
	}
}
