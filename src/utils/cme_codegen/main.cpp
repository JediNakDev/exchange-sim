// Generates fixed-block C++23 field descriptors and null defaults from the
// pinned CME SBE XML. No network, matching, or protocol behavior lives here.
#include <algorithm>
#include <cstdint>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

struct Node {
    std::string tag, text;
    std::map<std::string, std::string> attributes;
    std::vector<Node> children;
    std::string attr(const std::string& key, std::string fallback = {}) const {
        const auto it = attributes.find(key); return it == attributes.end() ? fallback : it->second;
    }
};
class Xml {
    std::string text_; std::size_t pos_ = 0;
    void space() { while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_; }
    std::string name() {
        const auto begin = pos_;
        while (pos_ < text_.size() && (std::isalnum(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == ':' || text_[pos_] == '_' || text_[pos_] == '-')) ++pos_;
        if (begin == pos_) throw std::runtime_error("XML name");
        return text_.substr(begin, pos_ - begin);
    }
    void expect(char value) { if (pos_ == text_.size() || text_[pos_++] != value) throw std::runtime_error("XML syntax"); }
    bool skip_special() {
        const bool comment = text_.compare(pos_, 4, "<!--") == 0;
        if (!comment && text_.compare(pos_, 2, "<?") != 0) return false;
        const std::string end = comment ? "-->" : "?>";
        const auto found = text_.find(end, pos_);
        if (found == std::string::npos) throw std::runtime_error("XML declaration");
        pos_ = found + end.size(); return true;
    }
    Node node() {
        expect('<'); Node result; result.tag = name();
        for (;;) {
            space();
            if (text_.compare(pos_, 2, "/>") == 0) { pos_ += 2; return result; }
            if (pos_ < text_.size() && text_[pos_] == '>') { ++pos_; break; }
            auto key = name(); space(); expect('='); space();
            if (pos_ == text_.size()) throw std::runtime_error("XML attribute");
            const char quote = text_[pos_++];
            if (quote != '\'' && quote != '"') throw std::runtime_error("XML quote");
            const auto end = text_.find(quote, pos_);
            if (end == std::string::npos) throw std::runtime_error("XML attribute end");
            result.attributes.emplace(std::move(key), text_.substr(pos_, end - pos_)); pos_ = end + 1;
        }
        for (;;) {
            if (pos_ >= text_.size()) throw std::runtime_error("XML unclosed element");
            if (skip_special()) continue;
            if (text_.compare(pos_, 2, "</") == 0) {
                pos_ += 2; if (name() != result.tag) throw std::runtime_error("XML closing element");
                space(); expect('>'); return result;
            }
            if (text_[pos_] == '<') result.children.push_back(node());
            else result.text += text_[pos_++];
        }
    }
public:
    explicit Xml(std::string text) : text_(std::move(text)) {}
    Node parse() { space(); while (skip_special()) space(); return node(); }
};

struct Type {
    std::string cpp;
    bool text = false;
    std::vector<unsigned char> defaults;
};
class Generator {
    const Node& root_;
    std::map<std::string, Node> types_;
    Type type(const std::string& name) { return resolve(types_.at(name)); }
    Type resolve(const Node& node) {
        if (node.attr("presence") == "constant") return {};
        if (node.tag == "enum" || node.tag == "set") return type(node.attr("encodingType"));
        if (node.tag == "composite") {
            Type result; std::size_t parts = 0;
            for (const auto& child : node.children) {
                Type part = resolve(child);
                if (part.defaults.empty()) continue;
                ++parts; result.cpp = part.cpp;
                const auto offset = std::stoul(child.attr("offset", std::to_string(result.defaults.size())));
                if (result.defaults.size() < offset + part.defaults.size()) result.defaults.resize(offset + part.defaults.size());
                std::copy(part.defaults.begin(), part.defaults.end(), result.defaults.begin() + offset);
            }
            if (parts != 1) result.cpp.clear();
            return result;
        }
        const auto primitive = node.attr("primitiveType");
        const std::map<std::string, std::pair<std::string, std::size_t>> primitives{
            {"char", {"char", 1}}, {"uint8", {"std::uint8_t", 1}}, {"int8", {"std::int8_t", 1}},
            {"uint16", {"std::uint16_t", 2}}, {"int16", {"std::int16_t", 2}},
            {"uint32", {"std::uint32_t", 4}}, {"int32", {"std::int32_t", 4}},
            {"uint64", {"std::uint64_t", 8}}, {"int64", {"std::int64_t", 8}}};
        const auto [cpp, width] = primitives.at(primitive);
        const auto length = std::stoul(node.attr("length", "1"));
        Type result{cpp, primitive == "char" && length > 1, std::vector<unsigned char>(width * length)};
        if (length > 1) result.cpp.clear();
        if (node.attr("presence") == "optional") {
            const auto null = node.attr("nullValue");
            std::uint64_t bits = 0;
            if (!null.empty()) bits = null.front() == '-' ? static_cast<std::uint64_t>(std::stoll(null)) : std::stoull(null);
            else if (primitive != "char") bits = primitive.front() == 'u' ? UINT64_MAX :
                width == 8 ? INT64_MAX : (std::uint64_t{1} << (width * 8 - 1)) - 1;
            for (std::size_t i = 0; i < result.defaults.size(); ++i) result.defaults[i] = static_cast<unsigned char>(bits >> (8 * (i % width)));
        }
        return result;
    }
    void bytes(const std::string& name, const std::vector<unsigned char>& data) {
        std::cout << "    inline static constexpr std::array<char, " << data.size() << "> " << name << "{";
        for (std::size_t i = 0; i < data.size(); ++i) {
            if (i % 16 == 0) std::cout << "\n        ";
            else std::cout << ' ';
            std::cout << "char(" << unsigned(data[i]) << ")" << (i + 1 == data.size() ? "" : ",");
        }
        std::cout << "};\n";
    }
    void block(const Node& node, bool message) {
        const auto size = std::stoul(node.attr("blockLength"));
        std::cout << "struct " << node.attr("name") << " {\n    static constexpr std::uint16_t block_length = " << size << ";\n";
        if (message) std::cout << "    static constexpr std::uint16_t id = " << node.attr("id") << ";\n";
        std::vector<unsigned char> defaults(size), tail;
        for (const auto& field : node.children) {
            if (field.tag == "group") {
                block(field, false);
                auto dimension = type(field.attr("dimensionType")).defaults;
                const auto entry_size = std::stoul(field.attr("blockLength"));
                dimension[0] = entry_size & 255; dimension[1] = (entry_size >> 8) & 255;
                tail.insert(tail.end(), dimension.begin(), dimension.end()); continue;
            }
            if (field.tag == "data") { tail.push_back(0); tail.push_back(0); continue; }
            if (field.tag != "field") continue;
            const Type value = type(field.attr("type"));
            if (value.defaults.empty()) continue;
            const auto offset = std::stoul(field.attr("offset"));
            if (offset + value.defaults.size() > defaults.size()) throw std::runtime_error("field outside block");
            std::copy(value.defaults.begin(), value.defaults.end(), defaults.begin() + offset);
            const auto descriptor = value.text ? "TextField<" + std::to_string(value.defaults.size()) + ">" :
                !value.cpp.empty() ? "Field<" + value.cpp + ">" : "BytesField<" + std::to_string(value.defaults.size()) + ">";
            std::cout << "    inline static constexpr " << descriptor << ' ' << field.attr("name") << '{' << offset << "};\n";
        }
        bytes("defaults", defaults);
        if (message) bytes("empty_tail", tail);
        std::cout << "};\n";
    }
public:
    explicit Generator(const Node& root) : root_(root) {
        const auto& types = *std::find_if(root.children.begin(), root.children.end(), [](const auto& node) { return node.tag == "types"; });
        for (const auto& node : types.children) types_.emplace(node.attr("name"), node);
    }
    void run() {
        if (root_.attr("id") != "8" || root_.attr("version") != "9") throw std::runtime_error("expected iLink schema 8 version 9");
        std::cout << "// Generated by src/utils/cme_codegen/main.cpp. Do not edit.\n#pragma once\n#include <array>\n#include <cstdint>\n#include <cstddef>\nnamespace exsim::protocol::ilink3 {\n"
            "template<class T> struct Field { std::size_t offset; };\n"
            "template<std::size_t N> struct TextField { std::size_t offset; };\n"
            "template<std::size_t N> struct BytesField { std::size_t offset; };\nnamespace schema {\n"
            "inline constexpr std::uint16_t id = 8, version = 9;\n";
        for (const auto& node : root_.children) if (node.tag.ends_with(":message") || node.tag == "message") block(node, true);
        std::cout << "} }\n";
    }
};
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: cme_codegen ilinkbinary.xml");
        std::ifstream file(argv[1]); if (!file) throw std::runtime_error("cannot read schema");
        std::string text((std::istreambuf_iterator<char>(file)), {});
        const auto root = Xml(std::move(text)).parse(); Generator(root).run();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
