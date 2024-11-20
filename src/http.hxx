#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <http/method.h>
#include <http/status.h>

class InputStream {
	std::optional<uint64_t> content_length = 0;

public:
	InputStream(std::optional<uint64_t> content_length) : content_length(content_length) {}

	virtual ~InputStream() = default;

	// Read up to `size` bytes, size=nullopt reads until EOF or dest.siz()
	// returns number of bytes read, 0 on EOF
	// Throws on error
	virtual size_t Read(std::span<char> dest) = 0;

	std::optional<uint64_t> ContentLength() const { return content_length; }
};

struct NullInputStream : public InputStream {
	NullInputStream() : InputStream(0) {}
	size_t Read(std::span<char>) override { return 0; }
};

class StringInputStream : public InputStream {
	std::string data;
	size_t cursor = 0;

public:
	StringInputStream(std::string str) : InputStream(str.size()), data(std::move(str)) {}

	size_t Read(std::span<char> dest) override;
};

struct Uri {
	std::string_view path;
	std::string_view query;

	[[gnu::pure]] static Uri split(std::string_view uri) noexcept;
};

[[gnu::pure]] bool
HeaderMatch(std::string_view a, std::string_view b) noexcept;

struct HttpRequest {
	std::string remote_addr;
	std::string script_name;
	std::string server_name;
	std::string server_port;
	std::string protocol; // e.g. HTTP/1.1
	std::string scheme;
	http_method_t method;
	Uri uri;
	std::vector<std::pair<std::string, std::string>> headers;
	std::unique_ptr<InputStream> body;

	std::optional<std::string_view> FindHeader(std::string_view header_name) const noexcept;
};

struct HttpResponse {
	http_status_t status = static_cast<http_status_t>(0);
	std::vector<std::pair<std::string, std::string>> headers;
	std::optional<uint64_t> content_length;
};

class HttpResponder {
	bool headers_sent = false;

protected:
	virtual void SendHeadersImpl(HttpResponse &&response) = 0;
	virtual void SendBodyImpl(std::string_view body_data) = 0;

public:
	void SendHeaders(HttpResponse &&response)
	{
		assert(!headers_sent);
		SendHeadersImpl(std::move(response));
		headers_sent = true;
	}

	void SendBody(std::string_view body_data)
	{
		assert(headers_sent);
		SendBodyImpl(body_data);
	}

	bool HeadersSent() const { return headers_sent; }
};

struct RequestHandler {
	virtual void Process(HttpRequest &&request, HttpResponder &responder) = 0;
};
