#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace rc {

namespace {
const Json kNull;
const Json::Array kEmptyArray;
const Json::Object kEmptyObject;
}  // namespace

Json& Json::operator[](const std::string& key) {
  if (type_ == Type::Null) type_ = Type::Object;
  if (type_ != Type::Object) throw std::runtime_error("json: not an object");
  return obj_[key];
}

const Json& Json::get(const std::string& key) const {
  if (type_ != Type::Object) return kNull;
  auto it = obj_.find(key);
  return it == obj_.end() ? kNull : it->second;
}

bool Json::has(const std::string& key) const {
  return type_ == Type::Object && obj_.count(key) > 0;
}

Json::Array& Json::arr() {
  if (type_ == Type::Null) type_ = Type::Array;
  if (type_ != Type::Array) throw std::runtime_error("json: not an array");
  return arr_;
}

const Json::Array& Json::arr() const { return type_ == Type::Array ? arr_ : kEmptyArray; }

Json::Object& Json::obj() {
  if (type_ == Type::Null) type_ = Type::Object;
  if (type_ != Type::Object) throw std::runtime_error("json: not an object");
  return obj_;
}

const Json::Object& Json::obj() const { return type_ == Type::Object ? obj_ : kEmptyObject; }

size_t Json::size() const {
  if (type_ == Type::Array) return arr_.size();
  if (type_ == Type::Object) return obj_.size();
  return 0;
}

std::vector<double> Json::num_vector() const {
  std::vector<double> v;
  for (const auto& x : arr()) v.push_back(x.as_num());
  return v;
}

std::vector<std::string> Json::str_vector() const {
  std::vector<std::string> v;
  for (const auto& x : arr()) v.push_back(x.as_str());
  return v;
}

// ---------------------------------------------------------------- serialize

static void escape_to(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += char(c);
        }
    }
  }
  out += '"';
}

static void number_to(std::string& out, double d) {
  if (!std::isfinite(d)) {
    out += "null";
    return;
  }
  char buf[32];
  if (d == std::floor(d) && std::fabs(d) < 1e15)
    snprintf(buf, sizeof buf, "%.0f", d);
  else
    snprintf(buf, sizeof buf, "%.7g", d);
  out += buf;
}

void Json::dump_to(std::string& out, int indent, int depth) const {
  auto newline = [&](int d) {
    if (indent < 0) return;
    out += '\n';
    out.append(size_t(indent * d), ' ');
  };
  switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += bool_ ? "true" : "false"; break;
    case Type::Number: number_to(out, num_); break;
    case Type::String: escape_to(out, str_); break;
    case Type::Array: {
      out += '[';
      // Numeric arrays (response curves) stay on one line even when pretty
      // printing; one number per line makes the files unreadable.
      bool flat = true;
      for (const auto& v : arr_) flat = flat && (v.is_number() || v.is_null());
      for (size_t i = 0; i < arr_.size(); ++i) {
        if (i) out += indent >= 0 && flat ? ", " : ",";
        if (!flat) newline(depth + 1);
        arr_[i].dump_to(out, indent, depth + 1);
      }
      if (!flat && !arr_.empty()) newline(depth);
      out += ']';
      break;
    }
    case Type::Object: {
      out += '{';
      bool first = true;
      for (const auto& [k, v] : obj_) {
        if (!first) out += ',';
        first = false;
        newline(depth + 1);
        escape_to(out, k);
        out += indent >= 0 ? ": " : ":";
        v.dump_to(out, indent, depth + 1);
      }
      if (!obj_.empty()) newline(depth);
      out += '}';
      break;
    }
  }
}

std::string Json::dump(int indent) const {
  std::string out;
  dump_to(out, indent, 0);
  return out;
}

// -------------------------------------------------------------------- parse

namespace {

struct Parser {
  const std::string& s;
  size_t i = 0;

  [[noreturn]] void fail(const char* what) {
    throw std::runtime_error(std::string("json parse error at ") + std::to_string(i) + ": " + what);
  }

  void ws() {
    while (i < s.size()) {
      char c = s[i];
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
        ++i;
      } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
        // Tolerate // comments so hand-edited config files survive.
        while (i < s.size() && s[i] != '\n') ++i;
      } else {
        break;
      }
    }
  }

  bool lit(const char* word) {
    size_t n = strlen(word);
    if (s.compare(i, n, word) == 0) {
      i += n;
      return true;
    }
    return false;
  }

  static void utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
      out += char(cp);
    } else if (cp < 0x800) {
      out += char(0xC0 | (cp >> 6));
      out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += char(0xE0 | (cp >> 12));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    } else {
      out += char(0xF0 | (cp >> 18));
      out += char(0x80 | ((cp >> 12) & 0x3F));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    }
  }

  unsigned hex4() {
    if (i + 4 > s.size()) fail("short \\u escape");
    unsigned v = unsigned(strtoul(s.substr(i, 4).c_str(), nullptr, 16));
    i += 4;
    return v;
  }

  std::string str() {
    if (s[i] != '"') fail("expected string");
    ++i;
    std::string out;
    while (true) {
      if (i >= s.size()) fail("unterminated string");
      char c = s[i++];
      if (c == '"') break;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i >= s.size()) fail("bad escape");
      char e = s[i++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned cp = hex4();
          if (cp >= 0xD800 && cp < 0xDC00 && lit("\\u")) cp = 0x10000 + ((cp - 0xD800) << 10) + (hex4() - 0xDC00);
          utf8(out, cp);
          break;
        }
        default: fail("bad escape");
      }
    }
    return out;
  }

  Json value() {
    ws();
    if (i >= s.size()) fail("unexpected end");
    char c = s[i];
    if (c == '{') {
      ++i;
      Json o = Json::object();
      ws();
      if (i < s.size() && s[i] == '}') {
        ++i;
        return o;
      }
      while (true) {
        ws();
        std::string k = str();
        ws();
        if (i >= s.size() || s[i] != ':') fail("expected ':'");
        ++i;
        o[k] = value();
        ws();
        if (i < s.size() && s[i] == ',') {
          ++i;
          continue;
        }
        if (i < s.size() && s[i] == '}') {
          ++i;
          return o;
        }
        fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      ++i;
      Json a = Json::array();
      ws();
      if (i < s.size() && s[i] == ']') {
        ++i;
        return a;
      }
      while (true) {
        a.push(value());
        ws();
        if (i < s.size() && s[i] == ',') {
          ++i;
          continue;
        }
        if (i < s.size() && s[i] == ']') {
          ++i;
          return a;
        }
        fail("expected ',' or ']'");
      }
    }
    if (c == '"') return Json(str());
    if (lit("true")) return Json(true);
    if (lit("false")) return Json(false);
    if (lit("null")) return Json();
    const char* start = s.c_str() + i;
    char* end = nullptr;
    double d = strtod(start, &end);
    if (end == start) fail("unexpected character");
    i += size_t(end - start);
    return Json(d);
  }
};

}  // namespace

Json Json::parse(const std::string& text) {
  Parser p{text};
  Json v = p.value();
  p.ws();
  if (p.i != text.size()) p.fail("trailing characters");
  return v;
}

}  // namespace rc
