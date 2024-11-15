#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <http/method.h>
#include <http/status.h>

// So far this whole Stream abstraction doesn't even make sense, but I hope it will make
// moving to an async solution easier.

class InputStream {
	size_t content_length = 0;

public:
	InputStream(uint64_t content_length) : content_length(content_length) {}

	virtual ~InputStream() = default;

	// Read up to `size` bytes, size=nullopt reads until EOF or dest.siz()
	// returns number of bytes read, 0 on EOF
	// Throws on error
	virtual size_t Read(std::span<char> dest) = 0;

	uint64_t ContentLength() const { return content_length; }
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

struct OutputStream {
	virtual ~OutputStream() = default;
	virtual bool Write(std::span<const char> src) = 0;
};

class StringOutputStream : public OutputStream {
	std::string data_;

public:
	bool Write(std::span<const char> src) override;

	operator std::string_view() const { return data_; }
	const char *data() const { return data_.data(); }
	size_t size() const { return data_.size(); }
};

struct Uri {
	std::string_view path;
	std::string_view query;

	[[gnu::pure]] static Uri split(std::string_view uri) noexcept;
};

[[gnu::pure]] bool
HeaderMatch(std::string_view a, std::string_view b) noexcept;

struct HttpRequest {
	std::string script_name;
	std::string protocol; // e.g. HTTP/1.1
	std::string scheme;   // TODO: probably hardcode this to 'http'
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

struct HttpResponder {
	virtual void SendHeaders(HttpResponse &&response) = 0;
	virtual void SendBody(std::string_view body_data) = 0;
	bool HeadersSent() const { return headers_sent; }

protected:
	bool headers_sent = false;
};

struct RequestHandler {
	virtual void Process(HttpRequest &&request, HttpResponder &responder) = 0;
};
