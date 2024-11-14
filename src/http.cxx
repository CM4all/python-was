#include "http.hxx"

#include <util/CharUtil.hxx>

size_t
StringInputStream::Read(std::span<char> dest)
{
	const auto to_read = std::min(data.size() - cursor, dest.size());
	const auto begin = data.begin() + cursor;
	std::copy(begin, begin + to_read, dest.begin());
	cursor += to_read;
	return to_read;
}

bool
StringOutputStream::Write(std::span<const char> src)
{
	data_.append(std::string_view(src.data(), src.size()));
	return true;
}

[[gnu::pure]] Uri
Uri::split(std::string_view uri) noexcept
{
	const auto q = uri.find('?');
	const auto path = uri.substr(0, q);
	const auto query = q == std::string_view::npos ? std::string_view{} : uri.substr(q);
	return Uri{ .path = path, .query = query };
}

[[gnu::pure]] bool
HeaderMatch(std::string_view a, std::string_view b) noexcept
{
	if (a.empty() || a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); ++i) {
		if (ToLowerASCII(a[i]) != ToLowerASCII(b[i])) {
			return false;
		}
	}
	return true;
}

std::optional<std::string_view>
HttpRequest::FindHeader(std::string_view header_name) const noexcept
{
	for (const auto &[name, value] : headers) {
		if (HeaderMatch(name, header_name)) {
			return value;
		}
	}
	return std::nullopt;
}
