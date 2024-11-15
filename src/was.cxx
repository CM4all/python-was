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
	WasInputStream(struct was_simple *was) : was(was) {}

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
	struct was_simple *was;
	size_t content_length_left = 0;

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

		// TODO: Maybe use chunked transfer without Content-Length?
		content_length_left = response.content_length.value_or(0);

		if (content_length_left == 0) {
			if (!was_simple_end(was)) {
				throw std::runtime_error("was_simple_end failed");
			}
		} else {
			// This should be done early, but the state won't match if I do it any earlier than here
			if (!was_simple_set_length(was, content_length_left)) {
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
