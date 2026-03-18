/**
 * @file FF_Parser.cpp
 * @author Ryan Landvater (ryan.landvater@example.com)
 * @brief 
 * @version 0.1
 * @date 2026-03-18
 * 
 * @copyright Copyright (c) 2026
 * 
 */

#include "../include/FF_Utilities.hpp"
#include "../include/FF_Parser.hpp"
#include "../generated_src/FF_Dictionary.hpp"
#include "../generated_src/FF_Reflection.hpp"

namespace FastFHIR {

Parser::Parser(const void* buffer, size_t size)
	: m_base(static_cast<const BYTE*>(buffer)),
	  m_size(size),
	  m_version(0),
	  m_root_offset(FF_NULL_OFFSET),
	  m_root_recovery(0) {
	if (size < FF_FILE_HEADER::HEADER_SIZE) {
		throw std::runtime_error("FastFHIR: Buffer too small to contain file header.");
	}

	FF_FILE_HEADER header(size);
	auto res = header.validate_full(m_base);
	if (!res) {
		throw std::runtime_error("FastFHIR Header Validation: " + res.message);
	}

	m_version = header.get_version(m_base);
	m_root_offset = header.get_root(m_base);
	m_root_recovery = header.get_root_type(m_base);
}

std::string_view Parser::expected_checksum() const {
	FF_FILE_HEADER header(m_size);
	FF_CHECKSUM checksum = header.get_checksum(m_base);

	if (!checksum || checksum.__offset + FF_CHECKSUM::HEADER_SIZE > m_size) {
		return {};
	}

	return checksum.get_hash_view(m_base);
}

Node Parser::root() const {
	return Node(m_base, m_size, m_version, m_root_offset, m_root_recovery, FF_FIELD_BLOCK);
}

std::vector<FF_FieldInfo> Node::fields() const {
	if (!is_object()) return {};
	return reflected_fields(m_recovery);
}

std::vector<std::string_view> Node::keys() const {
	if (!is_object()) return {};
	return reflected_keys(m_recovery);
}

size_t Node::size() const {
	if (is_array()) {
		FF_ARRAY array(m_offset, m_size, m_version);
		return array.entry_count(m_base);
	}
	if (is_object()) {
		return fields().size();
	}
	return 0;
}

std::vector<Node> Node::entries() const {
	std::vector<Node> out;
	if (!is_array()) return out;
	FF_ARRAY array(m_offset, m_size, m_version);
	auto count = array.entry_count(m_base);
	out.reserve(count);
	for (uint32_t i = 0; i < count; ++i) {
		out.push_back((*this)[static_cast<size_t>(i)]);
	}
	return out;
}

Node Node::operator[](std::string_view key) const {
	if (!is_object()) return {};
	return reflected_child_node(m_base, m_size, m_version, m_offset, m_recovery, key);
}

Node Node::operator[](FF_FieldKey key) const {
	if (!is_object()) return {};

	if (key.owner_recovery != 0 && key.owner_recovery != m_recovery) {
		return {};
	}

	if (key.kind == FF_FIELD_UNKNOWN) {
		if (!key.name.empty()) return (*this)[key.name];
		return {};
	}

	const Offset value_offset = m_offset + key.field_offset;
	if (key.kind == FF_FIELD_STRING) {
		const Offset child_offset = LOAD_U64(m_base + value_offset);
		if (child_offset == FF_NULL_OFFSET) return {};
		return Node(m_base, m_size, m_version, child_offset, FF_STRING::recovery, FF_FIELD_STRING);
	}
	if (key.kind == FF_FIELD_ARRAY) {
		const Offset child_offset = LOAD_U64(m_base + value_offset);
		if (child_offset == FF_NULL_OFFSET) return {};
		return Node(m_base, m_size, m_version, child_offset, FF_ARRAY::recovery, FF_FIELD_ARRAY,
					key.child_recovery, key.array_entries_are_offsets != 0);
	}
	if (key.kind == FF_FIELD_BLOCK) {
		const Offset child_offset = LOAD_U64(m_base + value_offset);
		if (child_offset == FF_NULL_OFFSET) return {};
		return Node(m_base, m_size, m_version, child_offset, key.child_recovery, FF_FIELD_BLOCK);
	}

	return Node::scalar(m_base, m_size, m_version, value_offset, key.kind);
}

Node Node::operator[](size_t index) const {
	if (!is_array()) return {};
	FF_ARRAY array(m_offset, m_size, m_version);
	auto count = array.entry_count(m_base);
	if (index >= count) return {};
	const BYTE* entry = array.entries(m_base) + index * array.entry_step(m_base);
	if (m_array_entries_are_offsets) {
		Offset child_offset = LOAD_U64(entry);
		if (child_offset == FF_NULL_OFFSET) return {};
		return Node(m_base, m_size, m_version, child_offset, m_child_recovery, FF_FIELD_STRING);
	}
	return Node(m_base, m_size, m_version, static_cast<Offset>(entry - m_base), m_child_recovery, FF_FIELD_BLOCK);
}

std::string_view Node::as_string() const {
	if (m_kind == FF_FIELD_STRING && m_offset != FF_NULL_OFFSET) {
		return FF_STRING(m_offset, m_size, m_version).read_view(m_base);
	}
	if (m_kind == FF_FIELD_CODE && m_scalar_offset != FF_NULL_OFFSET) {
		return FF_ResolveCode(LOAD_U32(m_base + m_scalar_offset), m_version);
	}
	return {};
}

bool Node::as_bool() const {
	if (m_kind != FF_FIELD_BOOL || m_scalar_offset == FF_NULL_OFFSET) return false;
	return LOAD_U8(m_base + m_scalar_offset) != 0;
}

uint32_t Node::as_uint32() const {
	if (m_kind != FF_FIELD_UINT32 || m_scalar_offset == FF_NULL_OFFSET) return 0;
	return LOAD_U32(m_base + m_scalar_offset);
}

double Node::as_float64() const {
	if (m_kind != FF_FIELD_FLOAT64 || m_scalar_offset == FF_NULL_OFFSET) return 0.0;
	return LOAD_F64(m_base + m_scalar_offset);
}

} // namespace FastFHIR
