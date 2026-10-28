#pragma once

// HTTPS to the Pi, handed to the test driver: the request goes out on stdout as
// "http <hex body>" and the driver answers "httpresp <status> <unix time> <hex body>".

#include <string>

namespace sim {
// Implemented in the node program.
int httpExchange(const std::string& body, std::string& answer, std::string& date);
}

namespace freeink {
class SecureHttpClient {
 public:
  void setCACert(const char*) {}
  void setTimeout(unsigned) {}
  void setReuse(bool) {}
  bool begin(const std::string&) { return true; }
  void addHeader(const std::string&, const std::string&) {}
  int POST(const std::string& body) { return sim::httpExchange(body, answer_, date_); }
  const std::string& getString() const { return answer_; }
  std::string getHeader(const std::string& name) const { return name == "Date" ? date_ : std::string(); }

 private:
  std::string answer_;
  std::string date_;
};
}  // namespace freeink
