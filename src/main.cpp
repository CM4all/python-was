#include <memory>
#include <string_view>
#include <vector>
#include <optional>
#include <span>

#include "python.hxx"
#include "wsgi.hxx"
#include "http.hxx"

#include <fmt/core.h>

#include <util/NumberParser.hxx>

struct CommandLine {
	std::vector<std::string_view> sys_path;
	std::optional<std::string_view> module;
	std::optional<std::string_view> app;
	std::optional<std::string_view> host;
	std::optional<uint16_t> port;

	void usage()
	{
		fmt::print("py-gi-bridge [--host <ip>] [--port <port>] [--module <module>] [--app <app>]\n");
	}

	std::string_view get_arg(std::span<const std::string_view> args, size_t& i)
	{
		i++;
		if (i >= args.size()) {
			fmt::print(stderr, "Missing parameter");
			usage();
			throw std::runtime_error("Could not parse command line arguments");
		}
		return args[i];
	}

	CommandLine(int argc, char **argv)
	{
		std::vector<std::string_view> args(argv + 1, argv + argc);
		for (size_t i = 0; i < args.size(); ++i)
		{
			if (args[i] == "--module") {
				module = get_arg(args, i);
			} else if (args[i] == "--app") {
				app = get_arg(args, i);
			} else if (args[i] == "--host") {
				host = get_arg(args, i);
			} else if (args[i] == "--port") {
				const auto p = ParseInteger<uint16_t>(get_arg(args, i));
				if (!p) {
					throw std::runtime_error("Could not parse port");
				}
				port = *p;
			} else if (args[i] == "--sys-path") {
				sys_path.push_back(get_arg(args, i));
			} else {
				fmt::print(stderr, "Unrecognized option '{}'\n", args[i]);
				usage();
				throw std::runtime_error("Could not parse command line arguments");
			}
		}
	}
};

HttpRequest request(HttpMethod method, std::string_view uri, std::string_view content_type, std::string body)
{
	HttpRequest header {
		.script_name = "",
		.protocol = "HTTP/1.1",
		.scheme = "http",
		.method = method,
		.uri = Uri::split(uri),
		.headers = {},
		.body={}
	};
	if (!body.empty()) {
		const auto content_length = body.size();
		header.body = std::make_unique<StringInputStream>(std::move(body));
		header.headers.emplace_back("Content-Type", content_type);
		header.headers.emplace_back("Content-Length", std::to_string(content_length));
	}
	return header;
}

void print_response(const HttpResponse& resp)
{
	fmt::print("STATUS {}\n", resp.status);
	for (const auto& [name, value] : resp.headers) {
		fmt::print("{}: {}\n", name, value);
	}
	fmt::print("{}\n", resp.body);
}

int main(int argc, char **argv)
{
	try {
		CommandLine args(argc, argv);
		// If you are in a virtual environment, <venv>/bin should be in PATH.
		// Python will try to find python3 in PATH and if it finds ../pyvenv.cfg next to python3, it will add
		// the corresponding site-packages of the venv to the sys.path.
		// So simply activating a venv should make this work. If it doesn't just add <venv>/lib/pythonX.YY/site-packages to sys.path.
		Py::Python python;
		Py::add_sys_path(".");
		WsgiRequestHandler wsgi(args.module, args.app);
		print_response(wsgi.OnRequest(request(HttpMethod::GET, "/", "", "")));
		print_response(wsgi.OnRequest(request(HttpMethod::PUT, "/", "application/json", R"({"key": "value"})")));
	} catch(const Py::Error& exc) {
		fmt::print(stderr, "Python Exception: {}\n", exc.what());
		return 1;
	} catch(const std::runtime_error& exc) {
		fmt::print(stderr, "{}\n", exc.what());
		return 1;
	}
	return 0;
}
