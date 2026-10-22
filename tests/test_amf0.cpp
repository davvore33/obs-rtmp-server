// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 obs-rtmp-server contributors
#include "test_util.h"

using namespace rtmp::amf0;

static void test_round_trip()
{
	Value obj = Value::object();
	obj.set("app", "live").set("capabilities", 31.0);
	obj.set("flag", Value::from_bool(true));
	Value nested = Value::object();
	nested.set("inner", "x");
	obj.set("nested", nested);

	Value arr = Value::ecma_array();
	arr.set("width", 1920.0).set("height", 1080.0);

	Value strict;
	strict.type = Type::StrictArray;
	strict.items.push_back(Value::from_number(1));
	strict.items.push_back(Value::from_string("two"));

	std::vector<uint8_t> bytes;
	encode(Value::from_string("connect"), bytes);
	encode(Value::from_number(1), bytes);
	encode(obj, bytes);
	encode(Value::null(), bytes);
	encode(Value::undefined(), bytes);
	encode(arr, bytes);
	encode(strict, bytes);

	std::vector<Value> out;
	CHECK(decode_all(bytes.data(), bytes.size(), out));
	CHECK_EQ(out.size(), size_t(7));
	CHECK_EQ(out[0].as_string(), std::string("connect"));
	CHECK_EQ(out[1].as_number(), 1.0);
	CHECK(out[2].type == Type::Object);
	CHECK_EQ(out[2].find("app")->as_string(), std::string("live"));
	CHECK_EQ(out[2].find("capabilities")->as_number(), 31.0);
	CHECK(out[2].find("flag")->boolean);
	CHECK_EQ(out[2].find("nested")->find("inner")->as_string(), std::string("x"));
	CHECK(out[2].find("missing") == nullptr);
	CHECK(out[3].type == Type::Null);
	CHECK(out[4].type == Type::Undefined);
	CHECK(out[5].type == Type::EcmaArray);
	CHECK_EQ(out[5].find("height")->as_number(), 1080.0);
	CHECK(out[6].type == Type::StrictArray);
	CHECK_EQ(out[6].items.size(), size_t(2));
	CHECK_EQ(out[6].items[1].as_string(), std::string("two"));
}

static void test_property_order_and_long_string()
{
	Value obj = Value::object();
	obj.set("b", 1.0).set("a", 2.0);
	std::vector<uint8_t> bytes;
	encode(obj, bytes);
	const uint8_t *p = bytes.data();
	Value out;
	CHECK(decode(p, bytes.data() + bytes.size(), out));
	CHECK_EQ(out.properties[0].first, std::string("b"));
	CHECK_EQ(out.properties[1].first, std::string("a"));
	CHECK(p == bytes.data() + bytes.size());

	std::string big(70000, 'x');
	bytes.clear();
	encode(Value::from_string(big), bytes);
	CHECK_EQ(bytes[0], uint8_t(0x0C)); // long string marker
	p = bytes.data();
	CHECK(decode(p, bytes.data() + bytes.size(), out));
	CHECK_EQ(out.string.size(), big.size());
}

static void test_malformed_input()
{
	std::vector<Value> out;

	// Truncated number.
	const uint8_t truncated[] = {0x00, 0x3f, 0xf0};
	CHECK(!decode_all(truncated, sizeof(truncated), out));

	// String claims more bytes than exist.
	const uint8_t short_string[] = {0x02, 0x00, 0x10, 'a', 'b'};
	out.clear();
	CHECK(!decode_all(short_string, sizeof(short_string), out));

	// Unsupported marker (reference).
	const uint8_t reference[] = {0x07, 0x00, 0x01};
	out.clear();
	CHECK(!decode_all(reference, sizeof(reference), out));

	// Unterminated object.
	const uint8_t unterminated[] = {0x03, 0x00, 0x01, 'a', 0x05};
	out.clear();
	CHECK(!decode_all(unterminated, sizeof(unterminated), out));

	// Strict array claiming 4 billion elements must fail without allocating them.
	const uint8_t array_bomb[] = {0x0A, 0xFF, 0xFF, 0xFF, 0xFF, 0x05};
	out.clear();
	CHECK(!decode_all(array_bomb, sizeof(array_bomb), out));

	// Deeply nested objects hit the depth limit instead of the stack.
	std::vector<uint8_t> deep;
	for (int i = 0; i < 1000; i++) {
		const uint8_t open[] = {0x03, 0x00, 0x01, 'k'};
		deep.insert(deep.end(), open, open + sizeof(open));
	}
	out.clear();
	CHECK(!decode_all(deep.data(), deep.size(), out));

	// Values decoded before the failure are kept.
	std::vector<uint8_t> mixed;
	encode(Value::from_string("ok"), mixed);
	mixed.push_back(0x07);
	out.clear();
	CHECK(!decode_all(mixed.data(), mixed.size(), out));
	CHECK_EQ(out.size(), size_t(1));
}

int main()
{
	test_round_trip();
	test_property_order_and_long_string();
	test_malformed_input();
	return finish_tests("amf0");
}
