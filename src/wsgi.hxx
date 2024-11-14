#pragma once

#include "http.hxx"
#include "python.hxx"

#include <optional>
#include <string_view>

class WsgiRequestHandler final : public RequestHandler {
	Py::Object module;
	Py::Object app;

public:
	~WsgiRequestHandler() = default;

	WsgiRequestHandler(std::optional<std::string_view> module_name, std::optional<std::string_view> app_name);

	virtual HttpResponse OnRequest(HttpRequest &&req) override;
};
