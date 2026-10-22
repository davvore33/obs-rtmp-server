// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 obs-rtmp-server contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rtmp::amf0 {

enum class Type : uint8_t { Number, Boolean, String, Object, Null, Undefined, EcmaArray, StrictArray };

struct Value {
	Type type = Type::Null;
	double number = 0;
	bool boolean = false;
	std::string string;
	// Object / EcmaArray entries, in wire order.
	std::vector<std::pair<std::string, Value>> properties;
	// StrictArray elements.
	std::vector<Value> items;

	static Value null() { return Value(); }
	static Value undefined();
	static Value from_number(double n);
	static Value from_bool(bool b);
	static Value from_string(std::string s);
	static Value object();
	static Value ecma_array();

	Value &set(std::string key, Value v);
	Value &set(std::string key, const char *s);
	Value &set(std::string key, const std::string &s);
	Value &set(std::string key, double n);

	const Value *find(const std::string &key) const;

	bool is_string() const { return type == Type::String; }
	bool is_number() const { return type == Type::Number; }
	std::string as_string(const std::string &fallback = std::string()) const;
	double as_number(double fallback = 0) const;
};

// Decodes one value at `p`, advancing it. Returns false on malformed or
// unsupported input (references, AMF3 switch, ...).
bool decode(const uint8_t *&p, const uint8_t *end, Value &out);

// Decodes consecutive values until the data is exhausted or a value fails to
// parse. The values decoded so far are kept either way; the return value says
// whether everything was consumed cleanly.
bool decode_all(const uint8_t *data, size_t size, std::vector<Value> &out);

void encode(const Value &value, std::vector<uint8_t> &out);

} // namespace rtmp::amf0
