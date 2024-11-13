#pragma once

#include <vector>
#include <string>
#include <optional>
#include <memory>

#include <http/method.h>
#include <http/status.h>
#include <util/CharUtil.hxx>

struct InputStream {
	virtual ~InputStream() = default;
	// Read up to size bytes, size=nullopt reads until EOF
	virtual std::string_view Read(std::optional<size_t> size) = 0;
};

struct NullInputStream : public InputStream {
	std::string_view Read(std::optional<size_t>) override
	{
		return {};
	}
};

class StringInputStream : public InputStream {
	std::string data;
	size_t cursor = 0;

public:
	StringInputStream(std::string str) : data(std::move(str)) {}

	std::string_view Read(std::optional<size_t> size) override
	{
		if (size) {
			return std::string_view(data).substr(cursor, *size);
		}
		return std::string_view(data).substr(cursor);
	}
};

struct Uri {
	std::string_view path;
	std::string_view query;

	static Uri split(std::string_view uri) {
		const auto q = uri.find('?');
		const auto path = uri.substr(0, q);
		const auto query = q == std::string_view::npos ? std::string_view{} : uri.substr(q);
		return Uri { .path = path, .query = query };
	}
};

inline bool HeaderMatch(std::string_view a, std::string_view b) {
	if (a.empty() || a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); ++i)
	{
		if (ToLowerASCII(a[i]) != ToLowerASCII(b[i])) {
			return false;
		}
	}
	return true;
}

struct HttpRequest {
	std::string script_name;
	std::string protocol; // e.g. HTTP/1.1
	std::string scheme; // TODO: probably hardcode this to 'http'
	http_method_t method;
	Uri uri;
	std::vector<std::pair<std::string, std::string>> headers;
	std::unique_ptr<InputStream> body;



	std::optional<std::string_view> FindHeader(std::string_view header_name) const
	{
		for (const auto& [name, value] : headers)
		{
			if (HeaderMatch(name, header_name)) {
				return value;
			}
		}
		return std::nullopt;
	}
};

struct HttpResponse {
	http_status_t status;
	std::vector<std::pair<std::string, std::string>> headers;
	std::string body;
};

struct RequestHandler {
	virtual ~RequestHandler() = default;
	virtual HttpResponse OnRequest(HttpRequest&& req) = 0;
};
