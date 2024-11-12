#pragma once

#include <vector>
#include <string>
#include <optional>

#include <http/Method.hxx>
#include <util/CharUtil.hxx>
#include <io/UniqueFileDescriptor.hxx>

struct HttpRequestHeader {
	std::string protocol; // e.g. HTTP/1.1
	std::string scheme; // TODO: probably hardcode this to 'http'
	HttpMethod method;
	std::string uri;
	std::vector<std::pair<std::string, std::string>> headers;

	static bool HeaderMatch(std::string_view a, std::string_view b) {
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

struct RequestHandler {
	virtual ~RequestHandler() = default;
	virtual void OnRequest(const HttpRequestHeader& req) = 0;
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
