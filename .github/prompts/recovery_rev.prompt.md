### FastFHIR Architectural Change: Semantic Recovery Tag Implementation (Bit-Masking)

#### 1. Core Definition Updates (FF_Primitives.hpp)
Modify the RECOVERY_TAG system to transition from a Structural ID (identifying only memory layout) to a Semantic Descriptor (identifying layout and content simultaneously).
- Define FF_ARRAY_BIT as 0x8000 (uint16_t). This high bit serves as an orthogonal modifier indicating the block follows the FF_ARRAY V-Table layout.
- Define FF_TYPE_MASK as 0x7FFF (uint16_t). This mask is used to extract the underlying clinical resource ID or data type ID from any recovery tag.
- Add inline helper functions: IsArrayTag(tag) which returns true if the high bit is set, and GetTypeFromTag(tag) which returns the result of ANDing the tag with FF_TYPE_MASK.

#### 2. Validation Logic Hardening (FF_Primitives.hpp / DATA_BLOCK)
Update the physical validation layer to support composite tags stored in the binary stream.
- In DATA_BLOCK::validate_offset, replace the exact equality check between the loaded tag and the expected tag.
- The new validation logic must allow a match if both tags have the FF_ARRAY_BIT set (logical agreement on array structure) or if the masked types match exactly. This ensures that a block stored as Array of Patient (0x8302) passes validation when the parser expects an Array structure.

#### 3. Metadata-Aware Traversal (FF_Builder.hpp)
Refactor the lightweight ObjectHandle to preserve type information through the traversal chain without increasing its 16-byte memory footprint.
- Update ObjectHandle::is_array() to return true if the high bit of its internal m_recovery field is set.
- Update ObjectHandle::operator[](size_t index) to implement "Metadata Injection." When accessing an array element, the indexer must strip the FF_ARRAY_BIT from the parent handle's tag to "inject" the correct child recovery tag into the resulting MutableEntry.
- This ensures that root[Bundle.ENTRY][0] produces a handle already typed as a BundleEntry rather than an UNDEFINED block.

#### 4. Lossless Handle Elevation (FF_Builder.hpp / MutableEntry)
Modify the bridge between read-only Nodes and writable Handles.
- Update the ObjectHandle constructor that accepts a Node to be lossless. It must copy m_recovery, m_child_recovery, and m_kind from the Node to ensure the Handle retains all schema context discovered during parsing.
- In MutableEntry::as_handle(), add a conditional peek for polymorphic fields (FHIR Choice [x] or Resource). If the field kind is FF_FIELD_RESOURCE or FF_FIELD_CHOICE, the handle must read the concrete recovery tag from the 10-byte reference in the stream memory rather than relying on the schema-defined tag.

#### 5. Type-Safe Accessors (FF_Parser.hpp)
Update the Node::as<T>() template to be "bitmask-blind."
- Before comparing the Node's m_recovery with TypeTraits<T>::recovery, apply GetTypeFromTag() to the internal tag. This allows a Node to be safely cast to its POD structure even if it is currently being held as part of an array handle.

#### 6. Generator Logic Update (ffc.py)
Update the code generator to bake semantic tags directly into the binary file format and Python field tokens.
- In generate_store_fields, update the logic for array blocks. Instead of storing the generic RECOVER_FF_ARRAY (0x0003), calculate a packed tag using (FF_ARRAY_BIT | child_recovery_tag).
- Pass this packed tag to the STORE_FF_ARRAY_HEADER function so it is physically written into the 8th byte of the array block on disk.
- Update the Python Field Registry emission to include the FF_ARRAY_BIT in the owner_recovery metadata for any field where is_array is true. This allows the Python API to start the traversal with a "Smart Handle."

#### 7. Dispatcher Normalization (FF_Reflection.cpp & FF_IngestMappings.cpp)
Standardize all switch statements that branch based on RECOVERY_TAG.
- Within reflected_fields, reflected_keys, and resource dispatchers, apply GetTypeFromTag() to the incoming tag before entering the switch block.
- This prevents composite tags (like Array of Observation) from hitting the default error case, as they will now correctly resolve to their base resource type handlers.