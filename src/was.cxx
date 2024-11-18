#include "was.hxx"

#include <http/header.h>
#include <util/NumberParser.hxx>
#include <was/simple.h>

#include <fmt/core.h>

namespace {
// This is a WasInputStream as opposed to a FdInputStream, because we need to call was_simple_received, even if we were
// reading from the fd directly and not using was_simple_read.
class WasInputStream : public InputStream {
	struct was_simple *was;

public:
	WasInputStream(struct was_simple *was, uint64_t content_length) : InputStream(content_length), was(was) {}

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
		assert(n >= 0);
		return static_cast<size_t>(n);
	}
};

// You must create a separate object for each request!
struct WasResponder : public HttpResponder {
	struct was_simple *was;
	std::optional<uint64_t> content_length_left = 0;

	WasResponder(struct was_simple *was) : was(was) {}

	void SendHeaders(HttpResponse &&response) override
	{
		assert(http_status_is_valid(response.status));

		if (!was_simple_status(was, response.status)) {
			throw std::runtime_error("Error in was_simple_status");
		}

		for (const auto &[name, value] : response.headers) {
			if (!was_simple_set_header_n(was, name.data(), name.size(), value.data(), value.size())) {
				throw std::runtime_error("was_simple_set_header_n failed");
			}
		}

		content_length_left = response.content_length;

		if (response.content_length == 0) {
			if (!was_simple_end(was)) {
				throw std::runtime_error("was_simple_end failed");
			}
		} else if (response.content_length) {
			// This should be done early, but the state won't match if I do it any earlier than here
			if (!was_simple_set_length(was, *response.content_length)) {
				throw std::runtime_error("was_simple_set_length failed");
			}
		}

		headers_sent = true;
	}

	// Needs to be called before SendHeaders
	void SendBody(std::string_view body_data) override
	{
		assert(headers_sent);

		const auto write_len =
		    content_length_left ? std::min(*content_length_left, body_data.size()) : body_data.size();

		if (!was_simple_write(was, body_data.data(), write_len)) {
			throw std::runtime_error("was_simple_write failed");
		}

		if (content_length_left) {
			if (body_data.size() > content_length_left) {
				throw std::runtime_error(
				    fmt::format("Attempting to send {} bytes, but only {} bytes left to sent",
						body_data.size(),
						*content_length_left));
			}
			*content_length_left -= body_data.size();
		}
	}
};
}

void
Was::ProcessRequest(RequestHandler &handler, std::string_view uri) noexcept
{
	const auto method = was_simple_get_method(was);
	if (method == HTTP_METHOD_INVALID) {
		fmt::print(stderr, "Invalid method: {}\n", method);
		if (!was_simple_status(was, HTTP_STATUS_METHOD_NOT_ALLOWED)) {
			fmt::print(stderr, "Error in was_simple_status\n");
		}
		return;
	}

	const auto remote_host = was_simple_get_remote_host(was);
	const auto script_name = was_simple_get_script_name(was);
	const auto path = was_simple_get_path_info(was);
	const auto query = was_simple_get_query_string(was);
	const auto parsed_uri = Uri::split(uri);

	// REMOTE_HOST is set to "<ip>:<port>" by beng-proxy.
	const auto remote_host_sv = remote_host ? std::string_view(remote_host) : std::string_view();
	const auto remote_addr = remote_host_sv.substr(0, remote_host_sv.find(':'));

	HttpRequest request{
		.remote_addr = std::string(remote_addr),
		.script_name = script_name ? script_name : "",
		.server_name = "localhost",
		.server_port = "80",
		// We hard-code this, because there is simply no way to know and no application should depend on this
		// anyways.
		.protocol = "HTTP/1.1",
		// We also cannot know the scheme, but we can know if HTTPS was used externally, so we will set it
		// later, if we find the corresponding header.
		.scheme = "http",
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
		std::string_view value = elem->value;
		if (HeaderMatch(elem->name, "X-CM4all-HTTPS") && value == "on") {
			request.scheme = "https";
		}
		if (HeaderMatch(elem->name, "Host")) {
			const auto colon = value.find(':');
			request.server_name = value.substr(0, colon);
			if (colon != std::string_view::npos) {
				request.server_port = value.substr(colon + 1);
			}
		}
		request.headers.emplace_back(elem->name, value);
	}
	was_simple_iterator_free(it);

	if (request.server_port.empty()) {
		request.server_port = request.scheme == "https" ? "443" : "80";
	}

	if (was_simple_has_body(was)) {
		const auto input_remaining = was_simple_input_remaining(was);
		if (input_remaining < 0) {
			// TODO: I think this happens if we got DATA, but not LENGTH. What to do here?
			fmt::print(stderr, "was_simple_has_body is true, but was_simple_input_remaining < 0");
			if (!was_simple_abort(was)) {
				fmt::print(stderr, "Error in was_simple_abort\n");
			}
			return;
		}
		request.body = std::make_unique<WasInputStream>(was, input_remaining);
	}

	WasResponder responder{ was };
	try {
		handler.Process(std::move(request), responder);
	} catch (std::exception &exc) {
		// If was_simple_status, was_simple_set_header, etc. fail, the cause is either a programming error or an
		// IO Error on the command channel, in which case it doesn't make sense to do anything else and we will
		// likely terminate soon.
		// I log the errors for each function in case it was a programming error (wrong state).
		// In the case of an IO Error, was_simple_accept will clean up the current request and either fail
		// itself in which case we gracefully terminate the loop in `Run` and exit the program or it will clean
		// up the current request and try another one.
		// We don't know what kind of exception we got, so it's possible a was_simple_* function failed, but
		// was_simple_abort will not do anything if the state is ERROR, so in case it was something else, we
		// abort the request here.
		fmt::print(stderr, "Exception handling request: {}\n", exc.what());
		if (!was_simple_abort(was)) {
			fmt::print(stderr, "Error in was_simple_abort\n");
		}
		return;
	}

	// We are supposed to log here and close the connection. We don't really have that option (anymore).
	// was_simple_accept will send PREMATURE and I hope that beng-proxy closes the connection by itself.
	if (responder.content_length_left > 0) {
		fmt::print(stderr, "{} bytes of response body data left to send\n", *responder.content_length_left);
	}
}

Was::Was() : was(was_simple_new()) {}
Was::~Was() { was_simple_free(was); }

void
Was::Run(RequestHandler &handler) noexcept
{
	const char *uri = nullptr;
	while ((uri = was_simple_accept(was))) {
		ProcessRequest(handler, uri);
	}
}
