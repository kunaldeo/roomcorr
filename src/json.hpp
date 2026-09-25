// Minimal JSON value, parser and serializer. Enough for the config file, the
// measurement metadata and the control protocol; not a general-purpose library.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace rc {

class Json {
public:
  enum class Type { Null, Bool, Number, String, Array, Object };
  using Array = std::vector<Json>;
  using Object = std::map<std::string, Json>;

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool b) : type_(Type::Bool), bool_(b) {}
  Json(double d) : type_(Type::Number), num_(d) {}
  Json(float d) : type_(Type::Number), num_(d) {}
  Json(int i) : type_(Type::Number), num_(i) {}
  Json(int64_t i) : type_(Type::Number), num_(double(i)) {}
  Json(uint64_t i) : type_(Type::Number), num_(double(i)) {}
  Json(unsigned i) : type_(Type::Number), num_(i) {}
  Json(const char* s) : type_(Type::String), str_(s) {}
  Json(std::string s) : type_(Type::String), str_(std::move(s)) {}
  Json(Array a) : type_(Type::Array), arr_(std::move(a)) {}
  Json(Object o) : type_(Type::Object), obj_(std::move(o)) {}
  template <typename T>
  Json(const std::vector<T>& v) : type_(Type::Array) {
    for (const auto& x : v) arr_.emplace_back(x);
  }

  static Json array() { return Json(Array{}); }
  static Json object() { return Json(Object{}); }

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }

  bool as_bool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
  double as_num(double def = 0) const { return type_ == Type::Number ? num_ : def; }
  int as_int(int def = 0) const { return type_ == Type::Number ? int(num_) : def; }
  std::string as_str(const std::string& def = "") const { return type_ == Type::String ? str_ : def; }

  // Object access. The mutable form turns a null into an object.
  Json& operator[](const std::string& key);
  const Json& get(const std::string& key) const;
  bool has(const std::string& key) const;

  Array& arr();
  const Array& arr() const;
  Object& obj();
  const Object& obj() const;
  void push(Json v) { arr().push_back(std::move(v)); }
  size_t size() const;

  std::vector<double> num_vector() const;
  std::vector<std::string> str_vector() const;

  std::string dump(int indent = -1) const;
  static Json parse(const std::string& text);  // throws std::runtime_error

private:
  void dump_to(std::string& out, int indent, int depth) const;

  Type type_ = Type::Null;
  bool bool_ = false;
  double num_ = 0;
  std::string str_;
  Array arr_;
  Object obj_;
};

}  // namespace rc
