#include "taskbar_config.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <stdexcept>

namespace taskbar_config {
using Json = nlohmann::json;
std::string XmlEscape(std::string_view text) {
    std::string result;
    for (char c : text) {
        switch (c) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        case '\'':
            result += "&apos;";
            break;
        default:
            result += c;
        }
    }
    return result;
}
std::string XamlPropertyName(std::string_view name) {
    if (name == "spacing")
        return "Margin"; // Schema-1 compatibility.
    std::string result(name);
    if (!result.empty())
        result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
    return result;
}
namespace {
std::string Color(const Json &color) {
    if (color.is_object() && color.contains("resource"))
        return "{ThemeResource " + color.at("resource").get<std::string>() + "}";
    auto value = color.get<std::string>();
    if (value.size() == 9 && value[0] == '#')
        value = "#" + value.substr(7, 2) + value.substr(1, 6);
    return value;
}
std::string Attribute(std::string_view key, const Json &value) {
    const auto text = value.is_string() ? value.get<std::string>() : value.dump();
    return " " + std::string(key) + "=\"" + XmlEscape(text) + "\"";
}
class Expression {
    std::string_view source_;
    const Json &variables_;
    std::size_t position_ = 0;
    int depth_ = 0;
    void Space() {
        while (position_ < source_.size() &&
               std::isspace(static_cast<unsigned char>(source_[position_])))
            ++position_;
    }
    bool Eat(std::string_view token) {
        Space();
        if (source_.substr(position_, token.size()) != token)
            return false;
        position_ += token.size();
        return true;
    }
    double Number(const Json &value) {
        if (!value.is_number() || !std::isfinite(value.get<double>()))
            throw std::runtime_error("numeric operand required");
        return value.get<double>();
    }
    Json Primary(bool evaluate) {
        if (++depth_ > 32)
            throw std::runtime_error("expression nesting exceeds 32");
        Json result;
        if (Eat("(")) {
            result = Conditional(evaluate);
            if (!Eat(")"))
                throw std::runtime_error("missing )");
        } else if (Eat("-")) {
            auto operand = Primary(evaluate);
            if (evaluate)
                result = -Number(operand);
        } else if (Eat("+")) {
            auto operand = Primary(evaluate);
            if (evaluate)
                result = Number(operand);
        } else {
            Space();
            const auto start = position_;
            if (position_ >= source_.size())
                throw std::runtime_error("missing operand");
            if (source_[position_] == '\'' || source_[position_] == '"') {
                const char quote = source_[position_++];
                std::string text;
                while (position_ < source_.size() && source_[position_] != quote)
                    text += source_[position_++];
                if (position_ == source_.size())
                    throw std::runtime_error("unterminated string");
                ++position_;
                result = text;
            } else if (std::isdigit(static_cast<unsigned char>(source_[position_])) ||
                       source_[position_] == '.') {
                while (position_ < source_.size() &&
                       (std::isdigit(static_cast<unsigned char>(source_[position_])) ||
                        source_[position_] == '.'))
                    ++position_;
                const auto literal = std::string(source_.substr(start, position_ - start));
                std::size_t used = 0;
                const double number = std::stod(literal, &used);
                if (used != literal.size() || !std::isfinite(number))
                    throw std::runtime_error("invalid number");
                result = number;
            } else {
                while (position_ < source_.size() &&
                       (std::isalnum(static_cast<unsigned char>(source_[position_])) ||
                        source_[position_] == '_'))
                    ++position_;
                if (start == position_)
                    throw std::runtime_error("invalid token");
                const auto id = std::string(source_.substr(start, position_ - start));
                if (Eat("(")) {
                    if (id == "skip") {
                        if (!Eat(")"))
                            throw std::runtime_error("skip takes no argument");
                        result = Json{{"$skip", true}};
                    } else {
                        const auto a = Conditional(evaluate);
                        if (!Eat(","))
                            throw std::runtime_error("missing comma");
                        const auto b = Conditional(evaluate);
                        if (!Eat(")"))
                            throw std::runtime_error("missing )");
                        if (id != "min" && id != "max")
                            throw std::runtime_error("unknown function");
                        if (evaluate)
                            result = id == "min" ? std::min(Number(a), Number(b))
                                                 : std::max(Number(a), Number(b));
                    }
                } else if (id == "true")
                    result = true;
                else if (id == "false")
                    result = false;
                else
                    result = variables_.value(id, Json(nullptr));
            }
        }
        --depth_;
        return evaluate ? result : Json(nullptr);
    }
    Json Product(bool evaluate) {
        auto a = Primary(evaluate);
        for (;;) {
            const bool multiply = Eat("*");
            if (!multiply && !Eat("/"))
                return a;
            auto b = Primary(evaluate);
            if (evaluate) {
                const double divisor = Number(b);
                if (!multiply && divisor == 0)
                    throw std::runtime_error("division by zero");
                a = multiply ? Number(a) * divisor : Number(a) / divisor;
            }
        }
    }
    Json Sum(bool evaluate) {
        auto a = Product(evaluate);
        for (;;) {
            const bool add = Eat("+");
            if (!add && !Eat("-"))
                return a;
            auto b = Product(evaluate);
            if (evaluate)
                a = add ? Number(a) + Number(b) : Number(a) - Number(b);
        }
    }
    Json Compare(bool evaluate) {
        auto a = Sum(evaluate);
        for (;;) {
            std::string_view op;
            for (const auto candidate : {"==", "!=", "<=", ">=", "<", ">"})
                if (Eat(candidate)) {
                    op = candidate;
                    break;
                }
            if (op.empty())
                return a;
            auto b = Sum(evaluate);
            if (!evaluate)
                continue;
            if (op == "==")
                a = a == b;
            else if (op == "!=")
                a = a != b;
            else if (op == "<=")
                a = Number(a) <= Number(b);
            else if (op == ">=")
                a = Number(a) >= Number(b);
            else if (op == "<")
                a = Number(a) < Number(b);
            else
                a = Number(a) > Number(b);
        }
    }
    Json Conditional(bool evaluate) {
        if (++depth_ > 32)
            throw std::runtime_error("expression nesting exceeds 32");
        auto condition = Compare(evaluate);
        if (!Eat("?")) {
            --depth_;
            return condition;
        }
        const bool selected =
            evaluate && (condition.is_boolean() ? condition.get<bool>() : Number(condition) != 0);
        auto yes = Conditional(evaluate && selected);
        if (!Eat(":"))
            throw std::runtime_error("missing :");
        auto no = Conditional(evaluate && !selected);
        --depth_;
        return selected ? yes : no;
    }

  public:
    Expression(std::string_view source, const Json &variables)
        : source_(source), variables_(variables) {
    }
    Json Run() {
        auto value = Conditional(true);
        Space();
        if (position_ != source_.size())
            throw std::runtime_error("trailing expression input");
        if (value.is_number() && !std::isfinite(value.get<double>()))
            throw std::runtime_error("non-finite expression result");
        return value;
    }
};
} // namespace
Json EvaluateExpression(std::string_view expression, const Json &variables) {
    if (expression.size() > 4096)
        throw std::runtime_error("expression too long");
    return Expression(expression, variables).Run();
}
std::string XamlValueMarkup(const Json &value, bool color_value) {
    if (value.is_null())
        return "{x:Null}";
    if (value.is_string()) {
        auto text = value.get<std::string>();
        if (color_value && text.size() == 9 && text[0] == '#')
            return Color(value);
        if (text == "auto")
            return "Auto";
        if (text == "transparent")
            return "Transparent";
        return text;
    }
    if (value.is_array()) {
        std::string result;
        for (const auto &component : value) {
            if (!result.empty())
                result += ',';
            result += XamlValueMarkup(component);
        }
        return result;
    }
    if (value.is_number() || value.is_boolean())
        return value.dump();
    if (value.contains("xaml"))
        return value.at("xaml").get<std::string>();
    if (value.contains("resource"))
        return "{ThemeResource " + value.at("resource").get<std::string>() + "}";
    const auto kind = value.at("kind").get<std::string>();
    if (kind == "solid")
        return "<SolidColorBrush Color=\"" + XmlEscape(Color(value.at("color"))) + "\"/>";
    if (kind == "image")
        return "<ImageBrush ImageSource=\"" + XmlEscape(value.at("source").get<std::string>()) +
               "\" Stretch=\"" + XmlEscape(value.value("stretch", "UniformToFill")) + "\"/>";
    if (kind == "gradient") {
        std::string result =
            "<LinearGradientBrush" +
            Attribute("StartPoint", XamlValueMarkup(value.value("start", Json::array({0, 0})))) +
            Attribute("EndPoint", XamlValueMarkup(value.value("end", Json::array({1, 1})))) + ">";
        for (const auto &stop : value.at("stops"))
            result += "<GradientStop" + Attribute("Offset", stop.at("offset")) +
                      Attribute("Color", Color(stop.at("color"))) + "/>";
        return result + "</LinearGradientBrush>";
    }
    if (kind == "acrylic")
        return "<AcrylicBrush" + Attribute("TintColor", Color(value.at("color"))) +
               Attribute("TintOpacity", value.value("tintOpacity", 0.5)) +
               Attribute("FallbackColor", Color(value.value("fallbackColor", Json("#202020FF")))) +
               "/>";
    if (kind == "transform" || kind == "transform3d" || kind == "projection" ||
        kind == "perspective") {
        const auto tag = kind == "transform"     ? "CompositeTransform"
                         : kind == "transform3d" ? "CompositeTransform3D"
                         : kind == "projection"  ? "PlaneProjection"
                                                 : "PerspectiveTransform3D";
        std::string result = "<" + std::string(tag);
        for (auto it = value.begin(); it != value.end(); ++it)
            if (it.key() != "kind")
                result += Attribute(XamlPropertyName(it.key()), it.value());
        return result + "/>";
    }
    if (kind == "columns" || kind == "rows") {
        const auto tag = kind == "columns" ? "ColumnDefinition" : "RowDefinition";
        const auto dimension = kind == "columns" ? "Width" : "Height";
        std::string result = "<" + std::string(tag) + "Collection>";
        for (const auto &size : value.at("sizes"))
            result += "<" + std::string(tag) + Attribute(dimension, size) + "/>";
        return result + "</" + tag + "Collection>";
    }
    if (kind == "transition")
        return "<BrushTransition" +
               Attribute("Duration",
                         "0:0:" + std::to_string(value.value("durationMs", 180) / 1000.0)) +
               "/>";
    throw std::runtime_error("material kind requires native resolver: " + kind);
}
} // namespace taskbar_config
