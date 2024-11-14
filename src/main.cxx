#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "http.hxx"
#include "python.hxx"
#include "was.hxx"
#include "wsgi.hxx"

#include <fmt/core.h>
#include <unistd.h>

#include <util/NumberParser.hxx>

struct CommandLine {
	std::vector<std::string_view> sys_path;
	std::optional<std::string_view> module;
	std::optional<std::string_view> app;
	std::optional<std::string_view> host;
	std::optional<uint16_t> port;

	void usage() { fmt::print("py-gi-bridge [--host <ip>] [--port <port>] [--module <module>] [--app <app>]\n"); }

	std::string_view get_arg(std::span<const std::string_view> args, size_t &i)
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
		for (size_t i = 0; i < args.size(); ++i) {
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

void
print_response(const HttpResponse &resp) noexcept
{
	fmt::print(stderr, "STATUS {}\n", resp.status);
	for (const auto &[name, value] : resp.headers) {
		fmt::print(stderr, "{}: {}\n", name, value);
	}
}

void
request(RequestHandler &handler,
	http_method_t method,
	std::string_view uri,
	std::string_view content_type,
	std::string request_body)
{
	HttpRequest request{
		.script_name = "",
		.protocol = "HTTP/1.1",
		.scheme = "http",
		.method = method,
		.uri = Uri::split(uri),
		.headers = {},
		.body = {},
	};

	if (!request_body.empty()) {
		const auto content_length = request_body.size();
		request.body = std::make_unique<StringInputStream>(std::move(request_body));
		request.headers.emplace_back("Content-Type", content_type);
		request.headers.emplace_back("Content-Length", std::to_string(content_length));
	}

	struct PrintResponder : public HttpResponder {
		void SendHeaders(HttpResponse &&response) override { print_response(response); }
		void SendBody(std::string_view body_data) override { fmt::print("{}", body_data); }
	};

	PrintResponder responder;
	handler.Process(std::move(request), responder);
	fmt::print("\n");
}

int
main(int argc, char **argv)
{
	try {
		fmt::print(stderr, "cwd: {}\n", ::get_current_dir_name());
		CommandLine args(argc, argv);
		Py::Python python;
		// If you are in a virtual environment, <venv>/bin should be in PATH.
		// Python will try to find python3 in PATH and if it finds ../pyvenv.cfg next to python3, it will add
		// the corresponding site-packages of the venv to the sys.path.
		// So simply activating a venv should make this work. If it doesn't just add
		// <venv>/lib/pythonX.YY/site-packages to sys.path.
		// FIXME: stand-alone app.py does work, but in runwas it does not (probably something with env)
		Py::add_sys_path(".");
		// FIXME: runwas doesn't forward env, so add it
		Py::add_sys_path("/home/joel/dev/py_gi_bridge/.venv/lib/python3.11/site-packages");
		// PyRun_SimpleString("import sys; print('sys.path', sys.path, file=sys.stderr)");
		// PyRun_SimpleString("import os; print('os.cwd', os.getcwd(), file=sys.stderr)");
		// PyRun_SimpleString("import os; print('os.environ', os.environ, file=sys.stderr)");

		WsgiRequestHandler wsgi(args.module, args.app);

		if (::isatty(0)) {
			request(wsgi, HTTP_METHOD_GET, "/", "", "");
			request(wsgi, HTTP_METHOD_PUT, "/", "application/json", R"({"key": "value"})");
			return 0;
		}

		fmt::print(stderr, "Starting in WAS mode\n");
		Was was;
		was.Run(wsgi);

		return 0;
	} catch (const Py::Error &exc) {
		fmt::print(stderr, "Python Exception: {}\n", exc.what());
		return 1;
	} catch (const std::runtime_error &exc) {
		fmt::print(stderr, "{}\n", exc.what());
		return 1;
	}
	return 0;
}
