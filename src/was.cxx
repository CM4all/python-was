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
	bool chunked_transfer = false;
	size_t content_length_left = 0;

	WasResponder(struct was_simple *was) : was(was) {}

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
		if (!was_simple_end(was)) {
			fmt::print(stderr, "Error in was_simple_end\n");
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
		// If was_simple_status, was_simple_set_header, etc. fail, the cause was likely an IO on the
		// command channel, in which case it doesn't make sense to do anything else and we will
		// likely terminate soon.
		// Abort will not do anything if the state is ERROR, EndRequest will discard input and flush the control
		// channel, so EndRequest would make more sense, but we don't know when exactly we failed, so I abort
		// for now.
		// TODO: Ask Max how to deal with this properly.
		fmt::print(stderr, "Exception handling request: {}\n", exc.what());
		if (!was_simple_abort(was)) {
			fmt::print(stderr, "Error in was_simple_abort\n");
		}
		return;
	}

	// Either this is a programming error, or an IO error and in both cases we can't do
	// much more than log and proceed to the next request.
	if (!was_simple_end(was)) {
		fmt::print(stderr, "Error in was_simple_end\n");
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
