/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * @file test_poco_values.cpp
 * @brief The generated *Data structs behave like C++ values.
 *
 * The shape Ryan asked for is the acceptance criterion, verbatim:
 *
 *     observation.code = CodeableConceptData{
 *         .coding = { CodingData{ .system = "http://loinc.org", ... } },
 *     };
 *
 * It needs two things that were not true. FHIR's `0..1` was a
 * std::unique_ptr, so every caller wrote make_unique before it could say
 * anything (FF_Optional). And a brace list can only COPY its elements, while
 * the structs were move-only through ChoiceEntry, so the inner list would not
 * compile whatever the member type was (copyable ChoiceEntry).
 *
 *   initializer  Ryan's snippet, compiled and stored
 *   copies       a copy is deep and independent, and survives its source
 *   absence      an unset FF_Optional is falsy and stores nothing
 *   choice       a copied ChoiceEntry does not share its block
 *   assignment   `value = QuantityData{...}` infers the variant tag
 *   strings      a field built from a runtime string outlives its source
 *   borrowing    a field assigned a view does not copy
 */

#include <FastFHIR.hpp>
#include <FF_FieldKeys.hpp>
#include "FF_AllTypes.hpp"

#include <sstream>
#include <string>
#include <type_traits>

#include "FFHR_tests.hpp"

using namespace FastFHIR;

namespace
{

static_assert(std::is_aggregate_v<CodingData>, "CodingData must stay brace-initializable");
static_assert(std::is_aggregate_v<CodeableConceptData>, "CodeableConceptData must stay brace-initializable");
static_assert(std::is_copy_constructible_v<CodingData>, "a POCO is a value");
static_assert(std::is_copy_constructible_v<ObservationData>, "a POCO is a value");
static_assert(std::is_copy_constructible_v<ChoiceEntry>, "a choice slot is a value");
static_assert(sizeof(FF_Optional<CodingData>) == sizeof(void *), "FF_Optional stays one pointer wide");

// The POCO string member. A literal must reach it without naming a type, and a
// std::string must be accepted by value -- those two are what let a brace list
// carry text at all. Being convertible BOTH ways is what makes it usable in the
// places std::string_view and std::string were used before.
static_assert(std::is_same_v<decltype(CodingData::system), FastFHIR::String>,
              "POCO string members are FastFHIR::String");
static_assert(std::is_convertible_v<FastFHIR::String, std::string_view>,
              "String converts to a view for the emitters");
static_assert(std::is_convertible_v<FastFHIR::String, std::string>,
              "String converts to an owning std::string for C-string callers");
static_assert(std::is_nothrow_move_constructible_v<FastFHIR::String>,
              "a move must not throw -- the assignment operators release first");
// The consteval literal constructor -- `field = runtime_char_pointer` is a
// compile error -- deliberately has NO static_assert here. is_constructible_v
// does not evaluate consteval-ness: it reports the constructor as viable, and
// the error fires only at a call with a non-constant argument. The property is
// real but not expressible as a trait, so it is pinned by the compile-failure
// note in FF_String.hpp rather than faked with an assertion that passes for
// the wrong reason.

/// Seal one resource and hand back what it exports, so the assertions are about
/// bytes that were written rather than about the struct in hand.
std::string sealed_json(const ObservationData &observation)
{
    // CHECK rather than REQUIRE: REQUIRE returns void, and this helper owes its
    // caller a string. A failure here is reported and the caller's assertions
    // then fail on the empty result.
    FF_Builder builder;
    if (!FF_CreateBuilder(FF_BuilderCreateInfo{.capacity = 1 << 20}, builder).succeeded())
    {
        CHECK(false, "create builder");
        return {};
    }
    const auto root = builder->append_obj(observation);
    if (!FF_BuilderSetRoot(FF_BuilderSetRootInfo{.builder = builder, .root = root}).succeeded())
    {
        CHECK(false, "set root");
        return {};
    }
    Memory::View sealed;
    if (!FF_BuilderFinalize(FF_BuilderFinalizeInfo{.builder = builder}, sealed).succeeded())
    {
        CHECK(false, "finalize");
        return {};
    }
    Parser parser;
    if (!FF_Parse(FF_ParseInfo{.buffer = sealed.data(), .size = sealed.size()}, parser).succeeded())
    {
        CHECK(false, "parse");
        return {};
    }
    std::ostringstream json;
    parser.root().print_json(json);
    return json.str();
}

// ─────────────────────────────────────────────────────────────────────────────

void ryans_initializer_compiles_and_stores()
{
    TEST_GROUP("initializer");

    ObservationData observation;
    observation.id     = "poco-1";
    observation.status = FF_ObservationStatus::Final;

    // Verbatim from the review. No make_unique, no push_back, no std::move.
    observation.code = CodeableConceptData{
        .coding = {
            CodingData{
                .system  = "http://loinc.org",
                .code    = "2345-7",
                .display = "Glucose [Mass/volume] in Serum or Plasma",
            },
        },
    };

    CHECK(observation.code != nullptr, "the optional child is present");
    CHECK(observation.code->coding.size() == 1, "the brace list built one Coding");

    const std::string json = sealed_json(observation);
    CHECK(json.find("\"system\":\"http://loinc.org\"") != std::string::npos,
          "the coding system reached the wire: " << json);
    CHECK(json.find("\"code\":\"2345-7\"") != std::string::npos, "and the code did too");
}

void a_copy_is_deep_and_outlives_its_source()
{
    TEST_GROUP("copies");

    ObservationData copy;
    {
        ObservationData original;
        original.id     = "poco-copy";
        original.status = FF_ObservationStatus::Final;
        original.code   = CodeableConceptData{.text = "original text"};

        copy = original;  // a value, so this is a copy, not a share
        CHECK(copy.code != nullptr && original.code != nullptr, "both hold a child");
        CHECK(copy.code.get() != original.code.get(), "and they are different objects");

        original.code->text = "changed after the copy";
    }
    // `original` is gone; the copy owns everything it needs.
    REQUIRE(copy.code != nullptr, "the copy kept its child");
    CHECK(copy.code->text == "original text", "and the source's later edit did not reach it");
}

void an_unset_optional_is_absent()
{
    TEST_GROUP("absence");

    ObservationData observation;
    observation.id     = "poco-absent";
    observation.status = FF_ObservationStatus::Final;

    CHECK(observation.code == nullptr, "an untouched FF_Optional is null");
    CHECK(!static_cast<bool>(observation.code), "and falsy");

    const std::string json = sealed_json(observation);
    CHECK(json.find("\"code\"") == std::string::npos, "an absent field writes nothing: " << json);

    observation.code = CodeableConceptData{.text = "present now"};
    CHECK(observation.code != nullptr, "assigning a value makes it present");
    observation.code = nullptr;
    CHECK(observation.code == nullptr, "and nullptr clears it again");
}

void a_copied_choice_does_not_share_its_block()
{
    TEST_GROUP("choice");

    ObservationData original;
    original.id     = "poco-choice";
    original.status = FF_ObservationStatus::Final;

    QuantityData quantity;
    quantity.value = 94.0;
    quantity.unit  = "mg/dL";
    original.value.tag          = RECOVER_FF_QUANTITY;
    original.value.block        = FF_MakeChoiceBlock(RECOVER_FF_QUANTITY);
    original.value.block->value = std::move(quantity);

    ObservationData copy = original;
    REQUIRE(copy.value.block != nullptr, "the copy has a block");
    CHECK(copy.value.block.get() != original.value.block.get(),
          "and it is its own, not the original's");
    CHECK(copy.value.tag == RECOVER_FF_QUANTITY, "the variant tag came with it");

    // Editing one must not be visible in the other.
    std::get<QuantityData>(copy.value.block->value).value = 188.0;
    CHECK(std::get<QuantityData>(original.value.block->value).value == 94.0,
          "the original still reads 94");

    const std::string json = sealed_json(copy);
    CHECK(json.find("\"valueQuantity\"") != std::string::npos, "the copy stores as a Quantity: " << json);
    CHECK(json.find("188") != std::string::npos, "with the copy's own value");
}

void assigning_a_datatype_infers_the_variant_tag()
{
    TEST_GROUP("assignment");

    ObservationData observation;
    observation.id     = "poco-assign";
    observation.status = FF_ObservationStatus::Final;

    // One spelling of the type, not three.
    observation.value = QuantityData{.value = 94.0, .unit = "mg/dL"};

    CHECK(observation.value.tag == RECOVER_FF_QUANTITY,
          "the tag came from TypeTraits, not from the caller: " << observation.value.tag);
    REQUIRE(observation.value.block != nullptr, "the block was built");
    CHECK(std::holds_alternative<QuantityData>(observation.value.block->value),
          "and it holds the value that was assigned");

    const std::string json = sealed_json(observation);
    CHECK(json.find("\"valueQuantity\"") != std::string::npos,
          "it stores under the variant's own name: " << json);
    CHECK(json.find("\"unit\":\"mg/dL\"") != std::string::npos, "with its fields intact");

    // Re-assigning a different datatype replaces the variant wholesale.
    observation.value = CodeableConceptData{.text = "not detected"};
    // TypeTraits, not a hand-written tag: RECOVER_FF_CODEABLECONCEPT (the FHIR
    // datatype) and RECOVER_FF_CODED_VALUE (the coded-value fallback
    // block, 0x0009) differ by one underscore, and this assertion picked the
    // wrong one first time. Inferring the tag is the point of the feature.
    CHECK(observation.value.tag == TypeTraits<CodeableConceptData>::recovery,
          "a second assignment re-tags the slot: " << observation.value.tag);
    CHECK(std::holds_alternative<CodeableConceptData>(observation.value.block->value),
          "and replaces the block");

    // Copy assignment between two ChoiceEntries still means copy, not hijack by
    // the template: this is the overload-resolution trap the constraint blocks.
    ChoiceEntry other;
    other = observation.value;
    CHECK(other.tag == observation.value.tag, "ChoiceEntry-to-ChoiceEntry assignment copies");
    CHECK(other.block.get() != observation.value.block.get(), "deeply");
}

/// The case this member type exists for: a producer computes a string, the
/// source dies at the end of its scope, and the append happens afterwards.
/// With a bare std::string_view this reads freed stack memory -- it is the
/// shape the ASan run reproduced before the type changed.
void a_runtime_string_outlives_the_scope_that_built_it()
{
    TEST_GROUP("strings");

    ObservationData observation;
    observation.id     = "poco-runtime-string";
    observation.status = FF_ObservationStatus::Final;

    {
        // Built at runtime so no literal is involved and the compiler cannot
        // fold it into static storage.
        std::string lot = std::string("LOT-") + std::to_string(77) + "-Q2-REAGENT";
        observation.code = CodeableConceptData{.text = lot};
        CHECK(observation.code->text.owns(), "a std::string source is copied, not borrowed");
    }
    // `lot` is gone. The field is not.
    REQUIRE(observation.code != nullptr, "the concept survived");
    CHECK(observation.code->text == "LOT-77-Q2-REAGENT", "and so did its text");

    const std::string json = sealed_json(observation);
    CHECK(json.find("\"text\":\"LOT-77-Q2-REAGENT\"") != std::string::npos,
          "the runtime string reached the wire intact: " << json);
}

/// The other half of the contract: a literal and an explicit view are BORROWED.
/// If either copied, materializing a POCO would allocate per string and the
/// zero-copy read path would be gone.
void a_literal_and_a_view_are_borrowed_not_copied()
{
    TEST_GROUP("borrowing");

    CodingData coding{.system = "http://unitsofmeasure.org", .code = "mg/dL"};
    CHECK(!coding.system.owns(), "a literal is borrowed");
    CHECK(!coding.code.owns(), "including in a brace list");

    // An explicit view is the caller vouching for the lifetime -- this is the
    // shape the read path and the ingestor's JSON buffer both use.
    static const std::string arena_bytes = "bytes-that-live-in-the-arena";
    CodingData borrowed;
    borrowed.display = std::string_view(arena_bytes);
    CHECK(!borrowed.display.owns(), "an explicit view is borrowed");
    CHECK(borrowed.display == "bytes-that-live-in-the-arena", "and reads back");

    // Copying a borrowed field stays borrowed, so copying a materialized POCO
    // allocates nothing.
    CodingData copy = borrowed;
    CHECK(!copy.display.owns(), "a copy of a borrowed field is still borrowed");

    // Round-trip through the reader: what comes back off the wire must borrow.
    ObservationData observation;
    observation.id     = "poco-borrowed";
    observation.status = FF_ObservationStatus::Final;
    observation.code   = CodeableConceptData{.text = "borrowed on read"};
    const std::string json = sealed_json(observation);
    CHECK(json.find("\"text\":\"borrowed on read\"") != std::string::npos,
          "the borrowed literal reached the wire: " << json);
}

} // namespace

int main(int argc, char **argv)
{
    ff_test::set_filter(argc, argv);
    ff_test::run("initializer", ryans_initializer_compiles_and_stores);
    ff_test::run("copies", a_copy_is_deep_and_outlives_its_source);
    ff_test::run("absence", an_unset_optional_is_absent);
    ff_test::run("choice", a_copied_choice_does_not_share_its_block);
    ff_test::run("assignment", assigning_a_datatype_infers_the_variant_tag);
    ff_test::run("strings", a_runtime_string_outlives_the_scope_that_built_it);
    ff_test::run("borrowing", a_literal_and_a_view_are_borrowed_not_copied);
    return ff_test::report("POCOs are values: brace-initializable, copyable, and absent when unset");
}
