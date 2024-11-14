#pragma once

#include <string_view>

#include "http.hxx"

struct was_simple;

class Was {
	struct was_simple *was;

	void EndRequest() noexcept;
	void ProcessRequest(RequestHandler &handler, std::string_view url) noexcept;

public:
	Was();
	~Was();
	operator struct was_simple *() const { return was; }

	void Run(RequestHandler &handler) noexcept;
};
