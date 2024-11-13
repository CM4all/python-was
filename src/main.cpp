#include <string_view>
#include <vector>
#include <optional>
#include <span>

#include "python.hxx"
#include "wsgi.hxx"
#include "http.hxx"

#include <fmt/core.h>
#include <unistd.h>

#include <was/simple.h>
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

HttpRequest request(http_method_t method, std::string_view uri, std::string_view content_type, std::string body)
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

struct WasSimple {
	struct was_simple* was;

	WasSimple() : was(was_simple_new()) {}
	~WasSimple() { was_simple_free(was); }
	operator struct was_simple*() const { return was; }
};

void print_response(const HttpResponse& resp)
{
	fmt::print(stderr, "STATUS {}\n", resp.status);
	for (const auto& [name, value] : resp.headers) {
		fmt::print(stderr, "{}: {}\n", name, value);
	}
	fmt::print(stderr, "{}\n", resp.body);
}

bool handle_request(RequestHandler& handler, WasSimple& was)
{
	fmt::print(stderr, "Got was request\n");

	const auto method = was_simple_get_method(was);
	if (method == HTTP_METHOD_INVALID) {
		// TODO
		fmt::print(stderr, "Invalid method\n");
		return false;
	}
	const auto script_name = was_simple_get_script_name(was);
	const auto path = was_simple_get_path_info(was);
	const auto query = was_simple_get_query_string(was);

	HttpRequest request {
		.script_name = script_name ? script_name : "",
		.protocol = "HTTP/1.1", // TODO
		.scheme = "http", // TODO
		.method = method,
		.uri = Uri {
			.path = path ? path : "/",
			.query = query ? query : ""
		},
	};

	auto it = was_simple_get_header_iterator(was);
	const was_simple_pair* elem;
	while((elem = was_simple_iterator_next(it))) {
		request.headers.emplace_back(elem->name, elem->value);
	}
	was_simple_iterator_free(it);

	if (was_simple_has_body(was)) {
		// TODO: Use outpt buffer
		std::string body;
		std::array<char, 1024> read_buf;
		ssize_t res = 0;
		while((res = was_simple_read(was, read_buf.data(), read_buf.size())) > 0) {
			body.append(read_buf.data(), read_buf.size());
		}
		if (res < 0) {
			// TODO
			fmt::print(stderr, "Error in was_simple_read: {}, errno: {}\n", res, errno);
			return false;
		}
		request.body = std::make_unique<StringInputStream>(std::move(body));
	}

	HttpResponse response;
	try {
		response = handler.OnRequest(std::move(request));
	} catch(std::exception& exc) {
		// TODO
		fmt::print(stderr, "Exception in OnRequest");
		return false;
	}

	if (!http_status_is_valid(response.status)) {
		// TODO
		fmt::print(stderr, "Invalid status: {}\n", response.status);
		return false;
	}

	if (!was_simple_status(was, response.status)) {
		// TODO
		fmt::print(stderr, "was_simple_status failed\n");
		return false;
	}

	for (const auto& [name, value] : response.headers) {
		if (HeaderMatch(name, "Content-Length")) {
			continue;
		}
		fmt::print(stderr, "{}: {}\n", name, value);
		if (!was_simple_set_header_n(was, name.data(), name.size(), value.data(), value.size())) {
			// TODO
			fmt::print(stderr, "was_simple_set_header_n failed\n");
			return false;
		}
	}

	// This should be done early, but the state won't match if I do it any earlier than here
	if (!response.body.empty()) {
		if (!was_simple_set_length(was, response.body.size())) {
			// TODO
			fmt::print(stderr, "was_simple_set_length failed\n");
			return false;
		}
	}

	if (!was_simple_write(was, response.body.data(), response.body.size())) {
		fmt::print(stderr, "was_simple_write failed\n");
		return false;
	}

	return true;
}

int main(int argc, char **argv)
{
	try {
		fmt::print(stderr, "cwd: {}\n", ::get_current_dir_name());
		CommandLine args(argc, argv);
		Py::Python python;
		// If you are in a virtual environment, <venv>/bin should be in PATH.
		// Python will try to find python3 in PATH and if it finds ../pyvenv.cfg next to python3, it will add
		// the corresponding site-packages of the venv to the sys.path.
		// So simply activating a venv should make this work. If it doesn't just add <venv>/lib/pythonX.YY/site-packages to sys.path.
		// FIXME: stand-alone app.py does work, but in runwas it does not (probably something with env)
		Py::add_sys_path(".");
		// FIXME: runwas doesn't forward env, so add it
		Py::add_sys_path("/home/joel/dev/py_gi_bridge/.venv/lib/python3.11/site-packages");
		//PyRun_SimpleString("import sys; print('sys.path', sys.path, file=sys.stderr)");
		//PyRun_SimpleString("import os; print('os.cwd', os.getcwd(), file=sys.stderr)");
		//PyRun_SimpleString("import os; print('os.environ', os.environ, file=sys.stderr)");

		WsgiRequestHandler wsgi(args.module, args.app);

		if (::isatty(0)) {
			print_response(wsgi.OnRequest(request(HTTP_METHOD_GET, "/", "", "")));
			print_response(wsgi.OnRequest(request(HTTP_METHOD_PUT, "/",
				"application/json", R"({"key": "value"})")));
			return 0;
		}

		fmt::print(stderr, "Starting in WAS mode\n");
		WasSimple was;
		const char* url = nullptr;
		while ((url = was_simple_accept(was))) {
			fmt::print(stderr, "accept '{}'\n", url);
			if (handle_request(wsgi, was)) {
				if (!was_simple_end(was)) {
					// TODO
					fmt::print("Could not end request\n");
				}
			} else {
				if (!was_simple_abort(was)) {
					// TODO
					fmt::print("Could not abort request\n");
				}
			}
		}

		return 0;
	} catch(const Py::Error& exc) {
		fmt::print(stderr, "Python Exception: {}\n", exc.what());
		return 1;
	} catch(const std::runtime_error& exc) {
		fmt::print(stderr, "{}\n", exc.what());
		return 1;
	}
	return 0;
}
