#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <utility>

#include "python.hxx"
#include "wsgi.hxx"
#include "HttpServer.hxx"

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

int main(int argc, char **argv)
{
	Py::Python python;
	try {
		CommandLine args(argc, argv);
		// If you are in a virtual environment, <venv>/bin should be in PATH.
		// Python will try to find python3 in PATH and if it finds ../pyvenv.cfg next to python3, it will add
		// the corresponding site-packages of the venv to the sys.path.
		// So simply activating a venv should make this work. If it doesn't just add <venv>/lib/pythonX.YY/site-packages to sys.path.
		Py::add_sys_path(".");
		WsgiRequestHandler wsgi(args.module, args.app);
		wsgi.OnRequest(HttpRequestHeader{.protocol="HTTP/1.1", .scheme="http", .method=HttpMethod::GET, .uri="/", .headers={}});
	} catch(const Py::Error& exc) {
		fmt::print(stderr, "Python Exception: {}\n", exc.what());
		return 1;
	} catch(const std::runtime_error& exc) {
		fmt::print(stderr, "{}\n", exc.what());
		return 1;
	}
	return 0;
}
