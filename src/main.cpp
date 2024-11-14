#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "http.hxx"
#include "http/header.h"
#include "http/status.h"
#include "python.hxx"
#include "wsgi.hxx"

#include <fmt/core.h>
#include <unistd.h>

#include <util/NumberParser.hxx>
#include <was/simple.h>

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

struct WasSimple {
	struct was_simple *was;

	WasSimple() : was(was_simple_new()) {}
	~WasSimple() { was_simple_free(was); }
	operator struct was_simple *() const { return was; }

	void EndRequest()
	{
		if (!was_simple_end(was)) {
			fmt::print(stderr, "Error in was_simple_end\n");
		}
	}

	void AbortRequest()
	{
		if (!was_simple_abort(was)) {
			fmt::print(stderr, "Error in was_simple_abort\n");
		}
	}
};

// This is a WasInputStream as opposed to a FdInputStream, because we need to call was_simple_received, even if we were
// reading from the fd directly and not using was_simple_read.
class WasInputStream : public InputStream {
	WasSimple &was;

public:
	WasInputStream(WasSimple &was) : was(was) {}

	size_t Read(std::span<char> dest) override
	{
		// We want to do a blocking read, so we use was_simpe_read
		const auto n = was_simple_read(was, dest.data(), dest.size());
		if (n == -2) {
			throw std::runtime_error("Error in was_simple_read");
		}
		if (n == -1) {
			throw std::system_error(errno, std::system_category());
		}
		assert(n > 0);
		return static_cast<size_t>(n);
	}
};

// You must create a separate object for each request!
struct WasResponder : public HttpResponder {
	WasSimple &was;
	bool chunked_transfer = false;
	size_t content_length_left = 0;

	WasResponder(WasSimple &was) : was(was) {}

	void SendHeaders(HttpResponse &&response) override
	{
		if (!http_status_is_valid(response.status)) {
			throw std::runtime_error(fmt::format("Invalid HTTP response status: {}", response.status));
		}

		if (!was_simple_status(was, response.status)) {
			throw std::runtime_error("Error in was_simple_status");
		}

		std::optional<std::string_view> transfer_encoding_header;
		std::optional<std::string_view> content_length_header;

		for (const auto &[name, value] : response.headers) {
			if (HeaderMatch(name, "Content-Length")) {
				content_length_header = value;
				continue;
			}
			if (HeaderMatch(name, "Transfer-Encoding")) { // this is hop-by-hop
				transfer_encoding_header = value;
			}
			if (http_header_is_hop_by_hop(name.c_str())) {
				continue;
			}
			if (!was_simple_set_header_n(was, name.data(), name.size(), value.data(), value.size())) {
				throw std::runtime_error("was_simple_set_header_n failed");
			}
		}

		// TODO: Figure out how this is actually done in Flask
		const auto chunked_transfer =
		    transfer_encoding_header && transfer_encoding_header->find("chunked") != std::string_view::npos;

		if (content_length_header) {
			const auto num = ParseInteger<uint64_t>(*content_length_header);
			if (!num) {
				throw std::runtime_error(fmt::format(
				    "Could not parse Content-Length response header: '{}'", *content_length_header));
			}
			content_length_left = *num;
		}

		assert(!chunked_transfer); // TODO!

		if (content_length_left == 0) {
			was.EndRequest();
		} else {
			// This should be done early, but the state won't match if I do it any earlier than here
			if (!was_simple_set_length(was, content_length_left)) {
				was.EndRequest();
				throw std::runtime_error("was_simple_set_length failed");
			}
		}

		headers_sent = true;
	}

	// Needs to be called before SendHeaders
	void SendBody(std::string_view body_data) override
	{
		assert(headers_sent);

		if (body_data.size() > content_length_left) {
			throw std::runtime_error(
			    fmt::format("Attempting to send {} bytes, but only {} bytes left to sent",
					body_data.size(),
					content_length_left));
		}

		if (!was_simple_write(was, body_data.data(), body_data.size())) {
			throw std::runtime_error("was_simple_write failed");
		}
	}
};

void
handle_request(RequestHandler &handler, WasSimple &was, std::string_view uri) noexcept
{
	const auto method = was_simple_get_method(was);
	if (method == HTTP_METHOD_INVALID) {
		fmt::print(stderr, "Invalid method: {}\n", method);
		if (!was_simple_status(was, HTTP_STATUS_METHOD_NOT_ALLOWED)) {
			fmt::print(stderr, "Error in was_simple_status\n");
		}
		was.EndRequest();
		return;
	}

	const auto script_name = was_simple_get_script_name(was);
	const auto path = was_simple_get_path_info(was);
	const auto query = was_simple_get_query_string(was);
	const auto parsed_uri = Uri::split(uri);

	HttpRequest request{
		.script_name = script_name ? script_name : "",
		.protocol = "HTTP/1.1", // TODO
		.scheme = "http",	// TODO
		.method = method,
		.uri =
		    Uri{
			.path = path ? path : parsed_uri.path,
			.query = query ? query : parsed_uri.query,
		    },
	};

	auto it = was_simple_get_header_iterator(was);
	const was_simple_pair *elem;
	while ((elem = was_simple_iterator_next(it))) {
		request.headers.emplace_back(elem->name, elem->value);
	}
	was_simple_iterator_free(it);

	if (was_simple_has_body(was)) {
		request.body = std::make_unique<WasInputStream>(was);
	}

	WasResponder responder{ was };
	try {
		handler.Process(std::move(request), responder);
	} catch (std::exception &exc) {
		// If was_simple_status, was_simple_set_header, etc. fail, the cause was likely an IO on the
		// command channel, in which case it doesn't make sense to do anything else and we will
		// likely terminate soon.
		// Abort will not do anything if the state is ERROR, EndRequest will discard input and flush the control
		// channel, so EndRequest would make more sense, but we don't know when exactly we failed, so I abort
		// for now.
		// TODO: Ask Max how to deal with this properly.
		fmt::print(stderr, "Exception handling request: {}\n", exc.what());
		was.AbortRequest();
		return;
	}

	// Either this is a programming error, or an IO error and in both cases we can't do
	// much more than log and proceed to the next request.
	if (!was_simple_end(was)) {
		fmt::print(stderr, "Error in was_simple_end\n");
	}
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
		WasSimple was;
		const char *url = nullptr;
		while ((url = was_simple_accept(was))) {
			handle_request(wsgi, was, url);
		}

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
