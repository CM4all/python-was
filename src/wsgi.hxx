#pragma once

#include "http.hxx"
#include "python.hxx"

#include <optional>
#include <string>

class WsgiRequestHandler final : public RequestHandler {
	Py::Object app;

public:
	static Py::Object FindApp(std::optional<std::string> module_name, std::optional<std::string> app_name);

	WsgiRequestHandler(Py::Object app);

	virtual void Process(HttpRequest &&req, HttpResponder &responder) override;
};
