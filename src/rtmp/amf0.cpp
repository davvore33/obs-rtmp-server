// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 obs-rtmp-server contributors
#include "rtmp/amf0.h"

#include "rtmp/bytes.h"

#include <cstring>

namespace rtmp::amf0 {

namespace {

enum Marker : uint8_t {
	kNumber = 0x00,
	kBoolean = 0x01,
	kString = 0x02,
	kObject = 0x03,
	kNull = 0x05,
	kUndefined = 0x06,
	kEcmaArray = 0x08,
	kObjectEnd = 0x09,
	kStrictArray = 0x0A,
	kDate = 0x0B,
	kLongString = 0x0C,
};

constexpr int kMaxDepth = 16;

bool read_double(const uint8_t *&p, const uint8_t *end, double &out)
{
	if (end - p < 8)
		return false;
	uint64_t bits = 0;
	for (int i = 0; i < 8; i++)
		bits = (bits << 8) | p[i];
	std::memcpy(&out, &bits, sizeof(out));
	p += 8;
	return true;
}

bool read_utf8(const uint8_t *&p, const uint8_t *end, size_t length, std::string &out)
{
	if (static_cast<size_t>(end - p) < length)
		return false;
	out.assign(reinterpret_cast<const char *>(p), length);
	p += length;
	return true;
}

bool decode_value(const uint8_t *&p, const uint8_t *end, Value &out, int depth);

// Reads `key: value` pairs until the 00 00 09 terminator.
bool decode_properties(const uint8_t *&p, const uint8_t *end, Value &out, int depth)
{
	for (;;) {
		if (end - p < 3)
			return false;
		size_t key_len = rd16(p);
		if (key_len == 0 && p[2] == kObjectEnd) {
			p += 3;
			return true;
		}
		p += 2;
		std::string key;
		if (!read_utf8(p, end, key_len, key))
			return false;
		Value v;
		if (!decode_value(p, end, v, depth + 1))
			return false;
		out.properties.emplace_back(std::move(key), std::move(v));
	}
}

bool decode_value(const uint8_t *&p, const uint8_t *end, Value &out, int depth)
{
	if (depth > kMaxDepth || p >= end)
		return false;

	out = Value();
	uint8_t marker = *p++;

	switch (marker) {
	case kNumber:
		out.type = Type::Number;
		return read_double(p, end, out.number);

	case kBoolean:
		if (p >= end)
			return false;
		out.type = Type::Boolean;
		out.boolean = *p++ != 0;
		return true;

	case kString: {
		if (end - p < 2)
			return false;
		size_t len = rd16(p);
		p += 2;
		out.type = Type::String;
		return read_utf8(p, end, len, out.string);
	}

	case kLongString: {
		if (end - p < 4)
			return false;
		size_t len = rd32(p);
		p += 4;
		out.type = Type::String;
		return read_utf8(p, end, len, out.string);
	}

	case kObject:
		out.type = Type::Object;
		return decode_properties(p, end, out, depth);

	case kEcmaArray:
		if (end - p < 4)
			return false;
		p += 4; // element count is advisory; the terminator is authoritative
		out.type = Type::EcmaArray;
		return decode_properties(p, end, out, depth);

	case kStrictArray: {
		if (end - p < 4)
			return false;
		size_t count = rd32(p);
		p += 4;
		// Every element takes at least one byte, which bounds the allocation.
		if (count > static_cast<size_t>(end - p))
			return false;
		out.type = Type::StrictArray;
		out.items.reserve(count);
		for (size_t i = 0; i < count; i++) {
			Value item;
			if (!decode_value(p, end, item, depth + 1))
				return false;
			out.items.push_back(std::move(item));
		}
		return true;
	}

	case kDate: {
		// Milliseconds since the epoch plus a reserved time zone field.
		if (end - p < 10)
			return false;
		out.type = Type::Number;
		bool ok = read_double(p, end, out.number);
		p += 2;
		return ok;
	}

	case kNull:
		out.type = Type::Null;
		return true;

	case kUndefined:
		out.type = Type::Undefined;
		return true;

	default:
		// References, AMF3 switch, XML, typed objects: not used by publishers.
		return false;
	}
}

void encode_key(const std::string &key, std::vector<uint8_t> &out)
{
	size_t len = key.size() > 0xFFFF ? 0xFFFF : key.size();
	wr16(out, static_cast<uint16_t>(len));
	out.insert(out.end(), key.begin(), key.begin() + static_cast<std::ptrdiff_t>(len));
}

void encode_properties(const Value &value, std::vector<uint8_t> &out)
{
	for (const auto &[key, v] : value.properties) {
		encode_key(key, out);
		encode(v, out);
	}
	wr16(out, 0);
	wr8(out, kObjectEnd);
}

} // namespace

Value Value::undefined()
{
	Value v;
	v.type = Type::Undefined;
	return v;
}

Value Value::from_number(double n)
{
	Value v;
	v.type = Type::Number;
	v.number = n;
	return v;
}

Value Value::from_bool(bool b)
{
	Value v;
	v.type = Type::Boolean;
	v.boolean = b;
	return v;
}

Value Value::from_string(std::string s)
{
	Value v;
	v.type = Type::String;
	v.string = std::move(s);
	return v;
}

Value Value::object()
{
	Value v;
	v.type = Type::Object;
	return v;
}

Value Value::ecma_array()
{
	Value v;
	v.type = Type::EcmaArray;
	return v;
}

Value &Value::set(std::string key, Value v)
{
	properties.emplace_back(std::move(key), std::move(v));
	return *this;
}

Value &Value::set(std::string key, const char *s)
{
	return set(std::move(key), from_string(s));
}

Value &Value::set(std::string key, const std::string &s)
{
	return set(std::move(key), from_string(s));
}

Value &Value::set(std::string key, double n)
{
	return set(std::move(key), from_number(n));
}

const Value *Value::find(const std::string &key) const
{
	for (const auto &[k, v] : properties) {
		if (k == key)
			return &v;
	}
	return nullptr;
}

std::string Value::as_string(const std::string &fallback) const
{
	return type == Type::String ? string : fallback;
}

double Value::as_number(double fallback) const
{
	if (type == Type::Number)
		return number;
	if (type == Type::Boolean)
		return boolean ? 1.0 : 0.0;
	return fallback;
}

bool decode(const uint8_t *&p, const uint8_t *end, Value &out)
{
	return decode_value(p, end, out, 0);
}

bool decode_all(const uint8_t *data, size_t size, std::vector<Value> &out)
{
	const uint8_t *p = data;
	const uint8_t *end = data + size;
	while (p < end) {
		Value v;
		if (!decode_value(p, end, v, 0))
			return false;
		out.push_back(std::move(v));
	}
	return true;
}

void encode(const Value &value, std::vector<uint8_t> &out)
{
	switch (value.type) {
	case Type::Number: {
		wr8(out, kNumber);
		uint64_t bits;
		std::memcpy(&bits, &value.number, sizeof(bits));
		for (int shift = 56; shift >= 0; shift -= 8)
			out.push_back(static_cast<uint8_t>(bits >> shift));
		break;
	}
	case Type::Boolean:
		wr8(out, kBoolean);
		wr8(out, value.boolean ? 1 : 0);
		break;
	case Type::String:
		if (value.string.size() <= 0xFFFF) {
			wr8(out, kString);
			wr16(out, static_cast<uint16_t>(value.string.size()));
		} else {
			wr8(out, kLongString);
			wr32(out, static_cast<uint32_t>(value.string.size()));
		}
		out.insert(out.end(), value.string.begin(), value.string.end());
		break;
	case Type::Object:
		wr8(out, kObject);
		encode_properties(value, out);
		break;
	case Type::EcmaArray:
		wr8(out, kEcmaArray);
		wr32(out, static_cast<uint32_t>(value.properties.size()));
		encode_properties(value, out);
		break;
	case Type::StrictArray:
		wr8(out, kStrictArray);
		wr32(out, static_cast<uint32_t>(value.items.size()));
		for (const Value &item : value.items)
			encode(item, out);
		break;
	case Type::Null:
		wr8(out, kNull);
		break;
	case Type::Undefined:
		wr8(out, kUndefined);
		break;
	}
}

} // namespace rtmp::amf0
