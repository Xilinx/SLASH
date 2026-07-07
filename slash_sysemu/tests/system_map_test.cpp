// ################################################################################################
//  The MIT License (MIT)
//  Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
//  and associated documentation files (the "Software"), to deal in the Software without
//  restriction, including without limitation the rights to use, copy, modify, merge, publish,
//  distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all copies or
//  substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// ################################################################################################

#include <gtest/gtest.h>

#include "system_map.h"

#include <string>

#include "fixtures_paths.h"

namespace slash_sysemu {
namespace {

using namespace slash_sysemu::test_fixtures;

std::string map_path(const char* name) {
    return std::string(kMapDir) + "/" + name;
}

// ── Well-formed documents ────────────────────────────────────────────────────

TEST(SystemMapTest, ParsesInlineEmulationBuffer) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap>
  <Platform>Emulation</Platform>
  <ClockFrequency>100000000</ClockFrequency>
  <Kernel>
    <Name>vadd</Name>
    <BaseAddress>0x10000</BaseAddress>
    <Range>0x1000</Range>
    <register offset="0x00" name="CTRL" access="RW" range="32"/>
    <register offset="0x28" name="size" access="W" range="32"/>
    <functional_args>
      <arg idx="3" name="size" type="scalar" offset="0x28" range="32" r="0" w="1"/>
      <arg idx="0" name="in1_r" type="buffer" offset="0x10" range="64" r="0" w="1" port="m_axi_gmem0"/>
    </functional_args>
    <connection port="m_axi_gmem0" target="HBM0"/>
  </Kernel>
</SystemMap>)";

    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_TRUE(res) << (res ? "" : res.error().message);
    const SystemMap& m = res.value();

    EXPECT_EQ(m.platform, Platform::Emulation);
    EXPECT_EQ(m.clock_frequency_hz, 100000000u);
    ASSERT_EQ(m.kernels.size(), 1u);

    const Kernel& k = m.kernels[0];
    EXPECT_EQ(k.name, "vadd");
    EXPECT_EQ(k.base_address, 0x10000u);
    EXPECT_EQ(k.range, 0x1000u);

    // Control register at offset 0 must be discoverable (Step 8 ap_start/ap_done).
    const Register* ctrl = k.register_at(0);
    ASSERT_NE(ctrl, nullptr);
    EXPECT_EQ(ctrl->name, "CTRL");
    EXPECT_EQ(ctrl->bit_width, 32u);

    // Args sorted by idx.
    ASSERT_EQ(k.args.size(), 2u);
    EXPECT_EQ(k.args[0].idx, 0u);
    EXPECT_EQ(k.args[0].name, "in1_r");
    EXPECT_EQ(k.args[0].type, "buffer");
    EXPECT_EQ(k.args[0].offset, 0x10u);
    EXPECT_EQ(k.args[0].bit_width, 64u);
    EXPECT_TRUE(k.args[0].writable);
    EXPECT_FALSE(k.args[0].readable);
    EXPECT_EQ(k.args[0].port, "m_axi_gmem0");
    EXPECT_EQ(k.args[1].idx, 3u);
    EXPECT_EQ(k.args[1].name, "size");

    // Lookups.
    EXPECT_EQ(k.find_register("size")->offset, 0x28u);
    EXPECT_EQ(k.find_arg("in1_r")->idx, 0u);
    EXPECT_EQ(k.find_connection("m_axi_gmem0")->target, "HBM0");
    EXPECT_EQ(k.find_connection("nope"), nullptr);
    EXPECT_EQ(m.find_kernel("vadd")->name, "vadd");
    EXPECT_EQ(m.find_kernel("missing"), nullptr);
}

TEST(SystemMapTest, ParsesQdmaAndHardwareClassification) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap>
  <Platform>Hardware</Platform>
  <ClockFrequency>50</ClockFrequency>
  <Qdma><kernel>vadd</kernel><interface>axis_in</interface>
        <direction>HostToDevice</direction><qid>7</qid></Qdma>
</SystemMap>)";
    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_TRUE(res) << res.error().message;
    EXPECT_EQ(res.value().platform, Platform::Hardware);
    ASSERT_EQ(res.value().qdma.size(), 1u);
    EXPECT_EQ(res.value().qdma[0].kernel, "vadd");
    EXPECT_EQ(res.value().qdma[0].direction, "HostToDevice");
    EXPECT_EQ(res.value().qdma[0].qid, 7u);
}

TEST(SystemMapTest, PlatformNames) {
    EXPECT_STREQ(platform_name(Platform::Hardware), "Hardware");
    EXPECT_STREQ(platform_name(Platform::Emulation), "Emulation");
    EXPECT_STREQ(platform_name(Platform::Simulation), "Simulation");
    EXPECT_STREQ(platform_name(Platform::Unknown), "Unknown");
}

TEST(SystemMapTest, RegisterDefaultsBitWidthTo32) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name>k</Name><BaseAddress>0</BaseAddress><Range>0</Range>
<register offset="0x4" name="R"/></Kernel></SystemMap>)";
    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_TRUE(res) << res.error().message;
    EXPECT_EQ(res.value().kernels[0].registers[0].bit_width, 32u);
}

// ── Broken documents (one distinct test each) ────────────────────────────────

TEST(SystemMapTest, RejectsNotXml) {
    auto res = parse_system_map_file(map_path("not_xml.xml"));
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().kind, VbinErrorKind::Parse);
}

TEST(SystemMapTest, RejectsWrongRoot) {
    auto res = parse_system_map_file(map_path("wrong_root.xml"));
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().kind, VbinErrorKind::Parse);
    EXPECT_NE(res.error().message.find("SystemMap"), std::string::npos);
}

TEST(SystemMapTest, RejectsUnknownPlatform) {
    auto res = parse_system_map_file(map_path("unknown_platform.xml"));
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().kind, VbinErrorKind::Parse);
    EXPECT_NE(res.error().message.find("Platform"), std::string::npos);
}

TEST(SystemMapTest, RejectsMissingPlatform) {
    auto res = parse_system_map_file(map_path("missing_platform.xml"));
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().message.find("Platform"), std::string::npos);
}

TEST(SystemMapTest, RejectsMissingKernelName) {
    auto res = parse_system_map_file(map_path("missing_name.xml"));
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().message.find("Name"), std::string::npos);
}

TEST(SystemMapTest, RejectsMissingBaseAddress) {
    auto res = parse_system_map_file(map_path("missing_base.xml"));
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().message.find("BaseAddress"), std::string::npos);
}

TEST(SystemMapTest, RejectsMalformedHexOffset) {
    auto res = parse_system_map_file(map_path("bad_offset.xml"));
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().message.find("offset"), std::string::npos);
}

TEST(SystemMapTest, RejectsMalformedDecimalRange) {
    auto res = parse_system_map_file(map_path("bad_range.xml"));
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().message.find("range"), std::string::npos);
}

TEST(SystemMapTest, RejectsMissingFile) {
    auto res = parse_system_map_file(map_path("does_not_exist.xml"));
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().kind, VbinErrorKind::Parse);
}

// ── Additional coverage for lookups and inline error branches ────────────────

TEST(SystemMapTest, LookupsReturnNullWhenAbsent) {
    Kernel k;
    EXPECT_EQ(k.find_register("x"), nullptr);
    EXPECT_EQ(k.register_at(0x99), nullptr);
    EXPECT_EQ(k.find_arg("x"), nullptr);
    EXPECT_EQ(k.find_connection("x"), nullptr);
    SystemMap m;
    EXPECT_EQ(m.find_kernel("x"), nullptr);
}

TEST(SystemMapTest, RejectsRegisterMissingOffset) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name>k</Name><BaseAddress>0</BaseAddress><Range>0</Range>
<register name="CTRL"/></Kernel></SystemMap>)";
    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().message.find("offset"), std::string::npos);
}

TEST(SystemMapTest, RejectsArgMissingIdx) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name>k</Name><BaseAddress>0</BaseAddress><Range>0</Range>
<functional_args><arg name="a" offset="0x10"/></functional_args></Kernel></SystemMap>)";
    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().message.find("idx"), std::string::npos);
}

TEST(SystemMapTest, RejectsArgMalformedIdx) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name>k</Name><BaseAddress>0</BaseAddress><Range>0</Range>
<functional_args><arg idx="nope" name="a" offset="0x10"/></functional_args></Kernel></SystemMap>)";
    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().message.find("idx"), std::string::npos);
}

TEST(SystemMapTest, RejectsArgMissingOffset) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name>k</Name><BaseAddress>0</BaseAddress><Range>0</Range>
<functional_args><arg idx="0" name="a"/></functional_args></Kernel></SystemMap>)";
    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().message.find("offset"), std::string::npos);
}

TEST(SystemMapTest, RejectsArgMalformedOffsetAndRange) {
    const std::string bad_off = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name>k</Name><BaseAddress>0</BaseAddress><Range>0</Range>
<functional_args><arg idx="0" name="a" offset="0xZ"/></functional_args></Kernel></SystemMap>)";
    auto r1 = parse_system_map_buffer(bad_off, "inline");
    ASSERT_FALSE(r1);
    EXPECT_NE(r1.error().message.find("offset"), std::string::npos);

    const std::string bad_range = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name>k</Name><BaseAddress>0</BaseAddress><Range>0</Range>
<functional_args><arg idx="0" name="a" offset="0x10" range="zz"/></functional_args></Kernel></SystemMap>)";
    auto r2 = parse_system_map_buffer(bad_range, "inline");
    ASSERT_FALSE(r2);
    EXPECT_NE(r2.error().message.find("range"), std::string::npos);
}

TEST(SystemMapTest, ArgDefaultsBitWidthTo32) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name>k</Name><BaseAddress>0</BaseAddress><Range>0</Range>
<functional_args><arg idx="0" name="a" offset="0x10"/></functional_args></Kernel></SystemMap>)";
    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_TRUE(res) << res.error().message;
    EXPECT_EQ(res.value().kernels[0].args[0].bit_width, 32u);
}

TEST(SystemMapTest, RejectsMalformedBaseAddress) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name>k</Name><BaseAddress>0xZZ</BaseAddress><Range>0</Range></Kernel></SystemMap>)";
    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().message.find("BaseAddress"), std::string::npos);
}

TEST(SystemMapTest, RejectsMalformedRange) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name>k</Name><BaseAddress>0</BaseAddress><Range>zz</Range></Kernel></SystemMap>)";
    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().message.find("Range"), std::string::npos);
}

TEST(SystemMapTest, RejectsMissingRange) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name>k</Name><BaseAddress>0</BaseAddress></Kernel></SystemMap>)";
    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().message.find("Range"), std::string::npos);
}

TEST(SystemMapTest, RejectsMalformedClockFrequency) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform><ClockFrequency>fast</ClockFrequency></SystemMap>)";
    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().message.find("ClockFrequency"), std::string::npos);
}

TEST(SystemMapTest, RejectsMalformedQid) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Qdma><kernel>k</kernel><qid>nan</qid></Qdma></SystemMap>)";
    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().message.find("qid"), std::string::npos);
}

TEST(SystemMapTest, RejectsBufferParseFailure) {
    auto res = parse_system_map_buffer("<<<not xml", "inline");
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().kind, VbinErrorKind::Parse);
}

// ── Adversary probes ─────────────────────────────────────────────────────────

// Build a minimal kernel doc with a single register offset attribute.
static std::string kernel_with_offset(const std::string& off) {
    return std::string("<?xml version=\"1.0\"?>\n<SystemMap>"
                       "<Platform>Emulation</Platform>"
                       "<Kernel><Name>k</Name><BaseAddress>0x1000</BaseAddress>"
                       "<Range>0x100</Range>"
                       "<register offset=\"") +
           off + "\" name=\"R\" range=\"32\"/></Kernel></SystemMap>";
}

// PROBE (FIXED): parse_uint no longer uses strtoull base 0, so a leading-zero
// decimal is parsed as DECIMAL, not octal. A register at offset "010" is stored
// as 10 (the value a human reads), preserving correct reverse addressing.
TEST(SystemMapTest, OctalTrap_LeadingZeroOffsetParsedAsDecimal) {
    auto res = parse_system_map_buffer(kernel_with_offset("010"), "inline");
    ASSERT_TRUE(res) << res.error().message;
    const uint64_t stored = res.value().kernels[0].registers[0].offset;
    EXPECT_EQ(stored, 10u) << "leading-zero decimal must parse as decimal, not octal";
}

// PROBE (FIXED): "09" is now a valid decimal offset (9), not a hard parse error.
// Under the old base-0 octal parse, '9' was not an octal digit and the doc was
// rejected; decimal-or-hex parsing accepts it.
TEST(SystemMapTest, OctalTrap_LeadingZeroNineParsedAsDecimal) {
    auto res = parse_system_map_buffer(kernel_with_offset("09"), "inline");
    ASSERT_TRUE(res) << res.error().message;
    EXPECT_EQ(res.value().kernels[0].registers[0].offset, 9u);
}

// PROBE (FIXED): hex offsets still work under decimal-or-hex parsing.
TEST(SystemMapTest, HexOffsetStillParses) {
    auto res = parse_system_map_buffer(kernel_with_offset("0x10"), "inline");
    ASSERT_TRUE(res) << res.error().message;
    EXPECT_EQ(res.value().kernels[0].registers[0].offset, 16u);
}

// PROBE (FIXED): whitespace inside a numeric attribute. node_attr now trims
// surrounding ASCII whitespace (consistent with node_text), so " 0x10 " parses.
TEST(SystemMapTest, WhitespaceInAttributeIsTrimmedAndParsed) {
    auto res = parse_system_map_buffer(kernel_with_offset(" 0x10 "), "inline");
    ASSERT_TRUE(res) << res.error().message;
    EXPECT_EQ(res.value().kernels[0].registers[0].offset, 16u);
}

// PROBE (FIXED): '+' prefix is now rejected for strictness; the schema never
// uses a sign, and '-' was already rejected to avoid wrapping.
TEST(SystemMapTest, PlusPrefixOffsetRejected) {
    auto res = parse_system_map_buffer(kernel_with_offset("+16"), "inline");
    EXPECT_FALSE(res) << "'+16' offset must be rejected (leading sign)";
}

// PROBE (FIXED): duplicate kernels with the same <Name> are now rejected. A name
// collision would silently shadow the second kernel for find_kernel(), breaking
// addressing, so the whole map is rejected as a Parse error.
TEST(SystemMapTest, DuplicateKernelNamesRejected) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name>demo</Name><BaseAddress>0x1000</BaseAddress><Range>0x100</Range></Kernel>
<Kernel><Name>demo</Name><BaseAddress>0x2000</BaseAddress><Range>0x100</Range></Kernel>
</SystemMap>)";
    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().kind, VbinErrorKind::Parse);
    EXPECT_NE(res.error().message.find("duplicate kernel"), std::string::npos);
}

// PROBE (FIXED): duplicate register offsets within one kernel are now rejected.
// register_at() resolves by offset; a collision would make the shadowed register
// unaddressable, so the map is rejected as a Parse error.
TEST(SystemMapTest, DuplicateRegisterOffsetsRejected) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name>k</Name><BaseAddress>0x1000</BaseAddress><Range>0x100</Range>
<register offset="0x0" name="A" range="32"/>
<register offset="0x0" name="B" range="32"/></Kernel></SystemMap>)";
    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().kind, VbinErrorKind::Parse);
    EXPECT_NE(res.error().message.find("duplicate register offset"), std::string::npos);
}

// PROBE (SPEC GAP): a kernel whose registers have NO control register at
// offset 0. Step 8 resolves ap_start/ap_done at offset 0; the parser accepts a
// kernel with only a nonzero-offset register. Documents that the model does not
// enforce the presence of offset-0.
TEST(SystemMapTest, KernelWithoutOffsetZeroRegisterAccepted) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name>k</Name><BaseAddress>0x1000</BaseAddress><Range>0x100</Range>
<register offset="0x10" name="only" range="32"/></Kernel></SystemMap>)";
    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_TRUE(res) << res.error().message;
    EXPECT_EQ(res.value().kernels[0].register_at(0), nullptr);
}

// PROBE (FIXED): a 'range' (bit width) that does not fit in uint32_t is now a
// hard Parse error naming the field, rather than a silent narrowing truncation
// (0x100000001 used to store as 1).
TEST(SystemMapTest, HugeRangeRejectedNotTruncated) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name>k</Name><BaseAddress>0x1000</BaseAddress><Range>0x100</Range>
<register offset="0x0" name="R" range="0x100000001"/></Kernel></SystemMap>)";
    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().kind, VbinErrorKind::Parse);
    EXPECT_NE(res.error().message.find("range"), std::string::npos);
}

// PROBE: bit_width of 0 is accepted (range="0"). Downstream mask computation may
// divide/shift by width; document that zero is not rejected.
TEST(SystemMapTest, ZeroBitWidthAccepted) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name>k</Name><BaseAddress>0x1000</BaseAddress><Range>0x100</Range>
<register offset="0x0" name="R" range="0"/></Kernel></SystemMap>)";
    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_TRUE(res) << res.error().message;
    EXPECT_EQ(res.value().kernels[0].registers[0].bit_width, 0u);
}

// PROBE: empty <Name></Name> is rejected (have_name true but name empty).
TEST(SystemMapTest, EmptyKernelNameRejected) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name></Name><BaseAddress>0x1000</BaseAddress><Range>0x100</Range></Kernel></SystemMap>)";
    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().message.find("Name"), std::string::npos);
}

// PROBE (SECURITY): XXE — external entity referencing /etc/passwd. With
// XML_PARSE_NONET but WITHOUT XML_PARSE_NOENT, libxml2 must NOT expand external
// entities. This asserts the file contents do not leak into a parsed field and
// parsing does not hang. Any read of a local file would be an XXE vulnerability.
TEST(SystemMapTest, XxeExternalEntityNotExpanded) {
    const std::string xml =
        "<?xml version=\"1.0\"?>\n"
        "<!DOCTYPE SystemMap [ <!ENTITY xxe SYSTEM \"file:///etc/hostname\"> ]>\n"
        "<SystemMap><Platform>Emulation</Platform>"
        "<Kernel><Name>&xxe;</Name><BaseAddress>0x1000</BaseAddress>"
        "<Range>0x100</Range></Kernel></SystemMap>";
    auto res = parse_system_map_buffer(xml, "inline");
    // Either the entity is left unexpanded (empty/entity-ref name -> rejected as
    // empty), or parsing fails. It must NOT contain /etc/hostname contents.
    if (res) {
        EXPECT_TRUE(res.value().kernels.empty() ||
                    res.value().kernels[0].name.find('.') == std::string::npos)
            << "external entity appears expanded: " << res.value().kernels[0].name;
    }
    SUCCEED();
}

// PROBE (SECURITY/DoS): billion-laughs entity expansion. Without a bomb guard
// this can blow up memory/CPU. libxml2 has built-in limits; assert we terminate
// (and preferably reject) rather than hang or OOM.
TEST(SystemMapTest, BillionLaughsDoesNotHang) {
    const std::string xml =
        "<?xml version=\"1.0\"?>\n"
        "<!DOCTYPE SystemMap [\n"
        "  <!ENTITY a \"aaaaaaaaaa\">\n"
        "  <!ENTITY b \"&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;\">\n"
        "  <!ENTITY c \"&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;\">\n"
        "  <!ENTITY d \"&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;\">\n"
        "  <!ENTITY e \"&d;&d;&d;&d;&d;&d;&d;&d;&d;&d;\">\n"
        "  <!ENTITY f \"&e;&e;&e;&e;&e;&e;&e;&e;&e;&e;\">\n"
        "]>\n"
        "<SystemMap><Platform>Emulation</Platform>"
        "<Kernel><Name>&f;</Name><BaseAddress>0x1000</BaseAddress>"
        "<Range>0x100</Range></Kernel></SystemMap>";
    auto res = parse_system_map_buffer(xml, "inline");
    // Must return (not hang). libxml2 should refuse the expansion.
    SUCCEED() << "parser returned: " << (res ? "ok" : res.error().message);
}

// PROBE: empty document.
TEST(SystemMapTest, EmptyDocumentRejected) {
    auto res = parse_system_map_buffer("", "inline");
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().kind, VbinErrorKind::Parse);
}

// PROBE: deeply nested XML must not crash the parser (stack safety).
TEST(SystemMapTest, DeeplyNestedXmlDoesNotCrash) {
    std::string xml = "<?xml version=\"1.0\"?><SystemMap><Platform>Emulation</Platform>";
    const int depth = 5000;
    for (int i = 0; i < depth; ++i) xml += "<x>";
    for (int i = 0; i < depth; ++i) xml += "</x>";
    xml += "</SystemMap>";
    auto res = parse_system_map_buffer(xml, "inline");
    // We only require that it returns; the nested <x> are ignored metadata.
    SUCCEED() << (res ? "ok" : res.error().message);
}

// PROBE (FIXED): "<BaseAddress>010</BaseAddress>" now parses as decimal 10, not
// octal 8 — decimal-or-hex parsing applies to element text as well as attributes.
TEST(SystemMapTest, OctalTrap_BaseAddressParsedAsDecimal) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name>k</Name><BaseAddress>010</BaseAddress><Range>0x100</Range></Kernel></SystemMap>)";
    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_TRUE(res) << res.error().message;
    EXPECT_EQ(res.value().kernels[0].base_address, 10u) << "010 base address must be decimal 10";
}

// PROBE (iter-2, parse_uint boundaries): the new base-10/base-16 selector keys
// off a "0x"/"0X" prefix that must have at least one following digit. Verify the
// tricky corners don't regress.
TEST(SystemMapTest, ParseUintPrefixCorners) {
    // "0x" with nothing after: size==2, so base stays 10; strtoull parses '0' and
    // stops at 'x' -> trailing char -> rejected.
    EXPECT_FALSE(parse_system_map_buffer(kernel_with_offset("0x"), "inline"));
    // "0X" uppercase with a digit -> base 16.
    {
        auto res = parse_system_map_buffer(kernel_with_offset("0X1F"), "inline");
        ASSERT_TRUE(res) << res.error().message;
        EXPECT_EQ(res.value().kernels[0].registers[0].offset, 31u);
    }
    // bare "0" -> decimal 0.
    {
        auto res = parse_system_map_buffer(kernel_with_offset("0"), "inline");
        ASSERT_TRUE(res) << res.error().message;
        EXPECT_EQ(res.value().kernels[0].registers[0].offset, 0u);
    }
    // "0x0" -> hex 0.
    {
        auto res = parse_system_map_buffer(kernel_with_offset("0x0"), "inline");
        ASSERT_TRUE(res) << res.error().message;
        EXPECT_EQ(res.value().kernels[0].registers[0].offset, 0u);
    }
    // A hex digit without prefix must NOT be accepted as hex ("1F" is not decimal).
    EXPECT_FALSE(parse_system_map_buffer(kernel_with_offset("1F"), "inline"));
    // "0xG" -> base 16 but 'G' is not a hex digit -> rejected.
    EXPECT_FALSE(parse_system_map_buffer(kernel_with_offset("0xG"), "inline"));
}

// PROBE (iter-2): a full-width 64-bit offset must round-trip (base address +
// offset addressing needs the whole 64-bit range). 0xFFFFFFFFFFFFFFFF is valid.
TEST(SystemMapTest, ParseUintFullWidthOffset) {
    auto res = parse_system_map_buffer(kernel_with_offset("0xFFFFFFFFFFFFFFFF"), "inline");
    ASSERT_TRUE(res) << res.error().message;
    EXPECT_EQ(res.value().kernels[0].registers[0].offset, 0xFFFFFFFFFFFFFFFFull);
}

// PROBE (iter-2): an offset that overflows uint64 (strtoull sets ERANGE) must be
// rejected, not wrapped.
TEST(SystemMapTest, ParseUintOverflowRejected) {
    auto res = parse_system_map_buffer(kernel_with_offset("0x1FFFFFFFFFFFFFFFF"), "inline");
    EXPECT_FALSE(res) << "65-bit offset must be rejected via ERANGE";
}

// PROBE (iter-2): range at exactly the uint32 boundary. 0xFFFFFFFF is the max
// allowed bit width (must pass); 0x100000000 is the first over the line (already
// covered by HugeRangeRejectedNotTruncated, this pins the boundary).
TEST(SystemMapTest, ParseUint32Boundary) {
    const std::string ok = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name>k</Name><BaseAddress>0</BaseAddress><Range>0</Range>
<register offset="0x0" name="R" range="0xFFFFFFFF"/></Kernel></SystemMap>)";
    auto r_ok = parse_system_map_buffer(ok, "inline");
    ASSERT_TRUE(r_ok) << r_ok.error().message;
    EXPECT_EQ(r_ok.value().kernels[0].registers[0].bit_width, 0xFFFFFFFFu);

    const std::string over = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name>k</Name><BaseAddress>0</BaseAddress><Range>0</Range>
<register offset="0x0" name="R" range="0x100000000"/></Kernel></SystemMap>)";
    EXPECT_FALSE(parse_system_map_buffer(over, "inline"));
}

// PROBE (iter-2, regression): the duplicate-offset rejection is PER KERNEL. Two
// different kernels may legitimately reuse the same register offsets/names (the
// real VBINs repeat CTRL/GIER at offset 0/0x4 in every kernel). This must NOT be
// rejected.
TEST(SystemMapTest, SameOffsetsAcrossDifferentKernelsAllowed) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name>k0</Name><BaseAddress>0x1000</BaseAddress><Range>0x100</Range>
<register offset="0x0" name="CTRL" range="32"/>
<register offset="0x4" name="GIER" range="32"/></Kernel>
<Kernel><Name>k1</Name><BaseAddress>0x2000</BaseAddress><Range>0x100</Range>
<register offset="0x0" name="CTRL" range="32"/>
<register offset="0x4" name="GIER" range="32"/></Kernel></SystemMap>)";
    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_TRUE(res) << res.error().message;
    ASSERT_EQ(res.value().kernels.size(), 2u);
    EXPECT_EQ(res.value().kernels[0].register_at(0)->name, "CTRL");
    EXPECT_EQ(res.value().kernels[1].register_at(0)->name, "CTRL");
}

// PROBE (reverse-mapping capability): confirm the model captures enough to
// reverse register -> absolute address (base_address + offset), per
// architecture.md "reverse the register -> address mapping".
TEST(SystemMapTest, ReverseRegisterToAbsoluteAddress) {
    const std::string xml = R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform>
<Kernel><Name>k</Name><BaseAddress>0x20200000000</BaseAddress><Range>0x10000</Range>
<register offset="0x18" name="out_r" range="32"/></Kernel></SystemMap>)";
    auto res = parse_system_map_buffer(xml, "inline");
    ASSERT_TRUE(res) << res.error().message;
    const Kernel& k = res.value().kernels[0];
    const Register* r = k.find_register("out_r");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(k.base_address + r->offset, 0x20200000018ull);
}

} // namespace
} // namespace slash_sysemu
