#pragma once

#include "python.hxx"
#include "http.hxx"

#include <optional>
#include <string_view>

struct WsgiRequestHandler : public RequestHandler {
	~WsgiRequestHandler() = default;

	WsgiRequestHandler(std::optional<std::string_view> module_name, std::optional<std::string_view> app_name);

	static PyObject* StartResponse(PyObject* self, PyObject* args);

	static std::string TranslateHeader(std::string_view header_name);

	virtual HttpResponse OnRequest(HttpRequest&& req) override;

	Py::Object module;
	Py::Object app;
};
